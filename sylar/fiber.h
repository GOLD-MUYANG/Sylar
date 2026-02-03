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
public:
    typedef std::shared_ptr<Fiber> ptr;
    enum State
    {
        INIT,
        HOLD,
        EXEC,
        TERM,
        READY,
        EXCEPT
    };

private:
    Fiber();

public:
    Fiber(std::function<void()> cb, size_t stacksize = 0);
    ~Fiber();
    void reset(std::function<void()> cb);
    //由主协程转到子协程
    void swapIn();
    //由子协程转到主协程
    void swapOut();

    uint32_t getId() const
    {
        return m_id;
    }

public:
    //下面的几个方法是协程的切换和获取
    static void SetThis(Fiber *f);
    static Fiber::ptr GetThis();
    static void YieldToReady();
    static void YieldToHold();
    static uint64_t TotalFibers();
    static void MainFunc();
    static uint64_t GetFiberId();

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