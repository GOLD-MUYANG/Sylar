#include "sylar/http/http_client.h"
#include "sylar/hook.h"
#include "sylar/iomanager.h"
#include "sylar/log.h"
#include "sylar/util.h"
#include <arpa/inet.h>
#include <atomic>
#include <cctype>
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

class TestHttpServer
{
public:
    TestHttpServer()
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

    ~TestHttpServer()
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

    std::string getLastMethod() const
    {
        return m_lastMethod;
    }

    std::string getLastBody() const
    {
        return m_lastBody;
    }

    int getFlakyCount() const
    {
        return m_flakyCount.load();
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
            std::thread(&TestHttpServer::handle_client, this, client).detach();
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

        std::string method;
        std::string path;
        size_t first_space = req.find(' ');
        size_t second_space = first_space == std::string::npos ? std::string::npos
                                                               : req.find(' ', first_space + 1);
        if (first_space != std::string::npos && second_space != std::string::npos)
        {
            method = req.substr(0, first_space);
            path = req.substr(first_space + 1, second_space - first_space - 1);
        }

        size_t header_end = req.find("\r\n\r\n");
        size_t content_length = find_content_length(req);
        while (!m_stop && header_end != std::string::npos &&
               req.size() < header_end + 4 + content_length)
        {
            int rt = recv_f(client, buf, sizeof(buf), 0);
            if (rt <= 0)
            {
                close_f(client);
                return;
            }
            req.append(buf, rt);
        }

        m_lastMethod = method;
        if (header_end != std::string::npos)
        {
            m_lastBody = req.substr(header_end + 4, content_length);
        }

        if (path == "/slow")
        {
            usleep(300 * 1000);
            send_response(client, 200, "OK", "slow");
        }
        else if (path == "/flaky")
        {
            int count = ++m_flakyCount;
            if (count == 1)
            {
                send_response(client, 503, "Service Unavailable", "try-again");
            }
            else
            {
                send_response(client, 200, "OK", "retry-ok");
            }
        }
        else if (path == "/error")
        {
            send_response(client, 503, "Service Unavailable", "down");
        }
        else if (method == "POST")
        {
            send_response(client, 200, "OK", "post:" + m_lastBody);
        }
        else
        {
            send_response(client, 200, "OK", "get-ok");
        }
        close_f(client);
    }

    size_t find_content_length(const std::string &req)
    {
        std::string lower = req;
        for (size_t i = 0; i < lower.size(); ++i)
        {
            lower[i] = ::tolower(lower[i]);
        }
        const std::string key = "content-length:";
        size_t pos = lower.find(key);
        if (pos == std::string::npos)
        {
            return 0;
        }
        pos += key.size();
        while (pos < req.size() && req[pos] == ' ')
        {
            ++pos;
        }
        return ::atoi(req.c_str() + pos);
    }

    void send_response(int client, int status, const std::string &reason, const std::string &body)
    {
        std::stringstream ss;
        ss << "HTTP/1.1 " << status << " " << reason << "\r\n"
           << "connection: close\r\n"
           << "content-length: " << body.size() << "\r\n\r\n"
           << body;
        std::string rsp = ss.str();
        send_f(client, rsp.c_str(), rsp.size(), 0);
    }

private:
    int m_fd = -1;
    uint16_t m_port = 0;
    std::atomic<bool> m_stop{false};
    std::atomic<int> m_flakyCount{0};
    std::thread m_thread;
    std::string m_lastMethod;
    std::string m_lastBody;
};

sylar::http::HttpClient::ptr create_client(uint16_t port)
{
    return sylar::http::HttpClient::Create("http://127.0.0.1:" + std::to_string(port), "", 2,
                                           30000, 10);
}

void test_client_get_and_post_use_pool()
{
    TestHttpServer server;
    usleep(50 * 1000);

    auto client = create_client(server.getPort());
    EXPECT_TRUE(client != nullptr);
    if (!client)
    {
        return;
    }

    auto get_result = client->get("/ok", 1000);
    EXPECT_TRUE(get_result != nullptr);
    EXPECT_EQ(get_result->result, (int)sylar::http::HttpResult::Error::OK);
    EXPECT_TRUE(get_result->response != nullptr);
    if (get_result->response)
    {
        EXPECT_EQ(get_result->response->getBody(), std::string("get-ok"));
    }

    auto post_result = client->post("/submit", 1000, {}, "payload");
    EXPECT_TRUE(post_result != nullptr);
    EXPECT_EQ(post_result->result, (int)sylar::http::HttpResult::Error::OK);
    EXPECT_TRUE(post_result->response != nullptr);
    if (post_result->response)
    {
        EXPECT_EQ(post_result->response->getBody(), std::string("post:payload"));
    }
    EXPECT_EQ(server.getLastMethod(), std::string("POST"));
    EXPECT_EQ(server.getLastBody(), std::string("payload"));
}

void test_client_maps_http_status_error()
{
    TestHttpServer server;
    usleep(50 * 1000);

    auto client = create_client(server.getPort());
    EXPECT_TRUE(client != nullptr);
    if (!client)
    {
        return;
    }

    auto result = client->get("/error", 1000);
    EXPECT_TRUE(result != nullptr);
    EXPECT_EQ(result->result, (int)sylar::http::HttpResult::Error::HTTP_STATUS_ERROR);
    EXPECT_TRUE(result->response != nullptr);
    if (result->response)
    {
        EXPECT_EQ((int)result->response->getStatus(), 503);
    }
}

void test_client_maps_recv_timeout()
{
    TestHttpServer server;
    usleep(50 * 1000);

    auto client = create_client(server.getPort());
    EXPECT_TRUE(client != nullptr);
    if (!client)
    {
        return;
    }

    sylar::http::HttpRequestOptions options = sylar::http::HttpRequestOptions::FromTimeout(1000);
    options.recv_timeout_ms = 50;
    uint64_t begin = sylar::GetCurrentMS();
    auto result = client->get("/slow", options);
    uint64_t elapsed = sylar::GetCurrentMS() - begin;

    EXPECT_TRUE(result != nullptr);
    EXPECT_EQ(result->result, (int)sylar::http::HttpResult::Error::RECV_TIMEOUT);
    EXPECT_LT(elapsed, 150ul);
}

void test_static_get_reports_invalid_url()
{
    auto result = sylar::http::HttpClient::Get("not-a-valid-url", 1000);
    EXPECT_TRUE(result != nullptr);
    EXPECT_EQ(result->result, (int)sylar::http::HttpResult::Error::INVALID_URL);
}

void test_client_retries_idempotent_5xx()
{
    TestHttpServer server;
    usleep(50 * 1000);

    auto client = create_client(server.getPort());
    EXPECT_TRUE(client != nullptr);
    if (!client)
    {
        return;
    }

    sylar::http::HttpRequestOptions options = sylar::http::HttpRequestOptions::FromTimeout(1000);
    sylar::http::HttpRetryOptions retry_options;
    retry_options.max_retry = 1;
    retry_options.retry_interval_ms = 10;

    auto result = client->get("/flaky", options, retry_options);
    EXPECT_TRUE(result != nullptr);
    EXPECT_EQ(result->result, (int)sylar::http::HttpResult::Error::OK);
    EXPECT_TRUE(result->response != nullptr);
    if (result->response)
    {
        EXPECT_EQ(result->response->getBody(), std::string("retry-ok"));
    }
    EXPECT_EQ(server.getFlakyCount(), 2);
}

void run_tests()
{
    test_client_get_and_post_use_pool();
    test_client_maps_http_status_error();
    test_client_maps_recv_timeout();
    test_static_get_reports_invalid_url();
    test_client_retries_idempotent_5xx();
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
