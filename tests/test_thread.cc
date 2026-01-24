#include "sylar/sylar.h"
#include <chrono>
#include <unistd.h>
sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");
void fun1()
{
    SYLAR_LOG_INFO(g_logger) << "name: " << sylar::Thread::GetName()
                             << " this.name: " << sylar::Thread::GetThis()->getName()
                             << " id: " << sylar::GetThreadId()
                             << " this.id: " << sylar::Thread::GetThis()->getId();

    sleep(20);
}

void fun2() {}

int main()
{
    SYLAR_LOG_INFO(g_logger) << "thread test begin";
    std::vector<sylar::Thread::ptr> thrs;
    for (int i = 0; i < 10; ++i)
    {
        sylar::Thread::ptr thr(new sylar::Thread(&fun1, "name_" + std::to_string(i)));
        thrs.push_back(thr);
    }

    for (size_t i = 0; i < thrs.size(); ++i)
    {
        thrs[i]->join();
    }
    SYLAR_LOG_INFO(g_logger) << "thread test end";
    return 0;
}