#include "hook.h"
#include "fiber.h"
#include "sylar/config.h"
#include "sylar/fd_manager.h"
#include "sylar/iomanager.h"
#include "sylar/log.h"
#include "sylar/scheduler.h"
#include <dlfcn.h>
#include <fcntl.h>

#include <functional>
#include <memory>
#include <sys/ioctl.h>
#include <sys/socket.h>

sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");
namespace sylar
{
// 线程局部变量，用于记录当前线程是否开启了hook
static thread_local bool t_hook_enable = false;

static sylar::ConfigVar<int>::ptr g_tcp_connect_timeout =
    sylar::Config::Lookup("tcp.connect.timeout", 5000, "tcp connect timeout");
#define HOOK_FUN(XX)                                                                               \
    XX(sleep)                                                                                      \
    XX(usleep)                                                                                     \
    XX(nanosleep)                                                                                  \
    XX(socket)                                                                                     \
    XX(connect)                                                                                    \
    XX(accept)                                                                                     \
    XX(read)                                                                                       \
    XX(readv)                                                                                      \
    XX(recv)                                                                                       \
    XX(recvfrom)                                                                                   \
    XX(recvmsg)                                                                                    \
    XX(write)                                                                                      \
    XX(writev)                                                                                     \
    XX(send)                                                                                       \
    XX(sendto)                                                                                     \
    XX(sendmsg)                                                                                    \
    XX(close)                                                                                      \
    XX(fcntl)                                                                                      \
    XX(ioctl)                                                                                      \
    XX(getsockopt)                                                                                 \
    XX(setsockopt)

// hook初始化函数，替换系统的原始函数指针
void hook_init()
{
    static bool is_inited = false;
    if (is_inited)
    {
        return;
    }

// 这里其实就相当于给原来的系统调用起了别名，现在的sleep_f 就是原来的系统调用sleep
#define XX(name) name##_f = (name##_fun)dlsym(RTLD_NEXT, #name);
    HOOK_FUN(XX);
#undef XX
}

static uint64_t s_connect_timeout = -1;
//主要是为了在main函数之前就执行（写成全局变量的static）
struct _HookIniter
{
    _HookIniter()
    {
        hook_init();
        s_connect_timeout = g_tcp_connect_timeout->getValue();
        g_tcp_connect_timeout->addListener(
            [](const int &old_value, const int &new_value)
            {
                SYLAR_LOG_INFO(g_logger)
                    << "tcp connect timeout changed from " << old_value << " to " << new_value;
                s_connect_timeout = new_value;
            });
    }
};

static _HookIniter s_hook_initer;

bool is_hook_enable()
{
    return t_hook_enable;
}
void set_hook_enable(bool flag)
{
    t_hook_enable = flag;
}
} // namespace sylar

struct timer_info
{
    int cancelled = 0;
};

template <typename OriginFun, typename... Args>
static ssize_t do_io(int fd,
                     OriginFun fun,
                     const char *hook_fun_name,
                     uint32_t event,
                     int timeout_so,
                     Args &&...args)
{
    // 1、如果线程没有开启hook，就运行系统调用
    if (!sylar::t_hook_enable)
    {
        return fun(fd, std::forward<Args>(args)...);
    }
    // 2、判断是不是对socket执行方法，不是就运行系统调用
    sylar::FdCtx::ptr ctx = sylar::FdMgr::GetInstance()->get(fd);
    if (!ctx)
    {
        return fun(fd, std::forward<Args>(args)...);
    }
    // 3、是socket，但是仍然不能继续往下走的情况

    // 对应的fd关闭了
    if (ctx->isClose())
    {
        errno = EBADF;
        return -1;
    }
    // 如果不是socket，或者用户设置了非阻塞，就运行系统调用
    if (!ctx->isSocket() || ctx->getUserNonblock())
    {
        return fun(fd, std::forward<Args>(args)...);
    }
    // 4、执行socket操作
    uint64_t to = ctx->getTimeout(timeout_so);
    std::shared_ptr<timer_info> tinfo(new timer_info());
    // 4.1 先使用系统调用直接执行，因为大多数socket其实是准备好了的，直接系统调用最快，
    // 你注册进epoll反而是资源浪费，影响性能
    ssize_t n = fun(fd, std::forward<Args>(args)...);
    while (n == -1 && errno == EINTR)
    {
        n = fun(fd, std::forward<Args>(args)...);
    }
    // 4.2 如果系统调用返回EAGAIN，说明socket还没有准备好，
    // 这时候就需要注册到epoll，等待事件发生，交给调度器管理去了
retry:
    if (n == -1 && (errno == EAGAIN))
    {
        sylar::IOManager *iom = sylar::IOManager::GetThis();
        sylar::Timer::ptr timer;
        std::weak_ptr<timer_info> winfo(tinfo);
        // uint64_t是无符号整数，-1 强转 代表的是最大值
        //这里就表示的超时怎么办（协程设置为超时，正常返回错误）
        if (to != (uint64_t)-1)
        {
            timer = iom->addConditionTimer(
                to,
                [winfo, fd, iom, event]()
                {
                    auto t = winfo.lock();
                    if (!t || t->cancelled)
                    {
                        return;
                    }
                    t->cancelled = ETIMEDOUT;
                    iom->cancelEvent(fd, (sylar::IOManager::Event)event);
                },
                winfo);
        }
        //正常是返回0，错误返回-1
        int rt = iom->addEvent(fd, (sylar::IOManager::Event)(event));
        //发生错误才执行后面的
        if (rt)
        {
            SYLAR_LOG_ERROR(g_logger)
                << hook_fun_name << " addEvent(" << fd << ", " << event << ")";
            if (timer)
            {
                timer->cancel();
            }
            return -1;
        }
        else
        {
            sylar::Fiber::YieldToHold();
            if (timer)
            {
                timer->cancel();
            }
            if (tinfo->cancelled)
            {
                errno = tinfo->cancelled;
                return -1;
            }
            goto retry;
        }
    }
    return n;
}

extern "C"
{

    //展开类似于这样，定义一个sleep_f用于接收系统的原始sleep函数指针
    // sleep_fun sleep_f = nullptr;
#define XX(name) name##_fun name##_f = nullptr;
    HOOK_FUN(XX);
#undef XX
}

// sleep
unsigned int sleep(unsigned int seconds)
{
    // 如果当前线程没有开启hook，就直接调用系统的sleep函数
    if (!sylar::t_hook_enable)
    {
        return sleep_f(seconds);
    }
    // 如果当前线程开启了hook，就调用我们自己实现的sleep逻辑
    sylar::Fiber::ptr fiber = sylar::Fiber::GetThis();
    sylar::IOManager *iom = sylar::IOManager::GetThis();
    iom->addTimer(seconds * 1000,
                  std::bind((void(sylar::Scheduler::*)(sylar::Fiber::ptr, int thread)) &
                                sylar::IOManager::schedule,
                            iom, fiber, -1));
    // 切换到其他协程，等待定时器到期
    sylar::Fiber::YieldToHold();
    return 0;
}

// usleep
int usleep(useconds_t usec)
{
    if (!sylar::t_hook_enable)
    {
        return usleep_f(usec);
    }
    sylar::Fiber::ptr fiber = sylar::Fiber::GetThis();
    sylar::IOManager *iom = sylar::IOManager::GetThis();
    iom->addTimer(usec / 1000,
                  std::bind((void(sylar::Scheduler::*)(sylar::Fiber::ptr, int thread)) &
                                sylar::IOManager::schedule,
                            iom, fiber, -1));
    // 切换到其他协程，等待定时器到期
    sylar::Fiber::YieldToHold();
    return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (!sylar::t_hook_enable)
    {
        return nanosleep_f(req, rem);
    }
    sylar::Fiber::ptr fiber = sylar::Fiber::GetThis();
    sylar::IOManager *iom = sylar::IOManager::GetThis();
    iom->addTimer(req->tv_sec * 1000 + req->tv_nsec / 1000 / 1000,
                  [iom, fiber]() { iom->schedule(fiber); });
    // 切换到其他协程，等待定时器到期
    sylar::Fiber::YieldToHold();
    return 0;
}

// socket
int socket(int domain, int type, int protocol)
{
    if (!sylar::t_hook_enable)
    {
        return socket_f(domain, type, protocol);
    }
    //调用完系统的socket函数，加到
    int fd = socket_f(domain, type, protocol);
    if (fd == -1)
    {
        return fd;
    }
    sylar::FdMgr::GetInstance()->get(fd, true);
    return fd;
}

int connect_with_timeout(int fd,
                         const struct sockaddr *addr,
                         socklen_t addrlen,
                         uint64_t timeout_ms)
{
    if (!sylar::t_hook_enable)
    {
        return connect_f(fd, addr, addrlen);
    }
    sylar::FdCtx::ptr ctx = sylar::FdMgr::GetInstance()->get(fd);
    if (!ctx || ctx->isClose())
    {
        errno = EBADF;
        return -1;
    }

    if (!ctx->isSocket())
    {
        return connect_f(fd, addr, addrlen);
    }

    if (ctx->getUserNonblock())
    {
        return connect_f(fd, addr, addrlen);
    }

    int n = connect_f(fd, addr, addrlen);
    if (n == 0)
    {
        return 0;
    }
    // EINPROGRESS 连接正在建立（正常情况）
    else if (n != -1 || errno != EINPROGRESS)
    {
        return n;
    }
    auto iom = sylar::IOManager::GetThis();
    sylar::Timer::ptr timer;
    std::shared_ptr<timer_info> tinfo(new timer_info);
    std::weak_ptr<timer_info> winfo(tinfo);
    if (timeout_ms != (uint64_t)-1)
    {
        timer = iom->addConditionTimer(
            timeout_ms,
            [winfo, fd, iom]()
            {
                auto t = winfo.lock();
                if (!t || t->cancelled)
                {
                    return;
                }
                t->cancelled = ETIMEDOUT;
                iom->cancelEvent(fd, sylar::IOManager::WRITE);
            },
            winfo);
    }
    int rt = iom->addEvent(fd, sylar::IOManager::WRITE);
    if (rt == 0)
    {
        sylar::Fiber::YieldToHold();
        //后面的代码其实就代表着协程恢复之后，也就是说connect已经连接成功了,也就不用timer了
        if (timer)
        {
            timer->cancel();
        }
        if (tinfo->cancelled)
        {
            errno = tinfo->cancelled;
            return -1;
        }
    }
    else
    {
        if (timer)
        {
            timer->cancel();
        }
        SYLAR_LOG_ERROR(g_logger) << "connect addEvent(" << fd << ", WRITE) error";
    }
    // epoll返回wirte只能代表事件状态发生变化，不能说明真的连接成功，所以还得正式查看一下
    int error = 0;
    socklen_t len = sizeof(int);
    // getsockopt == -1代表 getsockopt这个失败
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == -1)
    {
        return -1;
    }
    if (!error)
    {
        return 0;
    }
    //这里代表的是connect失败
    else
    {
        errno = error;
        return -1;
    }
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    return connect_with_timeout(sockfd, addr, addrlen, sylar::s_connect_timeout);
}
int accept(int s, struct sockaddr *addr, socklen_t *addrlen)
{
    int fd = do_io(s, accept_f, "accept", sylar::IOManager::READ, SO_RCVTIMEO, addr, addrlen);
    if (fd >= 0)
    {
        sylar::FdMgr::GetInstance()->get(fd, true);
    }
    return fd;
}

// read
ssize_t read(int fd, void *buf, size_t count)
{
    return do_io(fd, read_f, "read", sylar::IOManager::READ, SO_RCVTIMEO, buf, count);
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
    return do_io(fd, readv_f, "readv", sylar::IOManager::READ, SO_RCVTIMEO, iov, iovcnt);
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
    return do_io(sockfd, recv_f, "recv", sylar::IOManager::READ, SO_RCVTIMEO, buf, len, flags);
}

ssize_t recvfrom(
    int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
{
    return do_io(sockfd, recvfrom_f, "recvfrom", sylar::IOManager::READ, SO_RCVTIMEO, buf, len,
                 flags, src_addr, addrlen);
}

ssize_t recvmsg(int sockfd, struct mqsghdr *msg, int flags)
{
    return do_io(sockfd, recvmsg_f, "recvmsg", sylar::IOManager::READ, SO_RCVTIMEO, msg, flags);
}

// write
ssize_t write(int fd, const void *buf, size_t count)
{
    return do_io(fd, write_f, "write", sylar::IOManager::WRITE, SO_SNDTIMEO, buf, count);
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    return do_io(fd, writev_f, "writev", sylar::IOManager::WRITE, SO_SNDTIMEO, iov, iovcnt);
}

ssize_t send(int s, const void *msg, size_t len, int flags)
{
    return do_io(s, send_f, "send", sylar::IOManager::WRITE, SO_SNDTIMEO, msg, len, flags);
}

ssize_t
sendto(int s, const void *msg, size_t len, int flags, const struct sockaddr *to, socklen_t tolen)
{
    return do_io(s, sendto_f, "sendto", sylar::IOManager::WRITE, SO_SNDTIMEO, msg, len, flags, to,
                 tolen);
}

ssize_t sendmsg(int s, const struct msghdr *msg, int flags)
{
    return do_io(s, sendmsg_f, "sendmsg", sylar::IOManager::WRITE, SO_SNDTIMEO, msg, flags);
}

int close(int fd)
{
    if (!sylar::t_hook_enable)
    {
        return close_f(fd);
    }
    sylar::FdCtx::ptr ctx = sylar::FdMgr::GetInstance()->get(fd);
    if (ctx)
    {
        sylar::IOManager *iom = sylar::IOManager::GetThis();
        if (iom)
        {
            iom->cancelAll(fd);
        }
        sylar::FdMgr::GetInstance()->del(fd);
    }
    return close_f(fd);
}

// 其他相关系统调用
int fcntl(int fd, int cmd, ... /* arg */)
{
    va_list va;
    va_start(va, cmd);
    switch (cmd)
    {
    case F_SETFL:

    {
        int arg = va_arg(va, int);
        va_end(va);
        auto ctx = sylar::FdMgr::GetInstance()->get(fd);
        if (!ctx || ctx->isClose() || !ctx->isSocket())
        {
            return fcntl_f(fd, cmd, arg);
        }
        ctx->setUserNonblock(arg & O_NONBLOCK);
        if (ctx->getSysNonblock())
        {
            arg |= O_NONBLOCK;
        }
        else
        {
            arg &= ~O_NONBLOCK;
        }
        return fcntl_f(fd, cmd, arg);
    }
    break;
    case F_GETFL:
    {

        va_end(va);
        int arg = fcntl_f(fd, cmd);
        sylar::FdCtx::ptr ctx = sylar::FdMgr::GetInstance()->get(fd);
        if (!ctx || ctx->isClose() || !ctx->isSocket())
        {
            return arg;
        }
        if (ctx->getSysNonblock())
        {
            return arg | O_NONBLOCK;
        }
        else
        {
            return arg & ~O_NONBLOCK;
        }
    }
    break;
        //命令为一个int的
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
    case F_SETFD:
    case F_GETFD:
    case F_SETOWN:
    case F_SETSIG:
    case F_SETLEASE:
    case F_NOTIFY:
    case F_SETPIPE_SZ:
    {
        int arg = va_arg(va, int);
        va_end(va);
        return fcntl_f(fd, cmd, arg);
    }
    break;
        //不带额外命令的
    case F_GETOWN:
    case F_GETSIG:
    case F_GETLEASE:
    case F_GETPIPE_SZ:
    {
        va_end(va);
        return fcntl_f(fd, cmd);
    }
    break;
        //命令为flock
    case F_SETLK:
    case F_SETLKW:
    case F_GETLK:
    {
        struct flock *arg = va_arg(va, struct flock *);
        va_end(va);
        return fcntl_f(fd, cmd, arg);
    }
    break;
        //命令为f_owner_exlock的
    case F_GETOWN_EX:
    case F_SETOWN_EX:
    {
        struct f_owner_exlock *arg = va_arg(va, struct f_owner_exlock *);
        va_end(va);
        return fcntl_f(fd, cmd, arg);
    }
    break;
    default:
        va_end(va);
        return fcntl_f(fd, cmd);
    }
}

int ioctl(int d, unsigned long int request, ...)
{
    va_list va;
    va_start(va, request);
    void *arg = va_arg(va, void *);
    va_end(va);
    if (request == FIONBIO)
    {
        bool user_nonblock = !!*(int *)arg;
        auto ctx = sylar::FdMgr::GetInstance()->get(d);

        if (!ctx || ctx->isClose() || !ctx->isSocket())
        {
            return ioctl_f(d, request, arg);
        }
        ctx->setUserNonblock(user_nonblock);
    }
    return ioctl_f(d, request, arg);
}

int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen)
{
    return getsockopt_f(sockfd, level, optname, optval, optlen);
}

int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen)
{
    if (!sylar::t_hook_enable)
    {
        return setsockopt_f(sockfd, level, optname, optval, optlen);
    }
    if (level == SOL_SOCKET)
    {
        if (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO)
        {
            auto ctx = sylar::FdMgr::GetInstance()->get(sockfd);
            if (ctx)
            {
                const timeval *v = (const timeval *)optval;
                ctx->setTimeout(optname, v->tv_sec * 1000 + v->tv_usec / 1000);
            }
        }
    }
    return setsockopt_f(sockfd, level, optname, optval, optlen);
}
