#include "iomanager.h"
#include "sylar/log.h"
#include "sylar/macro.h"
#include "sylar/thread.h"
#include "sylar/timer.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <sys/epoll.h>
#include <unistd.h>
namespace sylar
{
static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

IOManager::FdContext::EventContext &IOManager::FdContext::getContext(IOManager::Event event)
{
    switch (event)
    {
    case READ:
        return read;
    case WRITE:
        return write;
    default:
        SYLAR_ASSERT2(false, "getContext");
    }
}
void IOManager::FdContext::resetContext(EventContext &ctx)
{
    ctx.scheduler = nullptr;
    ctx.fiber.reset();
    ctx.cb = nullptr;
}
void IOManager::FdContext::triggerEvent(Event event)
{
    SYLAR_ASSERT(events & event);
    events = (Event)(events & ~event);
    EventContext &ctx = getContext(event);
    if (ctx.cb)
    {
        ctx.scheduler->schedule(&ctx.cb);
    }
    else
    {
        ctx.scheduler->schedule(&ctx.fiber);
    }
    ctx.scheduler = nullptr;
    return;
}

IOManager::IOManager(size_t threads, bool use_caller, const std::string &name)
    : Scheduler(threads, use_caller, name)
{
    m_epfd = epoll_create(5000);
    SYLAR_ASSERT(m_epfd > 0);

    //创建管道，m_tickleFds[0]是读端，m_tickleFds[1]是写端,用于进程间的唤醒
    //当需要唤醒阻塞在 epoll_wait 的 IO 线程时，向管道写端（m_tickleFds [1]）写入一个字节，
    // epoll 检测到管道读端的读事件，线程就会从 epoll_wait 中返回；
    int rt = pipe(m_tickleFds);
    SYLAR_ASSERT(!rt);

    //初始化epoll_event结构体，用于描述要监听的事件 epoll_event event;
    epoll_event event;
    memset(&event, 0, sizeof(epoll_event));
    event.events = EPOLLIN | EPOLLET;
    event.data.fd = m_tickleFds[0];

    //将管道读端设置为非阻塞模式，避免读操作阻塞线程
    rt = fcntl(m_tickleFds[0], F_SETFL, O_NONBLOCK);
    SYLAR_ASSERT(!rt);

    //将管道读端注册到epoll实例中，监听其读事件
    rt = epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_tickleFds[0], &event);
    SYLAR_ASSERT(!rt);
    contextResize(32);
    start();
}

IOManager::~IOManager()
{
    stop();
    close(m_epfd);
    close(m_tickleFds[0]);
    close(m_tickleFds[1]);
    for (size_t i = 0; i < m_fdContexts.size(); i++)
    {
        if (m_fdContexts[i])
        {
            delete m_fdContexts[i];
        }
    }
}

// 上下文容器扩容,并且初始化
void IOManager::contextResize(size_t size)
{
    m_fdContexts.resize(size);
    for (size_t i = 0; i < m_fdContexts.size(); i++)
    {
        if (!m_fdContexts[i])
        {
            m_fdContexts[i] = new FdContext;
            m_fdContexts[i]->fd = i;
        }
    }
}

//对IOManager管理的一些fd添加监控，当发生某些事情，就会调用对应的回调函数
int IOManager::addEvent(int fd, Event event, std::function<void()> cb)
{
    FdContext *fd_ctx = nullptr;
    RWMutexType::ReadLock lock(m_mutex);
    // 1、拿到被管理的fd
    if ((int)m_fdContexts.size() > fd)
    {
        fd_ctx = m_fdContexts[fd];
        lock.unlock();
    }
    else
    {
        lock.unlock();
        RWMutexType::WriteLock lock2(m_mutex);
        contextResize(fd * 1.5);
        fd_ctx = m_fdContexts[fd];
    }
    // 2、查看fd上是否已经有了相同的想要被监控的事件
    FdContext::MutexType::Lock lock2(fd_ctx->mutex);
    if (fd_ctx->events & event)
    {
        SYLAR_LOG_ERROR(g_logger) << "addEvent assert fd=" << fd << " event=" << event
                                  << " fd_ctx.event=" << fd_ctx->events;
        SYLAR_ASSERT(!(fd_ctx->events & event));
    }

    // 3、如果没有，就设置好，然后被epoll监听
    // 3.1
    // 原来有没有事件，如果有并且新加的不是和以前一样的，那么epoll要做的操作就是修改，如果原来没有，那么epoll要做的，就是新增一个监控
    int op = fd_ctx->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    epoll_event epevent;
    epevent.events = EPOLLET | fd_ctx->events | event;

    // 3.2要取的时候，取到这个，里面不光有监控的fd，还有要执行的方法
    epevent.data.ptr = fd_ctx;
    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt)
    {
        SYLAR_LOG_ERROR(g_logger) << "epoll_ctl(" << m_epfd << ", " << op << "," << fd << ","
                                  << epevent.events << "):" << rt << " (" << errno << ") ("
                                  << strerror(errno) << ")";
        return -1;
    }
    m_pendingEventCount++;
    // 4、接着设置事件发生后要执行的回调函数
    //注册了什么事件也要保存一份
    fd_ctx->events = (Event)(fd_ctx->events | event);
    FdContext::EventContext &event_ctx = fd_ctx->getContext(event);
    SYLAR_ASSERT(!event_ctx.scheduler && !event_ctx.fiber && !event_ctx.cb);
    event_ctx.scheduler = Scheduler::GetThis();
    if (cb)
    {
        event_ctx.cb.swap(cb);
    }
    else
    {
        event_ctx.fiber = Fiber::GetThis();
        SYLAR_ASSERT(event_ctx.fiber->getState() == Fiber::EXEC);
    }
    return 0;
}
bool IOManager::delEvent(int fd, Event event)
{
    RWMutexType::ReadLock lock(m_mutex);
    //对哪个fd进行监控，拿到对应的fd_ctx
    if ((int)m_fdContexts.size() <= fd)
    {
        return false;
    }
    FdContext *fd_ctx = m_fdContexts[fd];
    lock.unlock();
    FdContext::MutexType::Lock lock2(fd_ctx->mutex);
    //看执行操作后EPOLL是修改还是删除
    if (!(fd_ctx->events & event))
    {
        return false;
    }
    // events应该是一个 & ~event，代表的就是位置成0
    fd_ctx->events = (Event)(fd_ctx->events & ~event);
    //交给epool管理
    int op = fd_ctx->events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events = EPOLLET | fd_ctx->events;
    epevent.data.ptr = fd_ctx;
    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt)
    {
        SYLAR_LOG_ERROR(g_logger) << "epoll_ctl(" << m_epfd << ", " << op << "," << fd << ","
                                  << epevent.events << "):" << rt << " (" << errno << ") ("
                                  << strerror(errno) << ")";
        return false;
    }
    m_pendingEventCount--;
    fd_ctx->events = (Event)(fd_ctx->events & ~event);
    //没有事件，删除cb
    FdContext::EventContext &event_ctx = fd_ctx->getContext(event);
    fd_ctx->resetContext(event_ctx);
    return true;
}

// 取消事件,不过会把时间先触发一下
bool IOManager::cancelEvent(int fd, Event event)
{
    RWMutexType::ReadLock lock(m_mutex);
    if ((int)m_fdContexts.size() <= fd)
    {
        return false;
    }
    FdContext *fd_ctx = m_fdContexts[fd];
    lock.unlock();

    FdContext::MutexType::Lock lock2(fd_ctx->mutex);
    if (!(fd_ctx->events & event))
    {
        return false;
    }

    Event new_events = (Event)(fd_ctx->events & ~event);
    int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events = EPOLLET | new_events;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt)
    {
        SYLAR_LOG_ERROR(g_logger) << "epoll_ctl(" << m_epfd << ", " << op << "," << fd << ","
                                  << epevent.events << "):" << rt << " (" << errno << ") ("
                                  << strerror(errno) << ")";
        return false;
    }

    fd_ctx->triggerEvent(event);
    --m_pendingEventCount;
    return true;
}

// 取消所有事件
bool IOManager::cancelAll(int fd)
{
    RWMutexType::ReadLock lock(m_mutex);
    if ((int)m_fdContexts.size() <= fd)
    {
        return false;
    }
    FdContext *fd_ctx = m_fdContexts[fd];
    lock.unlock();

    FdContext::MutexType::Lock lock2(fd_ctx->mutex);
    if (!fd_ctx->events)
    {
        return false;
    }
    int op = EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events = 0;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt)
    {
        SYLAR_LOG_ERROR(g_logger) << "epoll_ctl(" << m_epfd << ", " << op << "," << fd << ","
                                  << epevent.events << "):" << rt << " (" << errno << ") ("
                                  << strerror(errno) << ")";
        return false;
    }
    if (fd_ctx->events & Event::READ)
    {
        fd_ctx->triggerEvent(Event::READ);
    }
    if (fd_ctx->events & Event::WRITE)
    {
        fd_ctx->triggerEvent(Event::WRITE);
    }
    SYLAR_ASSERT(fd_ctx->events == 0);
    return true;
}

IOManager *IOManager::GetThis()
{
    return dynamic_cast<IOManager *>(Scheduler::GetThis());
}

//向管道写入一个字节，通知epoll_wait有事件发生
//因为新加入的协程任务并不是fd事件，epoll不会处理，主动tickle一下，
// ，继续执行 idle() 剩余代码，随后会swapout，或者说从idle返回去执行协程任务就是tickle的作用
void IOManager::tickle()
{
    //当所有的阻塞IO都完成后，才会真正调用tickle，去处理未完成的事件
    if (!hasIdleThreads())
    {
        return;
    }
    int rt = write(m_tickleFds[1], "T", 1);
    SYLAR_ASSERT(rt == 1);
}
bool IOManager::stopping()
{
    uint64_t timeout = 0;

    return stopping(timeout);
}

bool IOManager::stopping(uint64_t &timeout)
{
    timeout = getNextTimer();
    return timeout == ~0ull && m_pendingEventCount == 0 && Scheduler::stopping();
}

//如果任务队列里没有可以直接执行的任务或者说协程，才会调用idle方法
void IOManager::idle()
{
    epoll_event *events = new epoll_event[64];
    std::shared_ptr<epoll_event> shared_evnets(events, [](epoll_event *ptr) { delete[] ptr; });

    //一直等待IO事件发生并处理
    while (true)
    {
        //直到所有任务都处理完，所有线程也退出
        uint64_t next_timeout = 0;
        //把next_timeout穿进去，next_timeout的是一个引用参数，会被修改
        if (stopping(next_timeout))
        {
            SYLAR_LOG_INFO(g_logger) << "name=" << getName() << " idle stopping exit";
            break;
        }

        //持续监听事件
        int rt = 0;
        do
        {
            static const int MAX_TIMEOUT = 3000;
            if (next_timeout != ~0ull)
            {
                next_timeout = (int)next_timeout > MAX_TIMEOUT ? MAX_TIMEOUT : next_timeout;
            }
            else
            {
                next_timeout = MAX_TIMEOUT;
            }
            rt = epoll_wait(m_epfd, events, 64, (int)next_timeout);
            // SYLAR_LOG_INFO(g_logger) << "epoll_wait rt=" << rt;
            if (rt == -1 && errno == EINTR)
            {
            }
            //如果真的有事件了，就退出监听，然后去处理数据
            else
            {
                break;
            }
        } while (true);

        std::vector<std::function<void()>> cbs;
        listExpiredCb(cbs);
        if (!cbs.empty())
        {
            // SYLAR_LOG_DEBUG(g_logger) << "on timer cbs.size=" << cbs.size();
            schedule(cbs.begin(), cbs.end());
            cbs.clear();
        }
        for (int i = 0; i < rt; i++)
        {
            //如果是tickle发来的,因为现在idle等待IO事件中，线程阻塞在这里，
            //现在来一个新的任务，需要去处理，所以要让线程继续运行，去处理那个事件
            epoll_event &event = events[i];
            if (event.data.fd == m_tickleFds[0])
            {
                uint8_t dummy;
                while (read(m_tickleFds[0], &dummy, 1) == 1)
                    ;
                continue;
            }
            //如果是正常的IO读写事件
            FdContext *fd_ctx = (FdContext *)event.data.ptr;
            FdContext::MutexType::Lock lock(fd_ctx->mutex);
            //如果发生了错误或者挂起，也是需要处理的，触发用户回调，以便清理资源或关闭连接
            if (event.events & (EPOLLERR | EPOLLHUP))
            {
                event.events |= EPOLLIN | EPOLLOUT;
            }

            int real_events = NONE;
            if (event.events & EPOLLIN)
            {
                real_events |= READ;
            }
            if (event.events & EPOLLOUT)
            {
                real_events |= WRITE;
            }

            //如果没来实际的IO读写事件，那么不做处理
            real_events &= fd_ctx->events;

            if (real_events == NONE)
            {
                continue;
            }
            //处理完读写，还剩下的事件，继续加到epoll里去监听
            int left_events = fd_ctx->events & (~real_events);
            int op = left_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
            event.events = EPOLLET | left_events;
            int rt2 = epoll_ctl(m_epfd, op, fd_ctx->fd, &event);
            if (rt2)
            {
                SYLAR_LOG_ERROR(g_logger) << "epoll_ctl(" << m_epfd << ", " << op << ","
                                          << fd_ctx->fd << "," << event.events << "):" << rt2
                                          << " (" << errno << ") (" << strerror(errno) << ")";
                continue;
            }

            //的确有读写事件，就触发事件（直接加到调度器里）
            if (real_events & READ)
            {
                fd_ctx->triggerEvent(Event::READ);
                m_pendingEventCount--;
            }
            if (real_events & WRITE)
            {
                fd_ctx->triggerEvent(Event::WRITE);
                m_pendingEventCount--;
            }
        }

        Fiber::ptr cur = Fiber::GetThis();
        auto raw_ptr = cur.get();
        cur.reset();
        raw_ptr->swapOut();
    }
}

void IOManager::onTimerInsertedAtFront()
{
    tickle();
}
} // namespace sylar