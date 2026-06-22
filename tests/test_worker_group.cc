#include "sylar/iomanager.h"
#include "sylar/log.h"
#include "sylar/worker.h"

#include <atomic>
#include <stdexcept>

static sylar::Logger::ptr g_logger = SYLAR_LOG_ROOT();
static std::atomic<int> g_failures(0);

#define EXPECT_EQ(lhs, rhs)                                                                        \
    do                                                                                             \
    {                                                                                              \
        auto _lhs = (lhs);                                                                         \
        auto _rhs = (rhs);                                                                         \
        if (_lhs != _rhs)                                                                          \
        {                                                                                          \
            ++g_failures;                                                                          \
            SYLAR_LOG_ERROR(g_logger) << "EXPECT_EQ failed: " #lhs "=" << _lhs << " " #rhs "=" \
                                      << _rhs << " line=" << __LINE__;                             \
        }                                                                                          \
    } while (0)

namespace
{

void test_batch_size_allows_initial_and_follow_up_tasks()
{
    std::atomic<int> completed(0);
    auto group = sylar::WorkerGroup::Create(1);

    // batch_size 为 1 时，第一项任务必须能立即取得名额，第二项在前者结束后执行。
    group->schedule([&completed]() { ++completed; });
    group->schedule([&completed]() { ++completed; });
    group->waitAll();

    EXPECT_EQ(completed.load(), 2);
}

void test_throwing_callback_releases_worker_slot()
{
    std::atomic<int> completed(0);
    auto group = sylar::WorkerGroup::Create(1);

    // 第一项任务异常退出后，第二项任务仍必须能够取得被归还的并发名额。
    group->schedule([&completed]()
                    {
                        ++completed;
                        throw std::runtime_error("expected worker callback failure");
                    });
    group->schedule([&completed]() { ++completed; });
    group->waitAll();

    EXPECT_EQ(completed.load(), 2);
}

void run_tests()
{
    test_batch_size_allows_initial_and_follow_up_tasks();
    test_throwing_callback_releases_worker_slot();
}

} // namespace

int main()
{
    {
        sylar::IOManager iom(2);
        iom.schedule(run_tests);
    }
    return g_failures.load() == 0 ? 0 : 1;
}
