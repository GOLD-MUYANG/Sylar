#ifndef __SYLAR_WORKER_H__
#define __SYLAR_WORKER_H__

#include "iomanager.h"
#include "log.h"
#include "mutex.h"
#include "singleton.h"

namespace sylar
{

/**
 * @brief 一组带并发限制的调度任务。
 *
 * WorkerGroup 本身不创建线程，它依赖外部传入的 Scheduler。
 * schedule() 每次投递任务前都会先从 m_sem 获取一个并发名额，
 * 任务执行结束后再归还名额，因此最多只有 batch_size 个任务同时运行。
 */
class WorkerGroup : Noncopyable, public std::enable_shared_from_this<WorkerGroup>
{
public:
    typedef std::shared_ptr<WorkerGroup> ptr;
    static WorkerGroup::ptr Create(uint32_t batch_size,
                                   sylar::Scheduler *s = sylar::Scheduler::GetThis())
    {
        return std::make_shared<WorkerGroup>(batch_size, s);
    }

    WorkerGroup(uint32_t batch_size, sylar::Scheduler *s = sylar::Scheduler::GetThis());
    ~WorkerGroup();

    void schedule(std::function<void()> cb, int thread = -1);
    void waitAll();

private:
    // 包装真实回调：执行完用户任务后释放一个并发名额。
    void doWork(std::function<void()> cb);

private:
    // 最大并发任务数。
    uint32_t m_batchSize;
    // waitAll() 是否已经执行过，避免重复等待。
    bool m_finish;
    // 实际承载任务的调度器，不归 WorkerGroup 所有。
    Scheduler *m_scheduler;
    // 协程信号量，用于控制并发和等待任务完成。
    FiberSemaphore m_sem;
};

/**
 * @brief 全局 Worker 调度器管理器。
 *
 * 根据 workers 配置创建一批 IOManager，并按名称保存。
 * 外部可以通过名称获取调度器，或者直接把协程/回调投递到指定 worker。
 */
class WorkerManager
{
public:
    WorkerManager();
    // 把一个 Scheduler 加入管理，同名 Scheduler 会组成一组。
    void add(Scheduler::ptr s);
    // 按名称获取 Scheduler；同名有多个时随机返回一个，实现简单负载分摊。
    Scheduler::ptr get(const std::string &name);
    // 按名称获取 IOManager 类型的 Scheduler，类型不匹配时返回 nullptr。
    IOManager::ptr getAsIOManager(const std::string &name);

    template <class FiberOrCb>
    void schedule(const std::string &name, FiberOrCb fc, int thread = -1)
    {
        auto s = get(name);
        if (s)
        {
            s->schedule(fc, thread);
        }
        else
        {
            static sylar::Logger::ptr s_logger = SYLAR_LOG_NAME("system");
            SYLAR_LOG_ERROR(s_logger) << "schedule name=" << name << " not exists";
        }
    }

    template <class Iter>
    void schedule(const std::string &name, Iter begin, Iter end)
    {
        auto s = get(name);
        if (s)
        {
            s->schedule(begin, end);
        }
        else
        {
            static sylar::Logger::ptr s_logger = SYLAR_LOG_NAME("system");
            SYLAR_LOG_ERROR(s_logger) << "schedule name=" << name << " not exists";
        }
    }

    // 从配置项 workers 初始化所有 worker。
    bool init();
    // 停止并清理所有已管理的 Scheduler。
    void stop();

    bool isStoped() const
    {
        return m_stop;
    }
    std::ostream &dump(std::ostream &os);

    uint32_t getCount();

private:
    // worker 名称 -> 同名 Scheduler 列表。
    std::map<std::string, std::vector<Scheduler::ptr>> m_datas;
    // 是否已经停止，或初始化后没有任何 worker。
    bool m_stop;
};

typedef sylar::Singleton<WorkerManager> WorkerMgr;

} // namespace sylar

#endif
