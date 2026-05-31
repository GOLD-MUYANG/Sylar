#include "sylar/http/http_connection.h"
#include "sylar/iomanager.h"
#include "sylar/log.h"
#include "sylar/util.h"
#include <atomic>
#include <string>
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

#define EXPECT_NE(lhs, rhs)                                                                        \
    do                                                                                             \
    {                                                                                              \
        auto _lhs = (lhs);                                                                         \
        auto _rhs = (rhs);                                                                         \
        if (_lhs == _rhs)                                                                          \
        {                                                                                          \
            ++g_failures;                                                                          \
            SYLAR_LOG_ERROR(g_logger) << "EXPECT_NE failed: " #lhs "=" << _lhs << " " #rhs "="     \
                                      << _rhs << " line=" << __LINE__;                             \
        }                                                                                          \
    } while (0)

namespace
{

struct AsyncResult
{
    int result = -1;
    uint64_t elapsed = 0;
};

bool wait_until(const std::function<bool()> &cb, uint64_t timeout_ms)
{
    uint64_t end = sylar::GetCurrentMS() + timeout_ms;
    while (sylar::GetCurrentMS() < end)
    {
        if (cb())
        {
            return true;
        }
        usleep(1000);
    }
    return cb();
}

sylar::http::HttpResult::ptr
request(sylar::http::HttpConnectionPool::ptr pool, const std::string &path, uint64_t timeout_ms)
{
    return pool->doGet(path, timeout_ms);
}

void record_request(sylar::http::HttpConnectionPool::ptr pool,
                    const std::string &path,
                    uint64_t timeout_ms,
                    AsyncResult *out)
{
    uint64_t begin = sylar::GetCurrentMS();
    auto r = request(pool, path, timeout_ms);
    out->elapsed = sylar::GetCurrentMS() - begin;
    out->result = r->result;
}

sylar::http::HttpConnectionPool::ptr
create_pool(uint32_t max_size, uint32_t max_alive_time, uint32_t max_request)
{
    return std::make_shared<sylar::http::HttpConnectionPool>("www.baidu.com", "", 80, false,
                                                             max_size, max_alive_time, max_request);
}

void test_do_get_baidu()
{
    auto pool = create_pool(2, 30000, 10);
    auto r = request(pool, "/", 3000);
    EXPECT_TRUE(r != nullptr);
    if (!r)
    {
        return;
    }
    EXPECT_EQ(r->result, (int)sylar::http::HttpResult::Error::OK);
    EXPECT_TRUE(r->response != nullptr);
}
// 连接池大小为 1，连接还没过期、没超过请求次数、没关闭时，归还后再次获取应该复用同一个 TCP
// 连接。判断方式是比较本地地址,如果端口一样，基本说明是同一个 socket。
void test_keepalive_reuse()
{
    auto pool = create_pool(1, 30000, 10);
    auto c1 = pool->getConnection();
    EXPECT_TRUE(c1 != nullptr);
    if (!c1)
    {
        return;
    }
    std::string id1 = c1->getSocket()->getLocalAddress()->toString();
    c1.reset();

    auto c2 = pool->getConnection();
    EXPECT_TRUE(c2 != nullptr);
    if (!c2)
    {
        return;
    }
    EXPECT_EQ(id1, c2->getSocket()->getLocalAddress()->toString());
}

// 连接存活时间超过 max_alive_time 后，不应该复用旧连接，而应该重新建立新连接。
void test_max_alive_time()
{
    auto pool = create_pool(1, 50, 10);
    auto c1 = pool->getConnection();
    EXPECT_TRUE(c1 != nullptr);
    if (!c1)
    {
        return;
    }
    std::string id1 = c1->getSocket()->getLocalAddress()->toString();
    c1.reset();
    usleep(80 * 1000);

    auto c2 = pool->getConnection();
    EXPECT_TRUE(c2 != nullptr);
    if (!c2)
    {
        return;
    }
    EXPECT_NE(id1, c2->getSocket()->getLocalAddress()->toString());
}
// 一个连接达到最大请求次数后，不应该继续复用。
void test_max_request()
{
    auto pool = create_pool(1, 30000, 1);
    auto c1 = pool->getConnection();
    EXPECT_TRUE(c1 != nullptr);
    if (!c1)
    {
        return;
    }
    std::string id1 = c1->getSocket()->getLocalAddress()->toString();
    c1.reset();

    auto c2 = pool->getConnection();
    EXPECT_TRUE(c2 != nullptr);
    if (!c2)
    {
        return;
    }
    EXPECT_NE(id1, c2->getSocket()->getLocalAddress()->toString());
}
// 先拿走唯一连接，让连接池满。然后异步执行：record_request(pool, "/id", 50,
// &waiting);也就是让另一个协程/任务尝试从池里拿连接并发送请求，超时时间 50ms。
// 池满时，新的请求不会立刻乱建连接；它会等待连接归还。如果等待超过
// timeout，就返回 POOL_GET_CONNECTION 错误。
void test_wait_timeout_when_pool_full()
{
    auto pool = create_pool(1, 30000, 10);
    auto hold = pool->getConnection();
    EXPECT_TRUE(hold != nullptr);
    if (!hold)
    {
        return;
    }
    std::atomic<int> done(0);
    AsyncResult waiting;

    sylar::IOManager::GetThis()->schedule(
        [&]()
        {
            record_request(pool, "/id", 50, &waiting);
            ++done;
        });

    EXPECT_TRUE(wait_until([&]() { return done.load() == 1; }, 2000));
    EXPECT_EQ(waiting.result, (int)sylar::http::HttpResult::Error::POOL_GET_CONNECTION);
    EXPECT_TRUE(waiting.elapsed >= 40);
}

// 池满时，等待者不会失败；只要连接在超时前归还，等待者应该能拿到连接，而且拿到的是之前那个连接。
void test_wait_success_when_connection_returned()
{
    auto pool = create_pool(1, 30000, 10);
    auto hold = pool->getConnection();
    EXPECT_TRUE(hold != nullptr);
    if (!hold)
    {
        return;
    }
    std::string hold_id = hold->getSocket()->getLocalAddress()->toString();
    std::atomic<int> done(0);
    bool got_connection = false;
    uint64_t elapsed = 0;
    std::string wait_id;

    sylar::IOManager::GetThis()->schedule(
        [&]()
        {
            uint64_t begin = sylar::GetCurrentMS();
            auto conn = pool->getConnection();
            elapsed = sylar::GetCurrentMS() - begin;
            got_connection = conn != nullptr;
            if (conn)
            {
                wait_id = conn->getSocket()->getLocalAddress()->toString();
            }
            ++done;
        });
    usleep(120 * 1000);
    hold.reset();

    EXPECT_TRUE(wait_until([&]() { return done.load() == 1; }, 2000));
    EXPECT_TRUE(got_connection);
    EXPECT_TRUE(elapsed >= 80);
    EXPECT_EQ(hold_id, wait_id);
}

// 如果连接已经被主动关闭，归还连接池后不能被复用。
void test_connection_close_not_reused()
{
    auto pool = create_pool(1, 30000, 10);
    auto c1 = pool->getConnection();
    EXPECT_TRUE(c1 != nullptr);
    if (!c1)
    {
        return;
    }
    std::string id1 = c1->getSocket()->getLocalAddress()->toString();
    c1->close();
    c1.reset();

    auto c2 = pool->getConnection();
    EXPECT_TRUE(c2 != nullptr);
    if (!c2)
    {
        return;
    }
    EXPECT_NE(id1, c2->getSocket()->getLocalAddress()->toString());
}

void run_tests()
{
    test_do_get_baidu();
    test_keepalive_reuse();
    test_max_alive_time();
    test_max_request();
    test_wait_timeout_when_pool_full();
    test_wait_success_when_connection_returned();
    test_connection_close_not_reused();
    SYLAR_LOG_INFO(g_logger) << "run_tests over";
}

} // namespace

int main(int argc, char **argv)
{
    {
        sylar::IOManager iom(4);
        iom.schedule(run_tests);
    }
    return g_failures.load() == 0 ? 0 : 1;
}
