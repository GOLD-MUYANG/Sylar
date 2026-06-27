#include "sylar/http/http_client.h"
#include "sylar/http/http_connection.h"
#include "sylar/hook.h"
#include "sylar/iomanager.h"
#include "sylar/log.h"
#include "sylar/util.h"

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

#define EXPECT_TRUE(expr)                                                                       \
    do                                                                                          \
    {                                                                                           \
        if (!(expr))                                                                            \
        {                                                                                       \
            ++g_failures;                                                                       \
            SYLAR_LOG_ERROR(g_logger) << "EXPECT_TRUE failed: " #expr << " line=" << __LINE__; \
        }                                                                                       \
    } while (0)

#define EXPECT_EQ(lhs, rhs)                                                                     \
    do                                                                                          \
    {                                                                                           \
        auto _lhs = (lhs);                                                                      \
        auto _rhs = (rhs);                                                                      \
        if (!(_lhs == _rhs))                                                                    \
        {                                                                                       \
            ++g_failures;                                                                       \
            SYLAR_LOG_ERROR(g_logger) << "EXPECT_EQ failed: " #lhs "=" << _lhs << " " #rhs "="  \
                                      << _rhs << " line=" << __LINE__;                          \
        }                                                                                       \
    } while (0)

namespace
{

class SlowResponseServer
{
public:
    explicit SlowResponseServer(uint64_t response_delay_ms) : m_responseDelayMs(response_delay_ms)
    {
        m_fd = socket_f(AF_INET, SOCK_STREAM, 0);
        EXPECT_TRUE(m_fd >= 0);

        int reuse = 1;
        setsockopt_f(m_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in address;
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        EXPECT_EQ(::bind(m_fd, (const sockaddr *)&address, sizeof(address)), 0);
        EXPECT_EQ(::listen(m_fd, 16), 0);

        socklen_t length = sizeof(address);
        EXPECT_EQ(::getsockname(m_fd, (sockaddr *)&address, &length), 0);
        m_port = ntohs(address.sin_port);
        m_thread = std::thread(&SlowResponseServer::acceptLoop, this);
    }

    ~SlowResponseServer()
    {
        m_stop = true;
        if (m_fd >= 0)
        {
            shutdown(m_fd, SHUT_RDWR);
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

    bool hasRequest() const
    {
        return m_requestReceived.load();
    }

private:
    void acceptLoop()
    {
        while (!m_stop)
        {
            int client = accept_f(m_fd, nullptr, nullptr);
            if (client < 0)
            {
                continue;
            }
            std::thread(&SlowResponseServer::handleClient, this, client).detach();
        }
    }

    void handleClient(int client)
    {
        char buffer[1024];
        std::string request;
        while (request.find("\r\n\r\n") == std::string::npos)
        {
            int rt = recv_f(client, buffer, sizeof(buffer), 0);
            if (rt <= 0)
            {
                close_f(client);
                return;
            }
            request.append(buffer, rt);
        }

        m_requestReceived = true;
        usleep(m_responseDelayMs * 1000);
        const char *response =
            "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
        send_f(client, response, strlen(response), 0);
        close_f(client);
    }

private:
    uint64_t m_responseDelayMs = 0;
    int m_fd = -1;
    uint16_t m_port = 0;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_requestReceived{false};
    std::thread m_thread;
};

uint16_t PickClosedPort()
{
    int fd = socket_f(AF_INET, SOCK_STREAM, 0);
    EXPECT_TRUE(fd >= 0);

    sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    EXPECT_EQ(::bind(fd, (const sockaddr *)&address, sizeof(address)), 0);

    socklen_t length = sizeof(address);
    EXPECT_EQ(::getsockname(fd, (sockaddr *)&address, &length), 0);
    uint16_t port = ntohs(address.sin_port);
    close_f(fd);
    return port;
}

void test_connect_failure_is_marked_not_sent()
{
    const uint16_t port = PickClosedPort();
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/";

    auto result = sylar::http::HttpClient::Get(url, sylar::http::HttpRequestOptions::FromTimeout(80));

    EXPECT_TRUE(result != nullptr);
    if (!result)
    {
        return;
    }
    EXPECT_EQ((int)result->attempt.phase, (int)sylar::http::HttpAttemptPhase::NOT_SENT);
    EXPECT_TRUE(!result->attempt.request_bytes_started);
    EXPECT_TRUE(!result->attempt.may_have_submitted);
    EXPECT_EQ(std::string(sylar::http::HttpAttemptPhaseToString(result->attempt.phase)),
              std::string("not_sent"));
}

void test_recv_timeout_after_send_is_marked_unknown_submission()
{
    SlowResponseServer server(300);
    usleep(50 * 1000);

    auto options = sylar::http::HttpRequestOptions::FromTimeout(1000);
    options.recv_timeout_ms = 50;

    auto pool = std::make_shared<sylar::http::HttpConnectionPool>("127.0.0.1", "", server.getPort(),
                                                                  false, 1, 30000, 10);
    auto result = pool->doGet("/", options);

    EXPECT_TRUE(result != nullptr);
    if (!result)
    {
        return;
    }
    EXPECT_EQ(result->result, (int)sylar::http::HttpResult::Error::TIMEOUT);
    EXPECT_EQ((int)result->attempt.phase,
              (int)sylar::http::HttpAttemptPhase::RESPONSE_TIMEOUT_AFTER_SEND);
    EXPECT_TRUE(result->attempt.request_bytes_started);
    EXPECT_TRUE(result->attempt.request_bytes_completed);
    EXPECT_TRUE(result->attempt.may_have_submitted);
    EXPECT_TRUE(server.hasRequest());
}

void RunTests()
{
    test_connect_failure_is_marked_not_sent();
    test_recv_timeout_after_send_is_marked_unknown_submission();
}

} // namespace

int main()
{
    sylar::IOManager iom(1, false, "http-attempt-outcome-test");
    iom.schedule(RunTests);
    iom.stop();
    return g_failures == 0 ? 0 : 1;
}
