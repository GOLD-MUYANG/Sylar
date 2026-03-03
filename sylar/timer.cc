#include "timer.h"
#include "sylar/iomanager.h"
#include "sylar/thread.h"
#include "util.h"
#include <cstdint>
#include <functional>
#include <memory>
namespace sylar

{

// 返回 true 表示 a 应该排在 b 前面（严格弱序）。
bool Timer::Comparator::operator()(const Timer::ptr &lhs, const Timer::ptr &rhs) const
{
    if (!lhs && !rhs)
    {
        return false;
    }
    if (!lhs)
    {
        return true;
    }
    if (!rhs)
    {
        return false;
    }
    if (lhs->m_next < rhs->m_next)
    {
        return true;
    }
    if (rhs->m_next < lhs->m_next)
    {
        return false;
    }
    return lhs.get() < rhs.get();
}

Timer::Timer(uint64_t ms, std::function<void()> cb, TimerManager *manager, bool recurring)
    : m_ms(ms), m_cb(cb), m_manager(manager), m_recurring(recurring)
{
    m_next = GetCurrentMS() + m_ms;
}
Timer::Timer(uint64_t next) : m_next(next)
{
}

// cancel 取消定时器(后面不执行)
bool Timer::cancel()
{
    TimerManager::RWMutexType::WriteLock lock(m_manager->m_mutex);
    if (m_cb)
    {
        m_cb = nullptr;
        //从manager里面找到这个timer，然后删除他
        auto it = m_manager->m_timers.find(shared_from_this());
        if (it != m_manager->m_timers.end())
        {
            m_manager->m_timers.erase(it);
        }
        return true;
    }
    return false;
}

// 给现在这个定时器重新设置下次执行的时间（当前时间  +  ms）
bool Timer::refresh()
{
    TimerManager::RWMutexType::WriteLock lock(m_manager->m_mutex);
    if (!m_cb)
    {
        return false;
    }
    //先找到
    auto it = m_manager->m_timers.find(shared_from_this());
    if (it == m_manager->m_timers.end())
    {
        return false;
    }
    //删除原来的
    m_manager->m_timers.erase(it);
    //添加新的
    m_next = sylar::GetCurrentMS() + m_ms;
    m_manager->m_timers.insert(shared_from_this());
    return true;
}

// 给现在这个定时器重新设置执行间隔，
// 并确定下次执行的时间是当前时间 + ms还是原本应下次执行的时间再 + ms
bool Timer::reset(uint64_t ms, bool from_now)
{
    //什么时候不用变
    if (ms == m_ms && !from_now)
    {
        return true;
    }
    TimerManager::RWMutexType::WriteLock lock(m_manager->m_mutex);

    //先找到
    if (!m_cb)
    {
        return false;
    }
    auto it = m_manager->m_timers.find(shared_from_this());
    if (it == m_manager->m_timers.end())
    {
        return false;
    }
    //删除原来的
    m_manager->m_timers.erase(it);
    //添加新的
    m_ms = ms;
    uint64_t start = 0;
    if (from_now)
    {
        start = sylar::GetCurrentMS();
    }
    else
    {
        start = m_next - m_ms;
    }
    m_ms = ms;
    m_next = start + m_ms;
    m_manager->addTimer(shared_from_this(), lock);
    return true;
}

TimerManager::TimerManager()
{
    m_previouseTime = sylar::GetCurrentMS();
}

TimerManager::~TimerManager()
{
}

Timer::ptr TimerManager::addTimer(uint64_t ms, std::function<void()> cb, bool recurring)
{
    Timer::ptr timer(new Timer(ms, cb, this, recurring));
    RWMutexType::WriteLock lock(m_mutex);
    addTimer(timer, lock);
    return timer;
}

static void onTimer(std::weak_ptr<void> weak_cond, std::function<void()> cb)
{
    std::shared_ptr<void> cond = weak_cond.lock();
    if (cond)
    {
        cb();
    }
}

Timer::ptr TimerManager::addConditionTimer(uint64_t ms,
                                           std::function<void()> cb,
                                           std::weak_ptr<void> weak_cond,
                                           bool recurring)
{
    return addTimer(ms, std::bind(&onTimer, weak_cond, cb), recurring);
}

void TimerManager::addTimer(Timer::ptr val, RWMutexType::WriteLock &lock)
{
    //得到位置，看看时间是不是最早的
    auto it = m_timers.insert(val).first;
    bool at_front = (it == m_timers.begin()) && !m_tickled;
    if (at_front)
    {
        m_tickled = true;
    }
    lock.unlock();
    if (at_front)
    {
        //里面是tickle(),代表来了一个新任务（Fibers）
        onTimerInsertedAtFront();
    }
}

// 列出所有过期的定时器,判断是否重新调度
//按理来说m_timers里面都是没执行的，不会有过期的，有过期的就是时钟回拨了
void TimerManager::listExpiredCb(std::vector<std::function<void()>> &cbs)
{

    std::vector<Timer::ptr> expired;
    {
        RWMutexType::ReadLock lock(m_mutex);
        if (m_timers.empty())
        {
            return;
        }
    }
    RWMutexType::WriteLock lock(m_mutex);
    // 仅仅用于判断 m_timers里的定时器是否过期，用来比较的
    uint64_t now_ms = GetCurrentMS();
    // 检测是否发生了时钟回拨
    bool rollover = detectClockRollover(now_ms);
    //如果没有时钟回拨，并且最近的一个定时器都没到应执行的时间
    //那就是说明没有过期的定时器
    if (!rollover && (*(m_timers.begin()))->m_next > now_ms)
    {
        return;
    }

    Timer::ptr now_timer(new Timer(now_ms));
    // 找到第一个下次执行时间大于等于当前时间的定时器,如果时钟回拨了，就是所有的都过期了
    //避免定时器因时钟异常回拨而永不触发
    auto it = rollover ? m_timers.end() : m_timers.lower_bound(now_timer);
    //把下次执行时间等于当前时间的定时器排除在外，不算过期
    while (it != m_timers.end() && (*it)->m_next <= now_ms)
    {
        it++;
    }
    // 把所有过期的定时器都放到 expired 里
    expired.insert(expired.begin(), m_timers.begin(), it);
    m_timers.erase(m_timers.begin(), it);
    cbs.reserve(expired.size());
    for (auto &timer : expired)
    {
        cbs.push_back(timer->m_cb);
        if (timer->m_recurring)
        {
            timer->m_next = now_ms + timer->m_ms;
            m_timers.insert(timer);
        }
        else
        {
            timer->m_cb = nullptr;
        }
    }
}

//返回下一个未过期的定时器要执行的具体时间
uint64_t TimerManager::getNextTimer()
{
    RWMutexType::ReadLock lock(m_mutex);
    //表示读取的就是最早要执行的一个定时器的时间，不需要通知epoll去修改timeout
    m_tickled = false;
    // 如果定时器队列空了，返回一个最大值，代表没有定时器要执行
    if (m_timers.empty())
    {
        return ~0ull;
    }
    // 否则返回最早要执行的定时器的时间
    const Timer::ptr &next = *m_timers.begin();
    uint64_t now_ms = sylar::GetCurrentMS();
    if (now_ms >= next->m_next)
    {
        return 0;
    }
    else
    {
        return next->m_next - now_ms;
    }
}

bool TimerManager::hasTimer()
{
    RWMutexType::ReadLock lock(m_mutex);
    return !m_timers.empty();
}

bool TimerManager::detectClockRollover(uint64_t now_ms)
{
    bool rollover = false;
    if (now_ms < m_previouseTime && now_ms < (m_previouseTime - 60 * 60 * 1000))
    {
        rollover = true;
    }
    m_previouseTime = now_ms;
    return rollover;
}
} // namespace sylar