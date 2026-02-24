
#include "scheduler.h"
#include "log.h"
#include "macro.h"
#include "sylar/fiber.h"
#include "thread.h"
#include <cstddef>
#include <functional>
namespace sylar
{

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

//线程被哪个调度器所管理
static thread_local Scheduler *t_scheduler = nullptr;
//线程的主协程(只记录非调度器线程的主
// 协程，调度器线程的协程是m_rootFiber)
//当来一个任务时，要切换上下文了，记录好现在的协程，等任务执行完了，再切换回来
//之所以要存，是因为可以理解为任务队列里的协程随用随释放，根本不用回去
static thread_local Fiber *t_fiber = nullptr;

void Scheduler::setThis()
{
    t_scheduler = this;
}
//获得当前线程的调度器
Scheduler *Scheduler::GetThis()
{
    return t_scheduler;
}

Fiber *Scheduler::GetMainFiber()
{
    return t_fiber;
}

bool Scheduler::stopping()
{
    MutexType::Lock lock(m_mutex);
    return m_autoStop && m_stopping && m_fibers.empty() && m_activeThreadCount == 0;
}

//需要子类去自己实现逻辑
void Scheduler::tickle()
{
    SYLAR_LOG_INFO(g_logger) << "tickle了";
}
//需要子类去自己实现逻辑
void Scheduler::idle()
{
    SYLAR_LOG_INFO(g_logger) << "idle了";
    while (!stopping())
    {
        sylar::Fiber::YieldToHold();
    }
}

//需要初始化
//主要的难点是要考虑是否把调用Scheduler的线程也用来执行任务
//之所以要用到调度器线程，最大的考量就是节约一个线程的资源
Scheduler::Scheduler(size_t threads, bool use_caller, const std::string &name) : m_name(name)
{

    //如果要把运行调度器的线程也算上
    if (use_caller)
    {

        // 1、首先初始化Scheduler
        SYLAR_ASSERT2(GetThis() == nullptr, "调度器初始化时不为空");
        t_scheduler = this;

        // 2、然后初始化线程
        sylar::Thread::SetName(name);
        m_rootThread = sylar::GetThreadId();
        m_threadIds.push_back(m_rootThread);
        threads--;

        // 3、接下来是初始化当前线程的主协程。因为是协程的调度器，所有的交互通过协程来进行
        // 3.1 初始化调度器线程的主协程
        sylar::Fiber::GetThis();
        // 3.2 设置当前线程的子协程
        m_rootFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, true));
        // 3.3 设置当前线程的正在执行任务的协程是哪一个
        t_fiber = m_rootFiber.get();
    }
    else
    {
        //代表用不到调度器线程
        m_rootThread = -1;
        sylar::Thread::SetName(name);
    }
    //需要创建的线程数量
    m_threadCount = threads;
}

Scheduler::~Scheduler()
{
    //将保存的Scheduler指针置空
    SYLAR_ASSERT2(m_stopping, "调度器销毁时还未停止");
    if (GetThis() == this)
    {
        t_scheduler = nullptr;
    }
}

void Scheduler::start()
{
    MutexType::Lock lock(m_mutex);

    //如果不是停止状态，就不能反复start，也就是需要直接return
    if (!m_stopping)
    {
        return;
    }
    m_stopping = false;
    SYLAR_ASSERT2(m_threads.empty(), "线程池初始不为空");

    //创建m_threadCount数量的线程,并开启协程，用来消费任务
    m_threads.resize(m_threadCount);
    for (size_t i = 0; i < m_threadCount; i++)
    {

        m_threads[i].reset(
            new sylar::Thread(std::bind(&Scheduler::run, this), m_name + "-" + std::to_string(i)));
        m_threadIds.push_back(m_threads[i]->getId());
    }
    lock.unlock();
    //如果Scheduler所在的线程main也在任务队列中执行任务，那么需要让main开始消费任务
    // 不写在start里，是因为
    // if (m_rootFiber)
    // {
    //     m_rootFiber->call();
    //     SYLAR_LOG_INFO(g_logger) << "m_rootFiber 执行返回了";
    // }
}
void Scheduler::stop()
{
    // 1、如果满足某些条件，设置负责通知的相关变量
    m_autoStop = true;
    // 如果调度器线程也要执行任务，那么需要如下判断,当然了也可能还没完全满足停止条件，而往后走
    // m_threadCount == 0 只是为了在有m_rootFiber且不额外创建线程的时候做的退出判断
    if (m_rootFiber && m_threadCount == 0 &&
        (m_rootFiber->getState() == Fiber::TERM || m_rootFiber->getState() == Fiber::EXCEPT))
    {
        SYLAR_LOG_INFO(g_logger) << "Scheduler::stop()";
        m_stopping = true;
        if (stopping())
        {
            return;
        }
    }

    if (m_rootThread != -1)
    {
        SYLAR_ASSERT(GetThis() == this);
    }
    else
    {
        SYLAR_ASSERT(GetThis() != this);
    }

    m_stopping = true;

    // 2、通知所有的线程停止运行（其实就是在调度器线程不参与任务的情况下进行判断什么时候退出）

    // 2.1 先处理完未完成的任务，再安全退出
    for (size_t i = 0; i < m_threadCount; i++)
    {
        tickle();
    }
    if (m_rootFiber)
    {
        tickle();
    }

    if (m_rootFiber)
    {
        // while (!stopping())
        // {
        //     if (m_rootFiber->getState() == Fiber::TERM || m_rootFiber->getState() ==
        //     Fiber::EXCEPT)
        //     {
        //         m_rootFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, true));
        //         SYLAR_LOG_INFO(g_logger) << " root fiber is term, reset";
        //         // t_fiber = m_rootFiber.get();
        //     }
        //     SYLAR_LOG_INFO(g_logger) << "m_rootFiber->call() FiberId = " << m_rootFiber->getId();
        //     m_rootFiber->call();
        // }
        if (!stopping())
        {
            m_rootFiber->call();
        }
    }

    // 只要有多个线程操作同一个变量，就加锁，不管他是不是会加入额外的线程
    // 其实我怀疑sylar这样写是经验使然，并不是必须
    std::vector<Thread::ptr> thrs;
    {
        MutexType::Lock lock(m_mutex);
        thrs.swap(m_threads);
    }

    for (auto &i : thrs)
    {
        i->join();
    }
}

void Scheduler::run()
{
    SYLAR_LOG_INFO(g_logger) << "Scheduler 的 run 方法执行";
    // 1、设置当前线程的调度器
    setThis();
    // 2、设置好当前运行线程的协程
    if (sylar::GetThreadId() != m_rootThread)
    {
        t_fiber = Fiber::GetThis().get();
    }
    // 3、线程生命周期结束前，始终去查看是否有任务并消费
    // 用来存放当前线程所需要执行的协程或回调函数
    FiberAndThread ft;
    // 如果ft里面是一个回调函数，为了能用协程处理这个回调，需要把这个回调包装进一个Fiber里
    Fiber::ptr cb_fiber;

    Fiber::ptr idle_fiber(new Fiber(std::bind(&Scheduler::idle, this)));
    while (true)
    {
        ft.reset();
        // 3.1
        // 因为可以指定某个协程的任务由某个线程去执行，所以需要当前运行的线程找到自己能运行的任务
        bool tickle_me = false;
        bool is_active = false;
        {
            MutexType::Lock lock(m_mutex);
            auto it = m_fibers.begin();
            while (it != m_fibers.end())
            {
                //先是判断进程什么时候不能执行的某个任务
                if (it->thread != -1 && it->thread != sylar::GetThreadId())
                {
                    it++;
                    tickle_me = true;
                    continue;
                }
                SYLAR_ASSERT2(it->fiber || it->cb, "m_fibers里面的一个fiber没有任务");
                //然后是协程什么时候不能执行某个任务(已经在执行中了，就不能再执行)
                //因为有可能没有指定运行的线程，需要避免竞争
                // if (ft.fiber && ft.fiber->getState() == Fiber::EXEC)如果写成这样的话，是错的
                //因为  ft
                //还没有东西，永远为flase，那么就不会执行continue，就会往后走，触发tickle函数
                if (it->fiber && it->fiber->getState() == Fiber::EXEC)
                {
                    it++;
                    continue;
                }

                ft = *it;
                m_fibers.erase(it);
                ++m_activeThreadCount;
                is_active = true;
                break;
            }
        }

        if (tickle_me)
        {
            tickle();
        }
        // 3.2 可以运行的任务形势为Fiber或者回调函数
        // 3.2.1 如果m_fibers里面的FiberAndThread存的是一个Fiber
        if (ft.fiber &&
            (ft.fiber->getState() != Fiber::TERM && ft.fiber->getState() != Fiber::EXCEPT))
        {
            // m_activeThreadCount++;
            ft.fiber->swapIn();
            m_activeThreadCount--;
            //如果回来以后还是ready，说明是子类的阻塞任务已经完成，需要的数据已经准备好了，就重新加到任务队列里
            if (ft.fiber->getState() == Fiber::READY)
            {
                schedule(ft.fiber);
            }
            // 如果任务被阻塞了，就直接给它挂起
            else if (ft.fiber->getState() != Fiber::TERM && ft.fiber->getState() != Fiber::EXCEPT)
            {
                ft.fiber->m_state = Fiber::HOLD;
            }
            ft.reset();
        }
        // 3.2.2 如果m_fibers里面的FiberAndThread存的是一个Fiber else if ()
        else if (ft.cb)
        {
            // 如果cb_fiber还没有释放资源，又来拿到一个任务，就可以继续去执行这一个任务
            if (cb_fiber)
            {
                cb_fiber->reset(ft.cb);
            }
            //如果cb_fiber才刚来，那么就给他设置一个协程
            else
            {
                cb_fiber.reset(new Fiber(ft.cb));
            }
            //后面的执行都是用cb_fiber，比较ft里面只有一个回调，什么都做不了
            ft.reset();
            // m_activeThreadCount++;
            cb_fiber->swapIn();
            m_activeThreadCount--;

            if (cb_fiber->getState() == Fiber::READY)
            {
                schedule(cb_fiber);
                cb_fiber.reset();
            }
            else if (cb_fiber->getState() == Fiber::EXCEPT || cb_fiber->getState() == Fiber::TERM)
            {
                cb_fiber->reset(nullptr);
            }
            else
            { // if(cb_fiber->getState() != Fiber::TERM) {
                cb_fiber->m_state = Fiber::HOLD;
                cb_fiber.reset();
            }
        }
        // 这里是没有任务时，进行等待操作
        else
        {
            if (is_active)
            {
                --m_activeThreadCount;

                continue;
            }
            if (idle_fiber->getState() == Fiber::TERM)
            {
                SYLAR_LOG_INFO(g_logger) << "idle fiber term";
                break;
            }
            ++m_idleThreadCount;
            idle_fiber->swapIn();
            --m_idleThreadCount;
            if (idle_fiber->getState() != Fiber::TERM && idle_fiber->getState() != Fiber::EXCEPT)
            {
                idle_fiber->m_state = Fiber::HOLD;
            }
        }
    }
}

} // namespace sylar