#include "sylar.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <ucontext.h>
namespace sylar
{

static Logger::ptr g_logger = SYLAR_LOG_NAME("system");
std::atomic<uint64_t> s_fiber_id{0};
std::atomic<uint64_t> s_fiber_count{0};

//存储「当前线程正在运行的协程」的原始指针，用于快速获取当前执行的协程对象
static thread_local Fiber *t_fiber = nullptr;
//存储「当前线程的主协程」的智能指针，用于管理主协程的生命周期，保证主协程随线程生命周期存在。
static thread_local Fiber::ptr t_thread_fiber = nullptr;

sylar::ConfigVar<uint32_t>::ptr g_fiber_stack_size =
    Config::Lookup<uint32_t>("fiber.stack_size", 1024 * 1024, "fiber stack size");

class MallocStackAllocator
{
public:
    static void *Alloc(size_t size)
    {
        return malloc(size);
    }

    static void Dealloc(void *vp, size_t size)
    {
        return free(vp);
    }
};

using StackAllocator = MallocStackAllocator;
//无参构造函数用于创建主协程，不需要独立的栈空间，核心是关联当前线程的执行上下文
Fiber::Fiber()
{
    m_state = EXEC;
    SetThis(this);
    //获取线程的上下文环境，保存到m_ctx中，方便在子协程全部结束后回到线程的原状态
    if (getcontext(&m_ctx))
    {
        SYLAR_ASSERT2(false, "getcontext");
    }
    //当前线程中协程数量+1
    ++s_fiber_count;
    SYLAR_LOG_DEBUG(g_logger) << "Fiber::Fiber id=" << m_id;
}

//创建子协程，拥有独立的栈空间，执行函数cb
//需要初始化协程上下文
Fiber::Fiber(std::function<void()> cb, size_t stacksize) : m_id(++s_fiber_id), m_cb(cb)
{
    s_fiber_count++;
    m_stacksize = stacksize ? stacksize : g_fiber_stack_size->getValue();
    m_stack = StackAllocator::Alloc(m_stacksize);
    //保留的是线程的状态，子协程用不到，他只是为了初始化m_ctx,方便后面makecontext使用
    if (getcontext(&m_ctx))
    {
        SYLAR_ASSERT2(false, "getcontext");
    }
    //改成nullptr，后面协程执行完毕后不会再切换到别的协程，而是直接回到线程的上下文环境
    //所以这里只是暂时这样设置
    m_ctx.uc_link = nullptr;
    m_ctx.uc_stack.ss_sp = m_stack;
    m_ctx.uc_stack.ss_size = m_stacksize;
    makecontext(&m_ctx, &Fiber::MainFunc, 0);
    SYLAR_LOG_DEBUG(g_logger) << "Fiber::Fiber id=" << m_id;
}

Fiber::~Fiber()
{
    --s_fiber_count;
    if (m_stack)
    {
        SYLAR_ASSERT(m_state == TERM || m_state == EXCEPT || m_state == INIT);

        StackAllocator::Dealloc(m_stack, m_stacksize);
    }
    else
    {
        SYLAR_ASSERT(!m_cb);
        SYLAR_ASSERT(m_state == EXEC);

        Fiber *cur = t_fiber;
        if (cur == this)
        {
            SetThis(nullptr);
        }
    }
    SYLAR_LOG_DEBUG(g_logger) << "Fiber::~Fiber id=" << m_id;
}

//设置当前运行的协程
void Fiber::SetThis(Fiber *f)
{
    t_fiber = f;
}
Fiber::ptr Fiber::GetThis()
{
    if (t_fiber)
    {
        return t_fiber->shared_from_this();
    }
    //无参构造函数创建主协程，构造函数把当前协程设置为主协程
    Fiber::ptr main_fiber(new Fiber);

    SYLAR_ASSERT(t_fiber == main_fiber.get());
    t_thread_fiber = main_fiber;
    return t_fiber->shared_from_this();
}
void Fiber::YieldToReady()
{
    Fiber::ptr cur = GetThis();
    cur->m_state = READY;
    cur->swapOut();
}
void Fiber::YieldToHold()
{
    Fiber::ptr cur = GetThis();
    cur->m_state = HOLD;
    cur->swapOut();
}
uint64_t Fiber::TotalFibers()
{
    return s_fiber_count;
}

//创建的协程要执行哪一个方法
void Fiber::MainFunc()
{
    Fiber::ptr cur = GetThis();
    SYLAR_ASSERT(cur);
    try
    {
        cur->m_cb();
        //协程执行回调是一次性的，执行完毕后状态变为TERM，继续持有回调函数
        //有可能误写导致除法，也有可能一直持有内存泄露
        cur->m_cb = nullptr;
        cur->m_state = TERM;
    }
    catch (std::exception &e)
    {
        cur->m_state = EXCEPT;
        SYLAR_LOG_ERROR(SYLAR_LOG_ROOT())
            << "Fiber Except: " << e.what() << " fiber_id=" << cur->getId()
            << sylar::BacktraceToString(10, 2);
    }
    catch (...)
    {
        cur->m_state = EXCEPT;
        SYLAR_LOG_ERROR(g_logger) << "Fiber Except"
                                  << " fiber_id=" << cur->getId() << std::endl
                                  << sylar::BacktraceToString(10, 2);
    }
    //如果不用一个普通指针的话，就会造成能跳到主协程但是没办法释放子协程智能指针，
    //或者能释放但跳转不了，所以下面的写法是必要的
    auto raw_ptr = cur.get();
    cur.reset();
    raw_ptr->swapOut();
    SYLAR_ASSERT2(false, "never reach fiber_id=" + std::to_string(raw_ptr->getId()));
}

//执行完了当前的回调函数，但是还没有释放资源，那么就新给他一个任务
void Fiber::reset(std::function<void()> cb)
{
    SYLAR_ASSERT(m_stack);
    SYLAR_ASSERT(m_state == TERM || m_state == EXCEPT || m_state == INIT);
    m_cb = cb;
    ;
    if (getcontext(&m_ctx))
    {
        SYLAR_ASSERT2(false, "getcontext");
    }
    m_ctx.uc_link = nullptr;
    m_ctx.uc_stack.ss_sp = m_stack;
    m_ctx.uc_stack.ss_size = m_stacksize;
    makecontext(&m_ctx, &Fiber::MainFunc, 0);
    m_state = INIT;
}
//由主协程转到子协程
void Fiber::swapIn()
{
    SetThis(this);
    SYLAR_ASSERT(m_state != EXEC);
    m_state = EXEC;
    if (swapcontext(&t_thread_fiber->m_ctx, &m_ctx))
    {
        SYLAR_ASSERT2(false, "swapcontext");
    }
}
//由子协程转到主协程
void Fiber::swapOut()
{
    SetThis(t_thread_fiber.get());
    if (swapcontext(&m_ctx, &t_thread_fiber->m_ctx))
    {
        SYLAR_ASSERT2(false, "swapcontext");
    }
}

uint64_t Fiber::GetFiberId()
{
    if (t_fiber)
    {
        return t_fiber->getId();
    }
    return 0;
}

} // namespace sylar