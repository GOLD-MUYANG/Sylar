#include "http_connection.h"
#include "http_parser.h"
#include "sylar/iomanager.h"
#include "sylar/log.h"
#include "sylar/thread.h"
#include "sylar/uri.h"
#include <list>

namespace sylar
{
namespace http
{

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

std::string HttpResult::toString() const
{
    std::stringstream ss;
    ss << "[HttpResult result=" << result << " error=" << error
       << " response=" << (response ? response->toString() : "nullptr") << "]";
    return ss.str();
}

HttpConnection::HttpConnection(Socket::ptr sock, bool owner) : SocketStream(sock, owner)
{
}

HttpConnection::~HttpConnection()
{
    SYLAR_LOG_DEBUG(g_logger) << "HttpConnection::~HttpConnection";
}

HttpResponse::ptr HttpConnection::recvResponse()
{
    HttpResponseParser::ptr parser(new HttpResponseParser);
    uint64_t buff_size = HttpRequestParser::GetHttpRequestBufferSize();
    // uint64_t buff_size = 100;
    std::shared_ptr<char> buffer(new char[buff_size + 1], [](char *ptr) { delete[] ptr; });
    char *data = buffer.get();
    int offset = 0;
    do
    {
        int len = read(data + offset, buff_size - offset);
        if (len <= 0)
        {
            close();
            return nullptr;
        }
        len += offset;
        data[len] = '\0';
        size_t nparse = parser->execute(data, len, false);
        if (parser->hasError())
        {
            close();
            return nullptr;
        }
        offset = len - nparse;
        if (offset == (int)buff_size)
        {
            close();
            return nullptr;
        }
        if (parser->isFinished())
        {
            break;
        }
    } while (true);
    auto &client_parser = parser->getParser();
    if (client_parser.chunked)
    {
        std::string body;
        int len = offset;
        do
        {
            do
            {
                int rt = read(data + len, buff_size - len);
                if (rt <= 0)
                {
                    close();
                    return nullptr;
                }
                len += rt;
                data[len] = '\0';
                size_t nparse = parser->execute(data, len, true);
                if (parser->hasError())
                {
                    close();
                    return nullptr;
                }
                len -= nparse;
                if (len == (int)buff_size)
                {
                    close();
                    return nullptr;
                }
            } while (!parser->isFinished());
            len -= 2;

            SYLAR_LOG_INFO(g_logger) << "content_len=" << client_parser.content_len;
            if (client_parser.content_len <= len)
            {
                body.append(data, client_parser.content_len);
                memmove(data, data + client_parser.content_len, len - client_parser.content_len);
                len -= client_parser.content_len;
            }
            else
            {
                body.append(data, len);
                int left = client_parser.content_len - len;
                while (left > 0)
                {
                    int rt = read(data, left > (int)buff_size ? (int)buff_size : left);
                    if (rt <= 0)
                    {
                        close();
                        return nullptr;
                    }
                    body.append(data, rt);
                    left -= rt;
                }
                len = 0;
            }
        } while (!client_parser.chunks_done);
        parser->getData()->setBody(body);
    }
    else
    {
        int64_t length = parser->getContentLength();
        if (length > 0)
        {
            std::string body;
            body.resize(length);

            int len = 0;
            if (length >= offset)
            {
                memcpy(&body[0], data, offset);
                len = offset;
            }
            else
            {
                memcpy(&body[0], data, length);
                len = length;
            }
            length -= offset;
            if (length > 0)
            {
                if (readFixSize(&body[len], length) <= 0)
                {
                    close();
                    return nullptr;
                }
            }
            parser->getData()->setBody(body);
        }
    }
    if (client_parser.close)
    {
        parser->getData()->setClose(true);
    }
    else if (parser->getData()->getVersion() == 0x11)
    {
        parser->getData()->setClose(false);
    }
    return parser->getData();
}

int HttpConnection::sendRequest(HttpRequest::ptr rsp)
{
    std::stringstream ss;
    ss << *rsp;
    std::string data = ss.str();
    return writeFixSize(data.c_str(), data.size());
}

HttpResult::ptr HttpConnection::DoGet(const std::string &url,
                                      uint64_t timeout_ms,
                                      const std::map<std::string, std::string> &headers,
                                      const std::string &body)
{
    Uri::ptr uri = Uri::Create(url);
    if (!uri)
    {
        return std::make_shared<HttpResult>((int)HttpResult::Error::INVALID_URL, nullptr,
                                            "invalid url: " + url);
    }
    return DoGet(uri, timeout_ms, headers, body);
}

HttpResult::ptr HttpConnection::DoGet(Uri::ptr uri,
                                      uint64_t timeout_ms,
                                      const std::map<std::string, std::string> &headers,
                                      const std::string &body)
{
    return DoRequest(HttpMethod::GET, uri, timeout_ms, headers, body);
}

HttpResult::ptr HttpConnection::DoPost(const std::string &url,
                                       uint64_t timeout_ms,
                                       const std::map<std::string, std::string> &headers,
                                       const std::string &body)
{
    Uri::ptr uri = Uri::Create(url);
    if (!uri)
    {
        return std::make_shared<HttpResult>((int)HttpResult::Error::INVALID_URL, nullptr,
                                            "invalid url: " + url);
    }
    return DoPost(uri, timeout_ms, headers, body);
}

HttpResult::ptr HttpConnection::DoPost(Uri::ptr uri,
                                       uint64_t timeout_ms,
                                       const std::map<std::string, std::string> &headers,
                                       const std::string &body)
{
    return DoRequest(HttpMethod::POST, uri, timeout_ms, headers, body);
}

HttpResult::ptr HttpConnection::DoRequest(HttpMethod method,
                                          const std::string &url,
                                          uint64_t timeout_ms,
                                          const std::map<std::string, std::string> &headers,
                                          const std::string &body)
{
    Uri::ptr uri = Uri::Create(url);
    if (!uri)
    {
        return std::make_shared<HttpResult>((int)HttpResult::Error::INVALID_URL, nullptr,
                                            "invalid url: " + url);
    }
    return DoRequest(method, uri, timeout_ms, headers, body);
}

HttpResult::ptr HttpConnection::DoRequest(HttpMethod method,
                                          Uri::ptr uri,
                                          uint64_t timeout_ms,
                                          const std::map<std::string, std::string> &headers,
                                          const std::string &body)
{
    HttpRequest::ptr req = std::make_shared<HttpRequest>();
    req->setPath(uri->getPath());
    req->setQuery(uri->getQuery());
    req->setFragment(uri->getFragment());
    req->setMethod(method);
    bool has_host = false;
    for (auto &i : headers)
    {
        if (strcasecmp(i.first.c_str(), "connection") == 0)
        {
            if (strcasecmp(i.second.c_str(), "keep-alive") == 0)
            {
                req->setClose(false);
            }
            continue;
        }

        if (!has_host && strcasecmp(i.first.c_str(), "host") == 0)
        {
            has_host = !i.second.empty();
        }

        req->setHeader(i.first, i.second);
    }
    if (!has_host)
    {
        req->setHeader("Host", uri->getHost());
    }
    req->setBody(body);
    return DoRequest(req, uri, timeout_ms);
}

HttpResult::ptr HttpConnection::DoRequest(HttpRequest::ptr req, Uri::ptr uri, uint64_t timeout_ms)
{
    bool is_ssl = uri->getScheme() == "https";
    Address::ptr addr = uri->createAddress();
    if (!addr)
    {
        return std::make_shared<HttpResult>((int)HttpResult::Error::INVALID_HOST, nullptr,
                                            "invalid host: " + uri->getHost());
    }
    Socket::ptr sock = is_ssl ? SSLSocket::CreateTCP(addr) : Socket::CreateTCP(addr);
    if (!sock)
    {
        return std::make_shared<HttpResult>((int)HttpResult::Error::CREATE_SOCKET_ERROR, nullptr,
                                            "create socket fail: " + addr->toString() +
                                                " errno=" + std::to_string(errno) +
                                                " errstr=" + std::string(strerror(errno)));
    }
    if (!sock->connect(addr))
    {
        return std::make_shared<HttpResult>((int)HttpResult::Error::CONNECT_FAIL, nullptr,
                                            "connect fail: " + addr->toString());
    }
    sock->setRecvTimeOut(timeout_ms);
    HttpConnection::ptr conn = std::make_shared<HttpConnection>(sock);
    int rt = conn->sendRequest(req);
    if (rt == 0)
    {
        return std::make_shared<HttpResult>((int)HttpResult::Error::SEND_CLOSE_BY_PEER, nullptr,
                                            "send request closed by peer: " + addr->toString());
    }
    if (rt < 0)
    {
        return std::make_shared<HttpResult>(
            (int)HttpResult::Error::SEND_SOCKET_ERROR, nullptr,
            "send request socket error errno=" + std::to_string(errno) +
                " errstr=" + std::string(strerror(errno)));
    }
    auto rsp = conn->recvResponse();
    if (!rsp)
    {
        return std::make_shared<HttpResult>((int)HttpResult::Error::TIMEOUT, nullptr,
                                            "recv response timeout: " + addr->toString() +
                                                " timeout_ms:" + std::to_string(timeout_ms));
    }
    return std::make_shared<HttpResult>((int)HttpResult::Error::OK, rsp, "ok");
}
HttpConnectionPool::ptr HttpConnectionPool::Create(const std::string &uri,
                                                   const std::string &vhost,
                                                   uint32_t max_size,
                                                   uint32_t max_alive_time,
                                                   uint32_t max_request)
{
    Uri::ptr turi = Uri::Create(uri);
    if (!turi)
    {
        SYLAR_LOG_ERROR(g_logger) << "invalid uri=" << uri;
    }
    return std::make_shared<HttpConnectionPool>(turi->getHost(), vhost, turi->getPort(),
                                                turi->getScheme() == "https", max_size,
                                                max_alive_time, max_request);
}

HttpConnectionPool::HttpConnectionPool(const std::string &host,
                                       const std::string &vhost,
                                       uint32_t port,
                                       bool is_https,
                                       uint32_t max_size,
                                       uint32_t max_alive_time,
                                       uint32_t max_request)
    : m_host(host), m_vhost(vhost), m_port(port ? port : (is_https ? 443 : 80)),
      m_maxSize(max_size), m_maxAliveTime(max_alive_time), m_maxRequest(max_request),
      m_isHttps(is_https)
{
}

HttpConnection::ptr HttpConnectionPool::getConnection()
{
    //(uint64_t)-1.表示 无限等待。
    // 从连接池里拿一个连接，如果没有空闲连接，且连接数已满，就一直等，直到有连接释放出来。
    return getConnection((uint64_t)-1);
}

// 判断一个连接还能不能继续用。
bool HttpConnectionPool::isConnectionReusable(HttpConnection *ptr, uint64_t now_ms) const
{
    // socket 已经断开
    if (!ptr->isConnected())
    {
        return false;
    }
    // 连接活得太久了，超过最大存活时间
    if (m_maxAliveTime && (ptr->m_createTime + m_maxAliveTime <= now_ms))
    {
        return false;
    }
    // 这个连接处理请求次数太多了
    if (m_maxRequest && (ptr->m_request >= m_maxRequest))
    {
        return false;
    }
    return true;
}

// 唤醒等待连接的协程
void HttpConnectionPool::notifyWaiter()
{
    while (!m_waiters.empty())
    {
        auto waiter = m_waiters.front();
        m_waiters.pop_front();
        if (waiter->timeout || waiter->notified)
        {
            continue;
        }
        waiter->notified = true;
        waiter->scheduler->schedule(waiter->fiber);
        break;
    }
}

/**
开始取连接
    |
    v
检查空闲连接队列 m_conns
    |
    |-- 有可复用连接 -> 返回
    |
    |-- 有不可复用连接 -> 删除，m_total--
    |
    v
没有可用连接
    |
    |-- 总连接数没达到上限 -> 创建新连接
    |
    |-- 总连接数达到上限 -> 当前协程等待
                                  |
                                  |-- 被释放连接唤醒 -> 重新尝试
                                  |
                                  |-- 等待超时 -> 返回 nullptr
*/
HttpConnection::ptr HttpConnectionPool::getConnection(uint64_t timeout_ms)
{
    uint64_t start_ms = sylar::GetCurrentMS();
    std::vector<HttpConnection *> invalid_conns;
    while (true)
    {
        uint64_t now_ms = sylar::GetCurrentMS();
        HttpConnection *ptr = nullptr;
        bool need_create = false;
        Waiter::ptr waiter;
        // 先从空闲连接队列里找连接
        {
            MutexType::Lock lock(m_mutex);
            while (!m_conns.empty())
            {
                auto conn = *m_conns.begin();
                m_conns.pop_front();
                if (!isConnectionReusable(conn, now_ms))
                {
                    // 注意这里没有立刻 delete，而是先放入 invalid_conns。
                    // 原因是当前还在锁里：
                    // 它选择先把坏连接摘出来，释放锁之后再删除：
                    invalid_conns.push_back(conn);
                    --m_total;
                    continue;
                }
                ptr = conn;
                break;
            }
            // 没有空闲连接时，判断能不能新建
            if (!ptr)
            {
                if (m_maxSize == 0 || m_total < (int32_t)m_maxSize)
                {
                    ++m_total;
                    need_create = true;
                }
                else
                {
                    if (timeout_ms == 0 ||
                        (timeout_ms != (uint64_t)-1 && now_ms - start_ms >= timeout_ms))
                    {
                        break;
                    }
                    waiter.reset(new Waiter);
                    waiter->scheduler = sylar::Scheduler::GetThis();
                    waiter->fiber = sylar::Fiber::GetThis();
                    m_waiters.push_back(waiter);
                }
            }
        }

        for (auto i : invalid_conns)
        {
            delete i;
        }
        invalid_conns.clear();

        if (ptr)
        {
            return HttpConnection::ptr(
                ptr, std::bind(&HttpConnectionPool::ReleasePtr, std::placeholders::_1, this));
        }

        if (need_create)
        {
            IPAddress::ptr addr = Address::LookupAnyIPAddress(m_host);
            if (!addr)
            {
                SYLAR_LOG_ERROR(g_logger) << "get addr fail: " << m_host;
                MutexType::Lock lock(m_mutex);
                --m_total;
                notifyWaiter();
                return nullptr;
            }
            addr->setPort(m_port);
            Socket::ptr sock = m_isHttps ? SSLSocket::CreateTCP(addr) : Socket::CreateTCP(addr);
            if (!sock)
            {
                SYLAR_LOG_ERROR(g_logger) << "create sock fail: " << *addr;
                MutexType::Lock lock(m_mutex);
                --m_total;
                notifyWaiter();
                return nullptr;
            }
            if (!sock->connect(addr))
            {
                SYLAR_LOG_ERROR(g_logger) << "sock connect fail: " << *addr;
                MutexType::Lock lock(m_mutex);
                --m_total;
                notifyWaiter();
                return nullptr;
            }

            ptr = new HttpConnection(sock);
            ptr->m_createTime = sylar::GetCurrentMS();
            return HttpConnection::ptr(
                ptr, std::bind(&HttpConnectionPool::ReleasePtr, std::placeholders::_1, this));
        }
        // 等待逻辑：定时器 + 协程挂起
        if (waiter)
        {
            sylar::Timer::ptr timer;
            if (timeout_ms != (uint64_t)-1)
            {
                uint64_t left =
                    timeout_ms > (now_ms - start_ms) ? timeout_ms - (now_ms - start_ms) : 0;
                timer = sylar::IOManager::GetThis()->addTimer(left,
                                                              [this, waiter]()
                                                              {
                                                                  MutexType::Lock lock(m_mutex);
                                                                  if (waiter->notified)
                                                                  {
                                                                      return;
                                                                  }
                                                                  waiter->timeout = true;
                                                                  waiter->scheduler->schedule(
                                                                      waiter->fiber);
                                                              });
            }
            sylar::Fiber::YieldToHold();
            if (timer)
            {
                timer->cancel();
            }

            MutexType::Lock lock(m_mutex);
            if (!waiter->notified)
            {
                m_waiters.remove(waiter);
            }
            if (waiter->timeout)
            {
                return nullptr;
            }
        }
    }

    for (auto i : invalid_conns)
    {
        delete i;
    }
    return nullptr;
}

void HttpConnectionPool::ReleasePtr(HttpConnection *ptr, HttpConnectionPool *pool)
{
    ++ptr->m_request;
    uint64_t now_ms = sylar::GetCurrentMS();
    if (!pool->isConnectionReusable(ptr, now_ms))
    {
        delete ptr;
        MutexType::Lock lock(pool->m_mutex);
        --pool->m_total;
        pool->notifyWaiter();
        return;
    }
    MutexType::Lock lock(pool->m_mutex);
    pool->m_conns.push_back(ptr);
    pool->notifyWaiter();
}

HttpResult::ptr HttpConnectionPool::doGet(const std::string &url,
                                          uint64_t timeout_ms,
                                          const std::map<std::string, std::string> &headers,
                                          const std::string &body)
{
    return doRequest(HttpMethod::GET, url, timeout_ms, headers, body);
}

HttpResult::ptr HttpConnectionPool::doGet(Uri::ptr uri,
                                          uint64_t timeout_ms,
                                          const std::map<std::string, std::string> &headers,
                                          const std::string &body)
{
    std::stringstream ss;
    ss << uri->getPath() << (uri->getQuery().empty() ? "" : "?") << uri->getQuery()
       << (uri->getFragment().empty() ? "" : "#") << uri->getFragment();
    return doGet(ss.str(), timeout_ms, headers, body);
}

HttpResult::ptr HttpConnectionPool::doPost(const std::string &url,
                                           uint64_t timeout_ms,
                                           const std::map<std::string, std::string> &headers,
                                           const std::string &body)
{
    return doRequest(HttpMethod::POST, url, timeout_ms, headers, body);
}

HttpResult::ptr HttpConnectionPool::doPost(Uri::ptr uri,
                                           uint64_t timeout_ms,
                                           const std::map<std::string, std::string> &headers,
                                           const std::string &body)
{
    std::stringstream ss;
    ss << uri->getPath() << (uri->getQuery().empty() ? "" : "?") << uri->getQuery()
       << (uri->getFragment().empty() ? "" : "#") << uri->getFragment();
    return doPost(ss.str(), timeout_ms, headers, body);
}

HttpResult::ptr HttpConnectionPool::doRequest(HttpMethod method,
                                              const std::string &url,
                                              uint64_t timeout_ms,
                                              const std::map<std::string, std::string> &headers,
                                              const std::string &body)
{
    HttpRequest::ptr req = std::make_shared<HttpRequest>();
    req->setPath(url);
    req->setMethod(method);
    req->setClose(false);
    bool has_host = false;
    for (auto &i : headers)
    {
        if (strcasecmp(i.first.c_str(), "connection") == 0)
        {
            if (strcasecmp(i.second.c_str(), "keep-alive") == 0)
            {
                req->setClose(false);
            }
            continue;
        }

        if (!has_host && strcasecmp(i.first.c_str(), "host") == 0)
        {
            has_host = !i.second.empty();
        }

        req->setHeader(i.first, i.second);
    }
    if (!has_host)
    {
        if (m_vhost.empty())
        {
            req->setHeader("Host", m_host);
        }
        else
        {
            req->setHeader("Host", m_vhost);
        }
    }
    req->setBody(body);
    return doRequest(req, timeout_ms);
}

HttpResult::ptr HttpConnectionPool::doRequest(HttpMethod method,
                                              Uri::ptr uri,
                                              uint64_t timeout_ms,
                                              const std::map<std::string, std::string> &headers,
                                              const std::string &body)
{
    std::stringstream ss;
    ss << uri->getPath() << (uri->getQuery().empty() ? "" : "?") << uri->getQuery()
       << (uri->getFragment().empty() ? "" : "#") << uri->getFragment();
    return doRequest(method, ss.str(), timeout_ms, headers, body);
}

HttpResult::ptr HttpConnectionPool::doRequest(HttpRequest::ptr req, uint64_t timeout_ms)
{
    auto conn = getConnection(timeout_ms);
    if (!conn)
    {
        return std::make_shared<HttpResult>((int)HttpResult::Error::POOL_GET_CONNECTION, nullptr,
                                            "pool host:" + m_host +
                                                " port:" + std::to_string(m_port));
    }
    auto sock = conn->getSocket();
    if (!sock)
    {
        return std::make_shared<HttpResult>(
            (int)HttpResult::Error::POOL_INVALID_CONNECTION, nullptr,
            "pool host:" + m_host + " port:" + std::to_string(m_port));
    }
    sock->setRecvTimeOut(timeout_ms);
    int rt = conn->sendRequest(req);
    if (rt == 0)
    {
        return std::make_shared<HttpResult>((int)HttpResult::Error::SEND_CLOSE_BY_PEER, nullptr,
                                            "send request closed by peer: " +
                                                sock->getRemoteAddress()->toString());
    }
    if (rt < 0)
    {
        return std::make_shared<HttpResult>(
            (int)HttpResult::Error::SEND_SOCKET_ERROR, nullptr,
            "send request socket error errno=" + std::to_string(errno) +
                " errstr=" + std::string(strerror(errno)));
    }
    auto rsp = conn->recvResponse();
    if (!rsp)
    {
        return std::make_shared<HttpResult>(
            (int)HttpResult::Error::TIMEOUT, nullptr,
            "recv response timeout: " + sock->getRemoteAddress()->toString() +
                " timeout_ms:" + std::to_string(timeout_ms));
    }
    if (rsp->isClose())
    {
        conn->close();
    }
    return std::make_shared<HttpResult>((int)HttpResult::Error::OK, rsp, "ok");
}

} // namespace http
} // namespace sylar
