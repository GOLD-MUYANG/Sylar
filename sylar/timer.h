#ifndef __SYLAR_TIMER_H__
#define __SYLAR_TIMER_H__

#include "thread.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <vector>
namespace sylar
{
class TimerManager;
class Timer : public std::enable_shared_from_this<Timer>
{
    friend class TimerManager;

public:
    typedef std::shared_ptr<Timer> ptr;

    bool cancel();
    bool refresh();
    bool reset(uint64_t ms, bool from_now = false);

private:
    Timer(uint64_t ms, std::function<void()> cb, TimerManager *manager, bool recurring = false);
    Timer(uint64_t next);

private:
    struct Comparator
    {
        bool operator()(const Timer::ptr &lhs, const Timer::ptr &rhs) const;
    };

private:
    /// 定时器间隔时间（毫秒）
    uint64_t m_ms = 0;
    /// 定时器回调函数
    std::function<void()> m_cb;
    /// 定时器管理器
    TimerManager *m_manager = nullptr;
    //是否重复触发
    bool m_recurring = false;
    /// 下次触发时间（毫秒）（精确的一个触发时间）
    uint64_t m_next = 0;
};

class TimerManager
{
    friend class Timer;

public:
    typedef RWMutex RWMutexType;
    TimerManager();
    virtual ~TimerManager();

public:
    /// 添加定时器
    Timer::ptr addTimer(uint64_t ms, std::function<void()> cb, bool recurring = false);
    /// 添加条件定时器
    Timer::ptr addConditionTimer(uint64_t ms,
                                 std::function<void()> cb,
                                 std::weak_ptr<void> weak_cond,
                                 bool recurring = false);
    void listExpiredCb(std::vector<std::function<void()>> &cbs);
    uint64_t getNextTimer();
    bool hasTimer();

protected:
    virtual void onTimerInsertedAtFront() = 0;
    void addTimer(Timer::ptr val, RWMutexType::WriteLock &lock);

private:
    /// 检测是否发生了时钟回拨
    bool detectClockRollover(uint64_t now_ms);

private:
    /// 定时器集合
    std::set<Timer::ptr, Timer::Comparator> m_timers;
    /// 读写锁
    RWMutexType m_mutex;
    /// 用于通知epoll是否有新的定时器修改或者插入，需要去执行的
    bool m_tickled = false;
    /// 上一次记录的时钟时间
    uint64_t m_previouseTime = 0;
};
} // namespace sylar
#endif