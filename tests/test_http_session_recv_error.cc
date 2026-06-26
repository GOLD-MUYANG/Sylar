#include "sylar/http/http_session.h"
#include "sylar/log.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <string>

static sylar::Logger::ptr g_logger = SYLAR_LOG_ROOT();
static std::atomic<int> g_failures(0);

#define EXPECT_NULL(expr)                                                                          \
    do                                                                                             \
    {                                                                                              \
        if ((expr) != nullptr)                                                                     \
        {                                                                                          \
            ++g_failures;                                                                          \
            SYLAR_LOG_ERROR(g_logger) << "EXPECT_NULL failed: " #expr " line=" << __LINE__;        \
        }                                                                                          \
    } while (0)

#define EXPECT_RECV_ERROR_EQ(lhs, rhs)                                                             \
    do                                                                                             \
    {                                                                                              \
        auto _lhs = (lhs);                                                                         \
        auto _rhs = (rhs);                                                                         \
        if (!(_lhs == _rhs))                                                                       \
        {                                                                                          \
            ++g_failures;                                                                          \
            SYLAR_LOG_ERROR(g_logger) << "EXPECT_RECV_ERROR_EQ failed: " #lhs "=" << (int)_lhs     \
                                      << " " #rhs "=" << (int)_rhs << " line=" << __LINE__;        \
        }                                                                                          \
    } while (0)

namespace
{

class FakeSocket : public sylar::Socket
{
public:
    typedef std::shared_ptr<FakeSocket> ptr;

    FakeSocket(const std::string &data, int error)
        : sylar::Socket(sylar::Socket::IPv4, sylar::Socket::TCP, 0), m_data(data), m_error(error)
    {
        m_isConnected = true;
    }

    int recv(void *buffer, size_t length, int flags = 0) override
    {
        (void)flags;
        if (m_error != 0)
        {
            errno = m_error;
            return -1;
        }
        if (m_data.empty() || m_offset >= m_data.size())
        {
            return 0;
        }

        size_t remaining = m_data.size() - m_offset;
        size_t to_copy = remaining < length ? remaining : length;
        memcpy(buffer, m_data.data() + m_offset, to_copy);
        m_offset += to_copy;
        return (int)to_copy;
    }

private:
    std::string m_data;
    size_t m_offset = 0;
    int m_error = 0;
};

void TestClientClosed()
{
    sylar::http::HttpSession session(std::make_shared<FakeSocket>("", 0));
    auto request = session.recvRequest();
    EXPECT_NULL(request.get());
    EXPECT_RECV_ERROR_EQ(session.getLastRecvRequestError(),
                         sylar::http::HttpSessionRecvRequestError::CLIENT_CLOSED);
}

void TestTimeout()
{
    sylar::http::HttpSession session(std::make_shared<FakeSocket>("", EAGAIN));
    auto request = session.recvRequest();
    EXPECT_NULL(request.get());
    EXPECT_RECV_ERROR_EQ(session.getLastRecvRequestError(),
                         sylar::http::HttpSessionRecvRequestError::TIMEOUT);
}

void TestParseError()
{
    sylar::http::HttpSession session(std::make_shared<FakeSocket>("@@@\r\n\r\n", 0));
    auto request = session.recvRequest();
    EXPECT_NULL(request.get());
    EXPECT_RECV_ERROR_EQ(session.getLastRecvRequestError(),
                         sylar::http::HttpSessionRecvRequestError::PARSE_ERROR);
}

} // namespace

int main()
{
    TestClientClosed();
    TestTimeout();
    TestParseError();
    return g_failures.load() == 0 ? 0 : 1;
}
