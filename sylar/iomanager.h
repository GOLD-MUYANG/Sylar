#ifndef __SYLAR_IOMANAGER_H__
#define __SYLAR_IOMANAGER_H__
#include "scheduler.h"
#include "sylar/thread.h"
#include "timer.h"
#include <cstddef>
#include <memory>
#include <vector>

namespace sylar
{
class IOManager : public Scheduler, public TimerManager
{
public:
    typedef std::shared_ptr<IOManager> ptr;
    typedef RWMutex RWMutexType;

    enum Event
    {
        NONE = 0x0,
        READ = 0x1,  // EPOLLIN
        WRITE = 0x4, // EPOLLOUT
    };

    IOManager(size_t threads = 1, bool use_caller = true, const std::string &name = "");
    ~IOManager();

    // 0 success , -1 fail
    int addEvent(int fd, Event event, std::function<void()> cb = nullptr);
    // del一个fd的一个event
    bool delEvent(int fd, Event event);
    //给一个fd的一个event强制触发了回调之后，不再继续监控这个fd
    bool cancelEvent(int fd, Event event);
    //取消一个fd上的所有事件
    bool cancelAll(int fd);

    static IOManager *GetThis();

protected:
    //
    void tickle() override;
    bool stopping() override;
    //等待被阻塞的IO事件完成，并执行回调
    void idle() override;

    void contextResize(size_t size);

    void onTimerInsertedAtFront() override;
    bool stopping(uint64_t &timeout);

private:
    //将fd和回调进行绑定，一旦fd上有事件发生，就会调用对应的回调函数
    struct FdContext
    {
        typedef Mutex MutexType;
        // 事件上下文,类似于scheduler的FiberAndThread
        struct EventContext
        {
            Scheduler *scheduler = nullptr;
            Fiber::ptr fiber;
            std::function<void()> cb;
        };

        EventContext &getContext(Event event); // 获取事件上下文
        void resetContext(EventContext &ctx);  // 重置事件上下文
        void triggerEvent(Event event);        // 触发事件

        EventContext read;   // 读事件上下文
        EventContext write;  // 写事件上下文
        int fd = 0;          // 事件关联的句柄
        Event events = NONE; //已经注册的事件
        MutexType mutex;
    };

    int m_epfd = 0;
    int m_tickleFds[2];
    // 等待事件的数量
    std::atomic<size_t> m_pendingEventCount = {0};
    RWMutex m_mutex;
    // 文件描述符上下文容器
    std::vector<FdContext *> m_fdContexts;
};
} // namespace sylar
#endif