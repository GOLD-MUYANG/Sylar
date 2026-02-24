#include "sylar/sylar.h"

static sylar::Logger::ptr g_logger = SYLAR_LOG_ROOT();

void test_fiber()
{
    static int s_count = 5;
    SYLAR_LOG_INFO(g_logger) << "test in fiber s_count=" << s_count;

    sleep(1);
    if (--s_count >= 0)
    {
        sylar::Scheduler::GetThis()->schedule(&test_fiber, sylar::GetThreadId());
        // sylar::Scheduler::GetThis()->schedule(&test_fiber);
    }
}

int main(int argc, char **argv)
{
    SYLAR_LOG_INFO(g_logger) << "main";
    sylar::Scheduler sc(3, false, "main");
    // sylar::Scheduler sc(2);
    sc.schedule(&test_fiber);
    sc.start();
    // sleep(2);
    // SYLAR_LOG_INFO(g_logger) << "schedule";
    sc.stop();
    SYLAR_LOG_INFO(g_logger) << "over";
    return 0;
}
