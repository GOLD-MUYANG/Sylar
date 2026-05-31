#include "sylar/http/http_connection.h"
#include "sylar/fd_manager.h"
#include "sylar/hook.h"
#include "sylar/iomanager.h"
#include "sylar/log.h"
#include "sylar/util.h"
#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <netinet/in.h>
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

#define EXPECT_GE(lhs, rhs)                                                                        \
    do                                                                                             \
    {                                                                                              \
        auto _lhs = (lhs);                                                                         \
        auto _rhs = (rhs);                                                                         \
        if (!(_lhs >= _rhs))                                                                       \
        {                                                                                          \
            ++g_failures;                                                                          \
            SYLAR_LOG_ERROR(g_logger) << "EXPECT_GE failed: " #lhs "=" << _lhs << " " #rhs "="     \
                                      << _rhs << " line=" << __LINE__;                             \
        }                                                                                          \
    } while (0)

#define EXPECT_LT(lhs, rhs)                                                                        \
    do                                                                                             \
    {                                                                                              \
        auto _lhs = (lhs);                                                                         \
        auto _rhs = (rhs);                                                                         \
        if (!(_lhs < _rhs))                                                                        \
        {                                                                                          \
            ++g_failures;                                                                          \
            SYLAR_LOG_ERROR(g_logger) << "EXPECT_LT failed: " #lhs "=" << _lhs << " " #rhs "="     \
                                      << _rhs << " line=" << __LINE__;                             \
        }                                                                                          \
    } while (0)

namespace
{

class SlowHttpServer
{
public:
    SlowHttpServer(uint64_t response_delay_ms = 300) : m_responseDelayMs(response_delay_ms)
    {
        m_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_TRUE(m_fd >= 0);

        int reuse = 1;
        ::setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

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

    ~SlowHttpServer()
    {
        m_stop = true;
        if (m_fd >= 0)
        {
            ::shutdown(m_fd, SHUT_RDWR);
            ::close(m_fd);
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

    bool hasRequest() const
    {
        return m_requestReceived.load();
    }

private:
    void accept_loop()
    {
        while (!m_stop)
        {
            int client = ::accept(m_fd, nullptr, nullptr);
            if (client < 0)
            {
                if (!m_stop)
                {
                    usleep(1000);
                }
                continue;
            }
            std::thread(&SlowHttpServer::handle_client, this, client).detach();
        }
    }

    void handle_client(int client)
    {
        std::string req;
        char buf[1024];
        while (!m_stop && req.find("\r\n\r\n") == std::string::npos)
        {
            int rt = ::recv(client, buf, sizeof(buf), 0);
            if (rt <= 0)
            {
                ::close(client);
                return;
            }
            req.append(buf, rt);
        }

        m_requestReceived = true;
        usleep(m_responseDelayMs * 1000);
        const char *rsp =
            "HTTP/1.1 200 OK\r\nconnection: close\r\ncontent-length: 0\r\n\r\n";
        ::send(client, rsp, strlen(rsp), 0);
        ::close(client);
    }

private:
    int m_fd = -1;
    uint16_t m_port = 0;
    uint64_t m_responseDelayMs = 0;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_requestReceived{false};
    std::thread m_thread;
};

sylar::http::HttpConnectionPool::ptr create_pool(uint16_t port)
{
    return std::make_shared<sylar::http::HttpConnectionPool>("127.0.0.1", "", port, false, 1,
                                                             30000, 10);
}

void test_request_options_from_timeout()
{
    auto options = sylar::http::HttpRequestOptions::FromTimeout(123);
    EXPECT_EQ(options.connect_timeout_ms, 123ul);
    EXPECT_EQ(options.send_timeout_ms, 123ul);
    EXPECT_EQ(options.recv_timeout_ms, 123ul);
    EXPECT_EQ(options.total_timeout_ms, 123ul);
}

void test_socket_send_timeout_can_be_set()
{
    EXPECT_TRUE(sylar::is_hook_enable());
    SlowHttpServer server;
    usleep(50 * 1000);
    auto pool = create_pool(server.getPort());
    auto conn = pool->getConnection();
    EXPECT_TRUE(conn != nullptr);
    if (!conn)
    {
        return;
    }
    auto sock = conn->getSocket();
    EXPECT_TRUE(sock != nullptr);
    if (!sock)
    {
        return;
    }
    EXPECT_TRUE(sylar::FdMgr::GetInstance()->get(sock->getSocket()) != nullptr);

    sock->setSendTimeOut(50);
    EXPECT_EQ(sock->getSendTimeOut(), 50l);
}

void test_recv_timeout_option_limits_waiting_for_response()
{
    SlowHttpServer server;
    usleep(50 * 1000);
    auto pool = create_pool(server.getPort());

    auto options = sylar::http::HttpRequestOptions::FromTimeout(1000);
    options.recv_timeout_ms = 50;

    uint64_t begin = sylar::GetCurrentMS();
    auto r = pool->doGet("/", options);
    uint64_t elapsed = sylar::GetCurrentMS() - begin;

    EXPECT_TRUE(r != nullptr);
    if (!r)
    {
        return;
    }
    EXPECT_EQ(r->result, (int)sylar::http::HttpResult::Error::TIMEOUT);
    EXPECT_TRUE(server.hasRequest());
    EXPECT_TRUE(r->error.find("recv_timeout_ms:50") != std::string::npos);
    EXPECT_TRUE(r->error.find("total_timeout_ms:1000") != std::string::npos);
    EXPECT_GE(elapsed, 40ul);
    EXPECT_LT(elapsed, 150ul);
}

void test_total_timeout_option_limits_waiting_for_response()
{
    SlowHttpServer server;
    usleep(50 * 1000);
    auto pool = create_pool(server.getPort());

    sylar::http::HttpRequestOptions options;
    options.connect_timeout_ms = 1000;
    options.send_timeout_ms = 1000;
    options.recv_timeout_ms = 1000;
    options.total_timeout_ms = 50;

    uint64_t begin = sylar::GetCurrentMS();
    auto r = pool->doGet("/", options);
    uint64_t elapsed = sylar::GetCurrentMS() - begin;

    EXPECT_TRUE(r != nullptr);
    if (!r)
    {
        return;
    }
    EXPECT_EQ(r->result, (int)sylar::http::HttpResult::Error::TIMEOUT);
    EXPECT_TRUE(server.hasRequest());
    EXPECT_TRUE(r->error.find("recv_timeout_ms:1000") != std::string::npos);
    EXPECT_TRUE(r->error.find("total_timeout_ms:50") != std::string::npos);
    EXPECT_GE(elapsed, 40ul);
    EXPECT_LT(elapsed, 150ul);
}

void run_tests()
{
    test_request_options_from_timeout();
    test_socket_send_timeout_can_be_set();
    test_recv_timeout_option_limits_waiting_for_response();
    test_total_timeout_option_limits_waiting_for_response();
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
