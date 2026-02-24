#ifndef __SYLAR_SCHEDULER_H__
#define __SYLAR_SCHEDULER_H__
#include "fiber.h"
#include "thread.h"
#include <cstddef>
#include <functional>
#include <list>
#include <vector>

namespace sylar
{
class Scheduler
{
public:
    typedef std::shared_ptr<Scheduler> ptr;
    typedef Mutex MutexType;
    Scheduler(size_t threads = 1, bool user_caller = true, const std::string &name = "main");
    virtual ~Scheduler();
    const std::string &getName() const
    {
        return m_name;
    }
    //协程调度方法，传入一个协程指针，或者一个回调函数，指定由哪一个线程去执行
    //-1 表示 “任意空闲线程”；
    //单个任务调度
    template <class FiberOrCb>
    void schedule(FiberOrCb fc, int thread = -1)
    {
        bool need_tickle = false;
        {
            MutexType::Lock lock(m_mutex);
            need_tickle = scheduleNoLock(fc, thread);
        }
        if (need_tickle)
        {
            tickle();
        }
    }

    //批量任务调度(一次性完成这些任务)
    template <class InputIterator>
    void schedule(InputIterator begin, InputIterator end, int thread = -1)
    {
        bool need_tickle = false;
        {
            MutexType::Lock lock(m_mutex);
            while (begin != end)
            {
                need_tickle = scheduleNoLock(*begin, thread) || need_tickle;
                begin++;
            }
        }
        if (need_tickle)
        {
            tickle();
        }
    }

    static Scheduler *GetThis();
    static Fiber *GetMainFiber();

    void start();
    void stop();

    virtual bool stopping();

protected:
    //唤醒线程，调用 tickle() 通知调度器的工作线程 “有新任务了”。
    void tickle();
    //休眠
    virtual void idle();
    void run();

    void setThis();

protected:
    std::vector<int> m_threadIds;
    //这是代表需要额外创建的线程个数，并非现有的线程个数
    size_t m_threadCount = 0;
    //创建调度器的线程id
    int m_rootThread = 0;
    //是否已经可以停止了
    bool m_stopping = true;
    //作用不明的变量。。。。
    bool m_autoStop = false;
    //正在运行的线程数量
    std::atomic<size_t> m_activeThreadCount = {0};
    std::atomic<size_t> m_idleThreadCount = {0};

private:
    //用于绑定
    struct FiberAndThread
    {
        Fiber::ptr fiber;
        std::function<void()> cb;
        int thread;

        FiberAndThread(Fiber::ptr f, int thr) : fiber(f), thread(thr)
        {
        }

        //批量操作的时候迭代器传来的是指针
        //清楚原f指向的协程对象，避免重复操作
        FiberAndThread(Fiber::ptr *f, int thr) : thread(thr)
        {
            fiber.swap(*f);
        }

        FiberAndThread(std::function<void()> f, int thr) : cb(f), thread(thr)
        {
        }

        FiberAndThread(std::function<void()> *f, int thr) : thread(thr)
        {
            cb.swap(*f);
        }
        FiberAndThread() : thread(-1)
        {
        }
        void reset()
        {
            fiber = nullptr;
            cb = nullptr;
            thread = -1;
        }
    };

private:
    template <class FiberOrCb>
    bool scheduleNoLock(FiberOrCb fc, int thread)
    {
        bool need_tickle = m_fibers.empty();
        FiberAndThread ft(fc, thread);
        if (ft.fiber || ft.cb)
        {
            m_fibers.push_back(ft);
        }
        return need_tickle;
    }

private:
    MutexType m_mutex;
    std::string m_name;
    //存放可以运行的线程
    std::vector<Thread::ptr> m_threads;
    //存放的是协程以及它对应的线程id
    std::list<FiberAndThread> m_fibers;
    //存放调度器线程的主协程
    Fiber::ptr m_rootFiber;
};

} // namespace sylar
#endif