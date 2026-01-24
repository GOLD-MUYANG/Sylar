#include "sylar/thread.h"
#include "sylar/sylar.h"
#include <functional>
#include <pthread.h>
namespace sylar
{
//创建一个静态变量，避免函数在层层调用时还要传递这个Thread指针
static thread_local Thread *t_thread = nullptr;
static thread_local std::string t_thread_name = "UNKNOW";

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

Thread *Thread::GetThis() { return t_thread; }

const std::string &Thread::GetName() { return t_thread_name; }
void Thread::SetName(const std::string &name)
{
    if (t_thread)
    {
        t_thread->m_name = name;
    }
    t_thread_name = name;
}

Thread::Thread(std::function<void()> cb, const std::string &name) : m_name(name), m_cb(cb)
{
    if (name.empty())
    {
        m_name = "UNKNOW";
    }
    int rt = pthread_create(&m_thread, nullptr, &Thread::run, this);
    if (rt)
    {
        SYLAR_LOG_ERROR(g_logger) << "pthread_join thread fail, rt=" << rt << " name=" << m_name;
        throw std::logic_error("pthread_join error");
    }
}

Thread::~Thread()
{
    if (m_thread)
    {
        pthread_detach(m_thread);
    }
}

void Thread::join()
{
    if (m_thread)
    {
        int rt = pthread_join(m_thread, nullptr);
        if (rt)
        {
            SYLAR_LOG_ERROR(g_logger)
                << "pthread_join thread fail, rt=" << rt << " name=" << m_name;
            throw std::logic_error("pthread_join error");
        }
        m_thread = 0;
    }
}

void *Thread::run(void *arg)
{
    Thread *thread = (Thread *)arg;
    t_thread = thread;
    t_thread_name = thread->m_name;
    thread->m_id = sylar::GetThreadId();
    pthread_setname_np(pthread_self(), thread->m_name.substr(0, 15).c_str());
    /**
        如果直接调用 thread->m_cb()，回调执行后，thread->m_cb
        依然持有这个函数对象，可能被其他代码重复调用（比如线程退出时的重复触发）；
        用 swap 转移后，thread->m_cb 被清空，从根本上避免了 “回调被重复执行”
        的风险，是一种排他性的调用。
    */
    std::function<void()> cb;
    cb.swap(thread->m_cb);
    cb();
    return 0;
}

} // namespace sylar