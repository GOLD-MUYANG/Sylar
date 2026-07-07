#include "worker.h"
#include "config.h"
#include "util.h"

#include <exception>

namespace sylar
{

static sylar::ConfigVar<std::map<std::string, std::map<std::string, std::string>>>::ptr
    g_worker_config = sylar::Config::Lookup(
        "workers", std::map<std::string, std::map<std::string, std::string>>(), "worker config");

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

WorkerGroup::WorkerGroup(uint32_t batch_size, sylar::Scheduler *s)
    : m_batchSize(batch_size), m_finish(false), m_scheduler(s), m_sem(batch_size)
{
}

WorkerGroup::~WorkerGroup()
{
    waitAll();
}

void WorkerGroup::schedule(std::function<void()> cb, int thread)
{
    // 先占用一个并发名额；如果名额用完，当前协程会挂起等待。
    m_sem.wait();
    // shared_from_this() 保证异步任务执行期间 WorkerGroup 不会被析构。
    m_scheduler->schedule(std::bind(&WorkerGroup::doWork, shared_from_this(), cb), thread);
}

void WorkerGroup::doWork(std::function<void()> cb)
{
    try
    {
        cb();
    }
    catch (const std::exception &e)
    {
        // Scheduler 的普通任务协程不会接住异常；这里隔离失败任务，避免终止整个进程。
        SYLAR_LOG_ERROR(g_logger) << "WorkerGroup callback threw exception: " << e.what();
    }
    catch (...)
    {
        SYLAR_LOG_ERROR(g_logger) << "WorkerGroup callback threw an unknown exception";
    }
    // 无论正常完成还是异常退出，都释放名额，唤醒等待投递或等待完成的协程。
    m_sem.notify();
}

void WorkerGroup::waitAll()
{
    if (!m_finish)
    {
        m_finish = true;
        for (uint32_t i = 0; i < m_batchSize; ++i)
        {
            // 取回全部并发名额，就是说没有任务可以继续执行了。
            m_sem.wait();
        }
    }
}

WorkerManager::WorkerManager() : m_stop(false)
{
}

void WorkerManager::add(Scheduler::ptr s)
{
    m_datas[s->getName()].push_back(s);
}

Scheduler::ptr WorkerManager::get(const std::string &name)
{
    auto it = m_datas.find(name);
    if (it == m_datas.end())
    {
        return nullptr;
    }
    if (it->second.size() == 1)
    {
        return it->second[0];
    }
    // 同一名称下可能有多个 IOManager，随机选一个做简单负载分摊。
    return it->second[rand() % it->second.size()];
}

IOManager::ptr WorkerManager::getAsIOManager(const std::string &name)
{
    return std::dynamic_pointer_cast<IOManager>(get(name));
}

bool WorkerManager::init()
{
    auto workers = g_worker_config->getValue();
    for (auto &i : workers)
    {
        std::string name = i.first;
        // thread_num 表示每个 IOManager 内部的线程数。
        int32_t thread_num = sylar::GetParamValue(i.second, "thread_num", 1);
        // worker_num 表示同名 worker 组里创建多少个 IOManager。
        int32_t worker_num = sylar::GetParamValue(i.second, "worker_num", 1);

        for (int32_t x = 0; x < worker_num; ++x)
        {
            Scheduler::ptr s;
            if (!x)
            {
                s = std::make_shared<IOManager>(thread_num, false, name);
            }
            else
            {
                // 后续实例带序号，方便 dump 时区分具体调度器。
                s = std::make_shared<IOManager>(thread_num, false, name + "-" + std::to_string(x));
            }
            add(s);
        }
    }
    // 没有配置任何 worker 时，管理器视为已停止。
    m_stop = m_datas.empty();
    return true;
}

void WorkerManager::stop()
{
    if (m_stop)
    {
        return;
    }
    for (auto &i : m_datas)
    {
        for (auto &n : i.second)
        {
            // 投递一个空任务用于唤醒可能处于 idle 的调度线程，然后停止调度器。
            n->schedule([]() {});
            n->stop();
        }
    }
    m_datas.clear();
    m_stop = true;
}

uint32_t WorkerManager::getCount()
{
    return m_datas.size();
}

std::ostream &WorkerManager::dump(std::ostream &os)
{
    for (auto &i : m_datas)
    {
        for (auto &n : i.second)
        {
            n->dump(os) << std::endl;
        }
    }
    return os;
}

} // namespace sylar
