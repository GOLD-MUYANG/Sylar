#ifndef __SYLAR_FIBER_H__
#define __SYLAR_FIBER_H__
#include "thread.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <sys/ucontext.h>
#include <ucontext.h>
namespace sylar
{
class Fiber : public std::enable_shared_from_this<Fiber>
{

    friend class Scheduler;

public:
    enum State
    {
        INIT,
        HOLD,
        EXEC,
        TERM,
        READY,
        EXCEPT
    };

public:
    //下面的几个方法是协程的切换和获取
    typedef std::shared_ptr<Fiber> ptr;
    Fiber(std::function<void()> cb, size_t stacksize = 0, bool use_caller = false);
    ~Fiber();
    //用来给未释放的协程重新一个回调函数来执行任务
    void reset(std::function<void()> cb);
    //用来调度模块的调度器协程执行任务
    void call();
    void back();
    //由主协程转到子协程
    void swapIn();
    //由子协程转到主协程
    void swapOut();

    uint32_t getId() const
    {
        return m_id;
    }

    State getState() const
    {
        return m_state;
    }

public:
    static void SetThis(Fiber *f);
    static Fiber::ptr GetThis();
    static void YieldToReady();
    static void YieldToHold();
    static uint64_t TotalFibers();
    static void MainFunc();
    static void CallerMainFunc();
    static uint64_t GetFiberId();

private:
    Fiber();

private:
    uint64_t m_id = 0;
    uint32_t m_stacksize = 0;
    State m_state = INIT;
    ucontext_t m_ctx;
    void *m_stack = nullptr;
    std::function<void()> m_cb;
};
} // namespace sylar
#endif