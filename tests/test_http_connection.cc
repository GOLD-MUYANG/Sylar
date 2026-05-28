#include "sylar/http/http_connection.h"
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

#define EXPECT_TRUE(expr)                                                                          \
    do                                                                                             \
    {                                                                                              \
        if (!(expr))                                                                               \
        {                                                                                          \
            ++g_failures;                                                                          \
            SYLAR_LOG_ERROR(g_logger) << "EXPECT_TRUE failed: " #expr                              \
                                      << " line=" << __LINE__;                                     \
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
            SYLAR_LOG_ERROR(g_logger) << "EXPECT_EQ failed: " #lhs "=" << _lhs                    \
                                      << " " #rhs "=" << _rhs << " line=" << __LINE__;           \
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
            SYLAR_LOG_ERROR(g_logger) << "EXPECT_NE failed: " #lhs "=" << _lhs                    \
                                      << " " #rhs "=" << _rhs << " line=" << __LINE__;           \
        }                                                                                          \
    } while (0)

namespace
{

struct AsyncResult
{
    int result = -1;
    uint64_t elapsed = 0;
    std::string body;
};

class LocalHttpServer
{
public:
    LocalHttpServer()
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

    ~LocalHttpServer()
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
            std::thread(&LocalHttpServer::handle_client, this, client).detach();
        }
    }

    void handle_client(int client)
    {
        while (!m_stop)
        {
            std::string req;
            char buf[1024];
            while (req.find("\r\n\r\n") == std::string::npos)
            {
                int rt = ::recv(client, buf, sizeof(buf), 0);
                if (rt <= 0)
                {
                    ::close(client);
                    return;
                }
                req.append(buf, rt);
            }

            std::string path = parse_path(req);
            if (path == "/sleep")
            {
                usleep(120 * 1000);
            }

            std::string conn_id = client_id(client);
            bool close_conn = path == "/close";
            std::stringstream ss;
            ss << "HTTP/1.1 200 OK\r\n"
               << "connection: " << (close_conn ? "close" : "keep-alive") << "\r\n"
               << "X-Conn-Id: " << conn_id << "\r\n"
               << "content-length: 0\r\n\r\n";
            std::string rsp = ss.str();
            ::send(client, rsp.c_str(), rsp.size(), 0);
            if (close_conn)
            {
                break;
            }
        }
        ::close(client);
    }

    std::string parse_path(const std::string &req)
    {
        size_t first_space = req.find(' ');
        if (first_space == std::string::npos)
        {
            return "/";
        }
        size_t second_space = req.find(' ', first_space + 1);
        if (second_space == std::string::npos)
        {
            return "/";
        }
        return req.substr(first_space + 1, second_space - first_space - 1);
    }

    std::string client_id(int client)
    {
        sockaddr_in addr;
        socklen_t len = sizeof(addr);
        memset(&addr, 0, sizeof(addr));
        if (::getpeername(client, (sockaddr *)&addr, &len))
        {
            return "unknown";
        }
        char ip[INET_ADDRSTRLEN];
        const char *rt = ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        std::stringstream ss;
        ss << (rt ? ip : "unknown") << ":" << ntohs(addr.sin_port);
        return ss.str();
    }

private:
    int m_fd = -1;
    uint16_t m_port = 0;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
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

sylar::http::HttpResult::ptr request(sylar::http::HttpConnectionPool::ptr pool,
                                     const std::string &path,
                                     uint64_t timeout_ms)
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
    if (r->response)
    {
        out->body = r->response->getHeader("X-Conn-Id");
    }
}

sylar::http::HttpConnectionPool::ptr create_pool(uint16_t port,
                                                 uint32_t max_size,
                                                 uint32_t max_alive_time,
                                                 uint32_t max_request)
{
    return std::make_shared<sylar::http::HttpConnectionPool>(
        "127.0.0.1", "", port, false, max_size, max_alive_time, max_request);
}

void test_keepalive_reuse(uint16_t port)
{
    auto pool = create_pool(port, 1, 30000, 10);
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

void test_max_alive_time(uint16_t port)
{
    auto pool = create_pool(port, 1, 50, 10);
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

void test_max_request(uint16_t port)
{
    auto pool = create_pool(port, 1, 30000, 1);
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

void test_wait_timeout_when_pool_full(uint16_t port)
{
    auto pool = create_pool(port, 1, 30000, 10);
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

void test_wait_success_when_connection_returned(uint16_t port)
{
    auto pool = create_pool(port, 1, 30000, 10);
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

void test_connection_close_not_reused(uint16_t port)
{
    auto pool = create_pool(port, 1, 30000, 10);
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
    LocalHttpServer server;
    uint16_t port = server.getPort();
    usleep(50 * 1000);

    test_keepalive_reuse(port);
    test_max_alive_time(port);
    test_max_request(port);
    test_wait_timeout_when_pool_full(port);
    test_wait_success_when_connection_returned(port);
    test_connection_close_not_reused(port);

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
