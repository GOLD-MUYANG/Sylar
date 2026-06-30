#include "sylar/http/http_circuit_breaker.h"
#include "sylar/http/http.h"
#include "sylar/log.h"

#include <atomic>
#include <unistd.h>

static sylar::Logger::ptr g_logger = SYLAR_LOG_ROOT();
static std::atomic<int> g_failures(0);

#define EXPECT_TRUE(expr)                                                                          \
    do                                                                                             \
    {                                                                                              \
        if (!(expr))                                                                               \
        {                                                                                          \
            ++g_failures;                                                                          \
            SYLAR_LOG_ERROR(g_logger) << "EXPECT_TRUE failed: " #expr << " line=" << __LINE__;     \
        }                                                                                          \
    } while (0)

#define EXPECT_EQ(lhs, rhs)                                                                        \
    do                                                                                             \
    {                                                                                              \
        auto _lhs = (lhs);                                                                         \
        auto _rhs = (rhs);                                                                         \
        if (!(_lhs == _rhs))                                                                       \
        {                                                                                          \
            ++g_failures;                                                                          \
            SYLAR_LOG_ERROR(g_logger) << "EXPECT_EQ failed: " #lhs "=" << _lhs << " " #rhs "="     \
                                      << _rhs << " line=" << __LINE__;                             \
        }                                                                                          \
    } while (0)

namespace
{

sylar::http::HttpResult::ptr make_result(sylar::http::HttpResult::Error error)
{
    return std::make_shared<sylar::http::HttpResult>((int)error, nullptr, "test result");
}

sylar::http::HttpResult::ptr make_status_result(sylar::http::HttpStatus status)
{
    sylar::http::HttpResponse::ptr response(new sylar::http::HttpResponse);
    response->setStatus(status);
    return std::make_shared<sylar::http::HttpResult>(
        (int)sylar::http::HttpResult::Error::HTTP_STATUS_ERROR, response, "status error");
}

void test_consecutive_failures_open_circuit()
{
    sylar::http::HttpCircuitBreakerOptions options;
    options.enabled = true;
    options.consecutive_failure_threshold = 2;
    options.open_timeout_ms = 1000;

    auto breaker = sylar::http::HttpCircuitBreaker::Create(options);
    EXPECT_TRUE(breaker != nullptr);
    if (!breaker)
    {
        return;
    }

    EXPECT_TRUE(breaker->tryAcquire("endpoint-a") != nullptr);
    breaker->onRequestComplete("endpoint-a",
                               make_result(sylar::http::HttpResult::Error::CONNECT_FAIL));
    EXPECT_EQ((int)breaker->getState("endpoint-a"),
              (int)sylar::http::HttpCircuitBreakerState::CLOSED);

    EXPECT_TRUE(breaker->tryAcquire("endpoint-a") != nullptr);
    breaker->onRequestComplete("endpoint-a",
                               make_result(sylar::http::HttpResult::Error::RECV_TIMEOUT));
    EXPECT_EQ((int)breaker->getState("endpoint-a"),
              (int)sylar::http::HttpCircuitBreakerState::OPEN);
    EXPECT_TRUE(breaker->tryAcquire("endpoint-a") == nullptr);
}

void test_open_circuit_allows_half_open_probe_after_timeout()
{
    sylar::http::HttpCircuitBreakerOptions options;
    options.enabled = true;
    options.consecutive_failure_threshold = 1;
    options.open_timeout_ms = 20;

    auto breaker = sylar::http::HttpCircuitBreaker::Create(options);
    EXPECT_TRUE(breaker != nullptr);
    if (!breaker)
    {
        return;
    }

    EXPECT_TRUE(breaker->tryAcquire("endpoint-a") != nullptr);
    breaker->onRequestComplete("endpoint-a",
                               make_result(sylar::http::HttpResult::Error::CONNECT_FAIL));
    EXPECT_EQ((int)breaker->getState("endpoint-a"),
              (int)sylar::http::HttpCircuitBreakerState::OPEN);

    EXPECT_TRUE(breaker->tryAcquire("endpoint-a") == nullptr);
    usleep(30 * 1000);

    EXPECT_TRUE(breaker->tryAcquire("endpoint-a") != nullptr);
    EXPECT_EQ((int)breaker->getState("endpoint-a"),
              (int)sylar::http::HttpCircuitBreakerState::HALF_OPEN);

    breaker->onRequestComplete("endpoint-a", make_result(sylar::http::HttpResult::Error::OK));
    EXPECT_EQ((int)breaker->getState("endpoint-a"),
              (int)sylar::http::HttpCircuitBreakerState::CLOSED);
    EXPECT_TRUE(breaker->tryAcquire("endpoint-a") != nullptr);
}

void test_get_state_reports_half_open_after_open_timeout()
{
    sylar::http::HttpCircuitBreakerOptions options;
    options.enabled = true;
    options.consecutive_failure_threshold = 1;
    options.open_timeout_ms = 20;

    auto breaker = sylar::http::HttpCircuitBreaker::Create(options);
    EXPECT_TRUE(breaker != nullptr);
    if (!breaker)
    {
        return;
    }

    EXPECT_TRUE(breaker->tryAcquire("endpoint-a") != nullptr);
    breaker->onRequestComplete("endpoint-a",
                               make_result(sylar::http::HttpResult::Error::CONNECT_FAIL));
    EXPECT_EQ((int)breaker->getState("endpoint-a"),
              (int)sylar::http::HttpCircuitBreakerState::OPEN);

    usleep(30 * 1000);
    EXPECT_EQ((int)breaker->getState("endpoint-a"),
              (int)sylar::http::HttpCircuitBreakerState::HALF_OPEN);
    EXPECT_TRUE(breaker->tryAcquire("endpoint-a") != nullptr);
}

void test_half_open_limits_probe_and_reopens_on_failure()
{
    sylar::http::HttpCircuitBreakerOptions options;
    options.enabled = true;
    options.consecutive_failure_threshold = 1;
    options.open_timeout_ms = 1;
    options.half_open_max_requests = 1;

    auto breaker = sylar::http::HttpCircuitBreaker::Create(options);
    EXPECT_TRUE(breaker != nullptr);
    if (!breaker)
    {
        return;
    }

    EXPECT_TRUE(breaker->tryAcquire("endpoint-a") != nullptr);
    breaker->onRequestComplete("endpoint-a",
                               make_result(sylar::http::HttpResult::Error::CONNECT_FAIL));
    usleep(2 * 1000);

    EXPECT_TRUE(breaker->tryAcquire("endpoint-a") != nullptr);
    EXPECT_TRUE(breaker->tryAcquire("endpoint-a") == nullptr);

    breaker->onRequestComplete("endpoint-a",
                               make_result(sylar::http::HttpResult::Error::CONNECT_FAIL));
    EXPECT_EQ((int)breaker->getState("endpoint-a"),
              (int)sylar::http::HttpCircuitBreakerState::OPEN);
}

void test_failure_rate_opens_circuit()
{
    sylar::http::HttpCircuitBreakerOptions options;
    options.enabled = true;
    options.consecutive_failure_threshold = 10;
    options.failure_rate_threshold = 50;
    options.failure_rate_min_request = 4;
    options.failure_window_size = 4;
    options.open_timeout_ms = 1000;

    auto breaker = sylar::http::HttpCircuitBreaker::Create(options);
    EXPECT_TRUE(breaker != nullptr);
    if (!breaker)
    {
        return;
    }

    breaker->onRequestComplete("endpoint-a", make_result(sylar::http::HttpResult::Error::OK));
    breaker->onRequestComplete("endpoint-a",
                               make_result(sylar::http::HttpResult::Error::CONNECT_FAIL));
    breaker->onRequestComplete("endpoint-a", make_status_result(sylar::http::HttpStatus::OK));
    breaker->onRequestComplete("endpoint-a",
                               make_status_result(sylar::http::HttpStatus::SERVICE_UNAVAILABLE));

    EXPECT_EQ((int)breaker->getState("endpoint-a"),
              (int)sylar::http::HttpCircuitBreakerState::OPEN);
}

void test_client_error_status_does_not_count_as_failure()
{
    sylar::http::HttpCircuitBreakerOptions options;
    options.enabled = true;
    options.consecutive_failure_threshold = 1;
    options.open_timeout_ms = 1000;

    auto breaker = sylar::http::HttpCircuitBreaker::Create(options);
    EXPECT_TRUE(breaker != nullptr);
    if (!breaker)
    {
        return;
    }

    breaker->onRequestComplete("endpoint-a", make_status_result(sylar::http::HttpStatus::NOT_FOUND));

    EXPECT_EQ((int)breaker->getState("endpoint-a"),
              (int)sylar::http::HttpCircuitBreakerState::CLOSED);
}

void test_disabled_breaker_never_opens()
{
    sylar::http::HttpCircuitBreakerOptions options;
    options.enabled = false;
    options.consecutive_failure_threshold = 1;

    auto breaker = sylar::http::HttpCircuitBreaker::Create(options);
    EXPECT_TRUE(breaker != nullptr);
    if (!breaker)
    {
        return;
    }

    breaker->onRequestComplete("endpoint-a",
                               make_result(sylar::http::HttpResult::Error::CONNECT_FAIL));
    EXPECT_EQ((int)breaker->getState("endpoint-a"),
              (int)sylar::http::HttpCircuitBreakerState::CLOSED);
    EXPECT_TRUE(breaker->tryAcquire("endpoint-a") != nullptr);
}

void run_tests()
{
    test_consecutive_failures_open_circuit();
    test_open_circuit_allows_half_open_probe_after_timeout();
    test_get_state_reports_half_open_after_open_timeout();
    test_half_open_limits_probe_and_reopens_on_failure();
    test_failure_rate_opens_circuit();
    test_client_error_status_does_not_count_as_failure();
    test_disabled_breaker_never_opens();

    SYLAR_LOG_INFO(g_logger) << "run_tests over";
}

} // namespace

int main(int argc, char **argv)
{
    run_tests();
    return g_failures.load() == 0 ? 0 : 1;
}
