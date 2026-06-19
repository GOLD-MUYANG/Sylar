#include "sylar/http/http_load_balance_client.h"
#include "sylar/hook.h"
#include "sylar/iomanager.h"
#include "sylar/log.h"
#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
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

class NamedHttpServer
{
public:
    explicit NamedHttpServer(const std::string &name, uint32_t response_delay_ms = 0)
        : m_name(name), m_responseDelayMs(response_delay_ms)
    {
        m_fd = socket_f(AF_INET, SOCK_STREAM, 0);
        EXPECT_TRUE(m_fd >= 0);

        int reuse = 1;
        setsockopt_f(m_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        int rt = ::bind(m_fd, (const sockaddr *)&addr, sizeof(addr));
        EXPECT_EQ(rt, 0);
        rt = ::listen(m_fd, 128);
        EXPECT_EQ(rt, 0);

        socklen_t len = sizeof(addr);
        rt = ::getsockname(m_fd, (sockaddr *)&addr, &len);
        EXPECT_EQ(rt, 0);
        m_port = ntohs(addr.sin_port);

        m_thread = std::thread([this]() { accept_loop(); });
    }

    ~NamedHttpServer()
    {
        m_stop = true;
        if (m_fd >= 0)
        {
            ::shutdown(m_fd, SHUT_RDWR);
            close_f(m_fd);
            m_fd = -1;
        }
        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }

    uint16_t getPort() const
    {
        return m_port;
    }

private:
    void accept_loop()
    {
        while (!m_stop)
        {
            int client = accept_f(m_fd, nullptr, nullptr);
            if (client < 0)
            {
                if (!m_stop)
                {
                    usleep(1000);
                }
                continue;
            }
            std::thread(&NamedHttpServer::handle_client, this, client).detach();
        }
    }

    void handle_client(int client)
    {
        std::string req;
        char buf[1024];
        while (!m_stop && req.find("\r\n\r\n") == std::string::npos)
        {
            int rt = recv_f(client, buf, sizeof(buf), 0);
            if (rt <= 0)
            {
                close_f(client);
                return;
            }
            req.append(buf, rt);
        }

        std::stringstream ss;
        ss << "HTTP/1.1 200 OK\r\n"
           << "connection: close\r\n"
           << "content-length: " << m_name.size() << "\r\n\r\n"
           << m_name;
        std::string rsp = ss.str();
        if (m_responseDelayMs > 0)
        {
            usleep(m_responseDelayMs * 1000);
        }
        send_f(client, rsp.c_str(), rsp.size(), 0);
        close_f(client);
    }

private:
    std::string m_name;
    uint32_t m_responseDelayMs = 0;
    int m_fd = -1;
    uint16_t m_port = 0;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};

sylar::http::HttpEndpoint::ptr create_endpoint(uint16_t port, uint32_t weight = 1)
{
    return sylar::http::HttpEndpoint::Create("127.0.0.1", port, false, weight,
                                             sylar::http::HttpEndpointStatus::UP, "", 2, 30000,
                                             10);
}

void expect_ok_body(const sylar::http::HttpResult::ptr &result, const std::string &body)
{
    EXPECT_TRUE(result != nullptr);
    if (!result)
    {
        return;
    }
    EXPECT_EQ(result->result, (int)sylar::http::HttpResult::Error::OK);
    EXPECT_TRUE(result->response != nullptr);
    if (!result->response)
    {
        return;
    }
    EXPECT_EQ(result->response->getBody(), body);
}

void test_round_robin_rotates_between_endpoints()
{
    NamedHttpServer first("first");
    NamedHttpServer second("second");
    usleep(50 * 1000);

    std::vector<sylar::http::HttpEndpoint::ptr> endpoints;
    endpoints.push_back(create_endpoint(first.getPort()));
    endpoints.push_back(create_endpoint(second.getPort()));

    auto client = sylar::http::HttpLoadBalanceClient::Create(
        endpoints, sylar::http::HttpLoadBalanceStrategy::ROUND_ROBIN);
    EXPECT_TRUE(client != nullptr);
    if (!client)
    {
        return;
    }

    auto first_result = client->get("/ok", 1000);
    auto second_result = client->get("/ok", 1000);
    auto third_result = client->get("/ok", 1000);

    expect_ok_body(first_result, "first");
    expect_ok_body(second_result, "second");
    expect_ok_body(third_result, "first");
}

void test_down_endpoint_is_skipped()
{
    NamedHttpServer available("available");
    usleep(50 * 1000);

    auto down = sylar::http::HttpEndpoint::Create("127.0.0.1", available.getPort(), false, 1,
                                                  sylar::http::HttpEndpointStatus::DOWN, "", 2,
                                                  30000, 10);
    std::vector<sylar::http::HttpEndpoint::ptr> endpoints;
    endpoints.push_back(down);
    endpoints.push_back(create_endpoint(available.getPort()));

    auto client = sylar::http::HttpLoadBalanceClient::Create(
        endpoints, sylar::http::HttpLoadBalanceStrategy::ROUND_ROBIN);
    EXPECT_TRUE(client != nullptr);
    if (!client)
    {
        return;
    }

    auto result = client->get("/ok", 1000);
    expect_ok_body(result, "available");
}

void test_random_strategy_uses_available_endpoint()
{
    NamedHttpServer available("random-ok");
    usleep(50 * 1000);

    std::vector<sylar::http::HttpEndpoint::ptr> endpoints;
    endpoints.push_back(create_endpoint(available.getPort()));

    auto client = sylar::http::HttpLoadBalanceClient::Create(
        endpoints, sylar::http::HttpLoadBalanceStrategy::RANDOM);
    EXPECT_TRUE(client != nullptr);
    if (!client)
    {
        return;
    }

    auto result = client->get("/ok", 1000);
    expect_ok_body(result, "random-ok");
}

void test_weighted_round_robin_uses_endpoint_weight()
{
    NamedHttpServer first("first");
    NamedHttpServer second("second");
    usleep(50 * 1000);

    std::vector<sylar::http::HttpEndpoint::ptr> endpoints;
    endpoints.push_back(create_endpoint(first.getPort(), 2));
    endpoints.push_back(create_endpoint(second.getPort(), 1));

    auto client = sylar::http::HttpLoadBalanceClient::Create(
        endpoints, sylar::http::HttpLoadBalanceStrategy::WEIGHTED_ROUND_ROBIN);
    EXPECT_TRUE(client != nullptr);
    if (!client)
    {
        return;
    }

    expect_ok_body(client->get("/ok", 1000), "first");
    expect_ok_body(client->get("/ok", 1000), "first");
    expect_ok_body(client->get("/ok", 1000), "second");
    expect_ok_body(client->get("/ok", 1000), "first");
}

void test_least_connection_uses_less_busy_endpoint()
{
    NamedHttpServer slow("slow", 200);
    NamedHttpServer fast("fast");
    usleep(50 * 1000);

    std::vector<sylar::http::HttpEndpoint::ptr> endpoints;
    endpoints.push_back(create_endpoint(slow.getPort()));
    endpoints.push_back(create_endpoint(fast.getPort()));

    auto client = sylar::http::HttpLoadBalanceClient::Create(
        endpoints, sylar::http::HttpLoadBalanceStrategy::LEAST_CONNECTION);
    EXPECT_TRUE(client != nullptr);
    if (!client)
    {
        return;
    }

    sylar::http::HttpResult::ptr slow_result;
    sylar::http::HttpResult::ptr fast_result;

    sylar::IOManager *iom = sylar::IOManager::GetThis();
    iom->schedule([&]() { slow_result = client->get("/ok", 1000); });
    usleep(50 * 1000);
    fast_result = client->get("/ok", 1000);

    usleep(250 * 1000);
    expect_ok_body(fast_result, "fast");
    expect_ok_body(slow_result, "slow");
}

void test_health_check_marks_failed_endpoint_down()
{
    NamedHttpServer available("available");
    std::shared_ptr<NamedHttpServer> unavailable(new NamedHttpServer("unavailable"));
    uint16_t unavailable_port = unavailable->getPort();
    usleep(50 * 1000);

    std::vector<sylar::http::HttpEndpoint::ptr> endpoints;
    auto up = create_endpoint(available.getPort());
    auto down = create_endpoint(unavailable_port);
    endpoints.push_back(up);
    endpoints.push_back(down);

    auto client = sylar::http::HttpLoadBalanceClient::Create(
        endpoints, sylar::http::HttpLoadBalanceStrategy::ROUND_ROBIN);
    EXPECT_TRUE(client != nullptr);
    if (!client)
    {
        return;
    }

    unavailable.reset();
    size_t available_count =
        client->checkHealth("/ok", sylar::http::HttpRequestOptions::FromTimeout(200));

    EXPECT_EQ(available_count, (size_t)1);
    EXPECT_EQ((int)up->getStatus(), (int)sylar::http::HttpEndpointStatus::UP);
    EXPECT_EQ((int)down->getStatus(), (int)sylar::http::HttpEndpointStatus::DOWN);
    expect_ok_body(client->get("/ok", 1000), "available");
}

void run_tests()
{
    test_round_robin_rotates_between_endpoints();
    test_down_endpoint_is_skipped();
    test_random_strategy_uses_available_endpoint();
    test_weighted_round_robin_uses_endpoint_weight();
    test_least_connection_uses_less_busy_endpoint();
    test_health_check_marks_failed_endpoint_down();
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
