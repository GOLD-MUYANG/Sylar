#include "sylar/http/http_concurrency_limiter.h"
#include "sylar/log.h"
#include <atomic>
#include <unistd.h>

// 使用 sylar 根日志器，方便测试失败时输出日志。
static sylar::Logger::ptr g_logger = SYLAR_LOG_ROOT();

// 记录测试失败次数。
// 用 atomic 是为了即使以后测试改成多线程，也能保证自增操作相对安全。
static std::atomic<int> g_failures(0);

/**
 * @brief 简单测试断言宏。
 *
 * 如果 expr 为 false：
 * 1. 失败次数 +1；
 * 2. 打印失败表达式；
 * 3. 打印失败所在行号。
 *
 * 注意：
 * - 这个宏不会中断程序；
 * - 一个测试失败后，后面的测试仍然会继续跑；
 * - main 最后根据 g_failures 决定返回 0 还是 1。
 */
#define EXPECT_TRUE(expr)                                                                          \
    do                                                                                             \
    {                                                                                              \
        if (!(expr))                                                                               \
        {                                                                                          \
            ++g_failures;                                                                          \
            SYLAR_LOG_ERROR(g_logger) << "EXPECT_TRUE failed: " #expr << " line=" << __LINE__;     \
        }                                                                                          \
    } while (0)

// 匿名命名空间：
// 这里面的函数只在当前 cpp 文件内部可见，避免和其他文件里的函数重名。
namespace
{

/**
 * @brief 测试：默认不限流时，允许同一个 endpoint 获取多个并发名额。
 *
 * options 默认构造：
 * - max_service_concurrency 没有限制；
 * - max_endpoint_concurrency 没有限制；
 * - max_global_concurrency 没有限制。
 *
 * 预期：
 * - limiter 创建成功；
 * - endpoint-a 第一次 acquire 成功；
 * - endpoint-a 第二次 acquire 也成功。
 */
void test_unlimited_limiter_allows_multiple_requests()
{
    sylar::http::HttpConcurrencyLimitOptions options;

    // 根据限流配置创建限流器。
    auto limiter = sylar::http::HttpConcurrencyLimiter::Create(options);

    EXPECT_TRUE(limiter != nullptr);
    if (!limiter)
    {
        return;
    }

    // 默认不限流，同一个 endpoint 可以连续获取多个并发名额。
    auto first = limiter->tryAcquire("endpoint-a");
    auto second = limiter->tryAcquire("endpoint-a");

    EXPECT_TRUE(first != nullptr);
    EXPECT_TRUE(second != nullptr);
}

/**
 * @brief 测试：服务级并发限制。
 *
 * max_service_concurrency = 1 表示：
 * 当前 limiter 实例内部，同时最多只允许 1 个请求通过。
 *
 * 注意这里不区分 endpoint：
 * - endpoint-a 已经占用了服务级并发名额；
 * - endpoint-b 再来请求，也应该被拒绝。
 *
 * 预期：
 * 1. 第一次 acquire 成功；
 * 2. 第二次 acquire 失败；
 * 3. 释放 first 后，第三次 acquire 成功。
 */
void test_service_limit_rejects_until_guard_releases()
{
    sylar::http::HttpConcurrencyLimitOptions options;

    // 服务级最大并发数限制为 1。
    options.max_service_concurrency = 1;

    auto limiter = sylar::http::HttpConcurrencyLimiter::Create(options);

    EXPECT_TRUE(limiter != nullptr);
    if (!limiter)
    {
        return;
    }

    // 第一个请求占用唯一的服务级并发名额。
    auto first = limiter->tryAcquire("endpoint-a");

    // 虽然是 endpoint-b，但服务级限制是整个 limiter 共享的，
    // 所以这里应该获取失败。
    auto second = limiter->tryAcquire("endpoint-b");

    EXPECT_TRUE(first != nullptr);
    EXPECT_TRUE(second == nullptr);

    // 手动释放 first。
    // 这里依赖 guard 的 RAII 机制：
    // guard 被 reset / 析构时，会自动归还并发名额。
    first.reset();

    // 服务级名额释放后，endpoint-b 应该可以获取成功。
    auto third = limiter->tryAcquire("endpoint-b");

    EXPECT_TRUE(third != nullptr);
}

/**
 * @brief 测试：endpoint 级并发限制。
 *
 * max_endpoint_concurrency = 1 表示：
 * 每一个 endpoint 单独最多允许 1 个并发请求。
 *
 * 注意：
 * - endpoint-a 和 endpoint-b 的计数互不影响；
 * - endpoint-a 达到上限，不应该影响 endpoint-b。
 *
 * 预期：
 * 1. endpoint-a 第一次 acquire 成功；
 * 2. endpoint-a 第二次 acquire 失败；
 * 3. endpoint-b acquire 成功；
 * 4. 释放 endpoint-a 的 first 后，endpoint-a 可以再次 acquire 成功。
 */
void test_endpoint_limit_is_counted_per_endpoint()
{
    sylar::http::HttpConcurrencyLimitOptions options;

    // 单个 endpoint 最大并发数限制为 1。
    options.max_endpoint_concurrency = 1;

    auto limiter = sylar::http::HttpConcurrencyLimiter::Create(options);

    EXPECT_TRUE(limiter != nullptr);
    if (!limiter)
    {
        return;
    }

    // endpoint-a 第一次获取成功。
    auto first = limiter->tryAcquire("endpoint-a");

    // endpoint-a 已经达到并发上限，所以第二次应该失败。
    auto second = limiter->tryAcquire("endpoint-a");

    // endpoint-b 是另一个 endpoint，应该有独立计数，所以应该成功。
    auto third = limiter->tryAcquire("endpoint-b");

    EXPECT_TRUE(first != nullptr);
    EXPECT_TRUE(second == nullptr);
    EXPECT_TRUE(third != nullptr);

    // 释放 endpoint-a 的并发名额。
    first.reset();

    // endpoint-a 名额释放后，应该可以再次获取成功。
    auto fourth = limiter->tryAcquire("endpoint-a");

    EXPECT_TRUE(fourth != nullptr);
}

/**
 * @brief 测试：全局并发限制在多个 limiter 实例之间共享。
 *
 * max_global_concurrency = 1 表示：
 * 不管创建了多少个 HttpConcurrencyLimiter 实例，
 * 全局同时最多只允许 1 个请求通过。
 *
 * 这里专门创建两个 limiter：
 * - first_limiter
 * - second_limiter
 *
 * 目的：
 * 验证全局限制不是单个 limiter 内部的局部限制，
 * 而是所有 limiter 实例共享的限制。
 *
 * 预期：
 * 1. first_limiter 获取成功；
 * 2. second_limiter 获取失败；
 * 3. 释放 first 后，second_limiter 再次获取成功。
 */
void test_global_limit_is_shared_between_limiter_instances()
{
    sylar::http::HttpConcurrencyLimitOptions options;

    // 全局最大并发数限制为 1。
    options.max_global_concurrency = 1;

    // 创建两个不同的 limiter 实例。
    auto first_limiter = sylar::http::HttpConcurrencyLimiter::Create(options);
    auto second_limiter = sylar::http::HttpConcurrencyLimiter::Create(options);

    EXPECT_TRUE(first_limiter != nullptr);
    EXPECT_TRUE(second_limiter != nullptr);

    if (!first_limiter || !second_limiter)
    {
        return;
    }

    // 第一个 limiter 获取全局唯一名额。
    auto first = first_limiter->tryAcquire("endpoint-a");

    // 第二个 limiter 虽然是另一个实例，但全局名额已经被占用，
    // 所以这里应该失败。
    auto second = second_limiter->tryAcquire("endpoint-b");

    EXPECT_TRUE(first != nullptr);
    EXPECT_TRUE(second == nullptr);

    // 释放全局并发名额。
    first.reset();

    // 全局名额释放后，第二个 limiter 应该可以获取成功。
    auto third = second_limiter->tryAcquire("endpoint-b");

    EXPECT_TRUE(third != nullptr);
}

/**
 * @brief 测试：服务级 QPS 限制。
 *
 * max_service_qps = 1 表示：
 * 当前 limiter 实例内部，每秒最多允许 1 个请求通过。
 *
 * 预期：
 * 1. 第一次 acquire 成功；
 * 2. 第二次立即 acquire 失败；
 * 3. 等待 token 补充后，第三次 acquire 成功。
 */
void test_service_qps_limit_refills_tokens()
{
    sylar::http::HttpConcurrencyLimitOptions options;
    options.max_service_qps = 1;

    auto limiter = sylar::http::HttpConcurrencyLimiter::Create(options);
    EXPECT_TRUE(limiter != nullptr);
    if (!limiter)
    {
        return;
    }

    auto first = limiter->tryAcquire("endpoint-a");
    EXPECT_TRUE(first != nullptr);

    first.reset();
    auto second = limiter->tryAcquire("endpoint-b");
    EXPECT_TRUE(second == nullptr);

    usleep(1100 * 1000);
    auto third = limiter->tryAcquire("endpoint-b");
    EXPECT_TRUE(third != nullptr);
}

/**
 * @brief 测试：Endpoint 级 QPS 限制按 endpoint 单独计数。
 */
void test_endpoint_qps_limit_is_counted_per_endpoint()
{
    sylar::http::HttpConcurrencyLimitOptions options;
    options.max_endpoint_qps = 1;

    auto limiter = sylar::http::HttpConcurrencyLimiter::Create(options);
    EXPECT_TRUE(limiter != nullptr);
    if (!limiter)
    {
        return;
    }

    auto first = limiter->tryAcquire("endpoint-a");
    EXPECT_TRUE(first != nullptr);

    first.reset();
    auto second = limiter->tryAcquire("endpoint-a");
    auto third = limiter->tryAcquire("endpoint-b");
    EXPECT_TRUE(second == nullptr);
    EXPECT_TRUE(third != nullptr);

    usleep(1100 * 1000);
    auto fourth = limiter->tryAcquire("endpoint-a");
    EXPECT_TRUE(fourth != nullptr);
}

/**
 * @brief 测试：全局 QPS 限制在多个 limiter 实例之间共享。
 */
void test_global_qps_limit_is_shared_between_limiter_instances()
{
    sylar::http::HttpConcurrencyLimitOptions options;
    options.max_global_qps = 1;

    auto first_limiter = sylar::http::HttpConcurrencyLimiter::Create(options);
    auto second_limiter = sylar::http::HttpConcurrencyLimiter::Create(options);
    EXPECT_TRUE(first_limiter != nullptr);
    EXPECT_TRUE(second_limiter != nullptr);
    if (!first_limiter || !second_limiter)
    {
        return;
    }

    auto first = first_limiter->tryAcquire("endpoint-a");
    EXPECT_TRUE(first != nullptr);

    first.reset();
    auto second = second_limiter->tryAcquire("endpoint-b");
    EXPECT_TRUE(second == nullptr);

    usleep(1100 * 1000);
    auto third = second_limiter->tryAcquire("endpoint-b");
    EXPECT_TRUE(third != nullptr);
}

/**
 * @brief 测试：Endpoint QPS 拒绝请求时，会归还已经占用的全局 token。
 */
void test_endpoint_qps_reject_refunds_global_token()
{
    sylar::http::HttpConcurrencyLimitOptions options;
    options.max_global_qps = 2;
    options.max_endpoint_qps = 1;

    auto limiter = sylar::http::HttpConcurrencyLimiter::Create(options);
    EXPECT_TRUE(limiter != nullptr);
    if (!limiter)
    {
        return;
    }

    auto first = limiter->tryAcquire("endpoint-a");
    EXPECT_TRUE(first != nullptr);

    first.reset();
    auto second = limiter->tryAcquire("endpoint-a");
    EXPECT_TRUE(second == nullptr);

    auto third = limiter->tryAcquire("endpoint-b");
    EXPECT_TRUE(third != nullptr);
}

/**
 * @brief 执行所有测试用例。
 *
 * 当前一共测试 8 类情况：
 * 1. 默认不限流；
 * 2. 服务级限流；
 * 3. endpoint 级限流；
 * 4. 全局限流。
 * 5. 服务级 QPS 限流；
 * 6. endpoint 级 QPS 限流；
 * 7. 全局 QPS 限流。
 * 8. QPS 组合限流失败时归还已占用 token。
 */
void run_tests()
{
    test_unlimited_limiter_allows_multiple_requests();
    test_service_limit_rejects_until_guard_releases();
    test_endpoint_limit_is_counted_per_endpoint();
    test_global_limit_is_shared_between_limiter_instances();
    test_service_qps_limit_refills_tokens();
    test_endpoint_qps_limit_is_counted_per_endpoint();
    test_global_qps_limit_is_shared_between_limiter_instances();
    test_endpoint_qps_reject_refunds_global_token();

    SYLAR_LOG_INFO(g_logger) << "run_tests over";
}

} // namespace

/**
 * @brief 测试程序入口。
 *
 * 返回值：
 * - 0：所有 EXPECT_TRUE 都通过；
 * - 1：至少有一个 EXPECT_TRUE 失败。
 */
int main(int argc, char **argv)
{
    run_tests();

    return g_failures.load() == 0 ? 0 : 1;
}
