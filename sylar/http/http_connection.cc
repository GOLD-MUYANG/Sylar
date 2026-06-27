#include "http_connection.h"
#include "http_parser.h"
#include "sylar/iomanager.h"
#include "sylar/log.h"
#include "sylar/thread.h"
#include "sylar/uri.h"
#include <algorithm>
#include <list>

namespace sylar
{
namespace http
{

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

static uint64_t s_http_timeout_forever = (uint64_t)-1;
// GetTimeoutLeft() 计算一次请求的 总超时还剩多少时间。
static uint64_t GetTimeoutLeft(uint64_t start_ms, uint64_t total_timeout_ms)
{
    if (total_timeout_ms == s_http_timeout_forever)
    {
        return s_http_timeout_forever;
    }
    uint64_t now_ms = sylar::GetCurrentMS();
    uint64_t elapsed = now_ms >= start_ms ? now_ms - start_ms : 0;
    return elapsed >= total_timeout_ms ? 0 : total_timeout_ms - elapsed;
}
// MergeTimeout() 把某个阶段的超时和总超时剩余时间合并，取更严格的那个。
// 目的：某个阶段最多只能等待 min(阶段超时, 总超时剩余时间)。
// 例如：
// - recv_timeout_ms = 1000
// - total_timeout_ms 剩余 50
// 那么 recv 阶段最多只能等 50ms，而不是 1000ms。
static uint64_t
MergeTimeout(uint64_t stage_timeout_ms, uint64_t start_ms, uint64_t total_timeout_ms)
{
    uint64_t left = GetTimeoutLeft(start_ms, total_timeout_ms);
    if (stage_timeout_ms == s_http_timeout_forever)
    {
        return left;
    }
    if (left == s_http_timeout_forever)
    {
        return stage_timeout_ms;
    }
    return std::min(stage_timeout_ms, left);
}

const char *HttpAttemptPhaseToString(HttpAttemptPhase phase)
{
    switch (phase)
    {
    case HttpAttemptPhase::NOT_SENT:
        return "not_sent";
    case HttpAttemptPhase::SEND_FAILED_BEFORE_BODY:
        return "send_failed_before_body";
    case HttpAttemptPhase::PARTIAL_WRITE_OR_UNKNOWN:
        return "partial_write_or_unknown";
    case HttpAttemptPhase::RESPONSE_TIMEOUT_AFTER_SEND:
        return "response_timeout_after_send";
    case HttpAttemptPhase::RESPONSE_RECEIVED:
        return "response_received";
    default:
        return "unknown";
    }
}

static HttpResult::ptr MakeHttpResult(int result,
                                      HttpResponse::ptr response,
                                      const std::string &error,
                                      const HttpAttemptOutcome &attempt)
{
    HttpResult::ptr value = std::make_shared<HttpResult>(result, response, error);
    value->attempt = attempt;
    return value;
}

static SSLSocket::ClientOptions BuildTlsClientOptions(const std::string &host,
                                                      const HttpRequestOptions &options)
{
    SSLSocket::ClientOptions tls_options;
    tls_options.server_name = options.tls_server_name.empty() ? host : options.tls_server_name;
    tls_options.ca_file = options.tls_ca_file;
    tls_options.ca_path = options.tls_ca_path;
    tls_options.verify_peer = options.tls_verify_peer;
    return tls_options;
}

std::string HttpResult::toString() const
{
    std::stringstream ss;
    ss << "[HttpResult result=" << result << " error=" << error
       << " attempt_phase=" << HttpAttemptPhaseToString(attempt.phase)
       << " may_have_submitted=" << attempt.may_have_submitted
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
    return sendRequest(rsp, nullptr);
}

int HttpConnection::sendRequest(HttpRequest::ptr rsp, HttpAttemptOutcome *attempt)
{
    std::stringstream ss;
    ss << *rsp;
    std::string data = ss.str();
    size_t offset = 0;
    int64_t left = data.size();
    while (left > 0)
    {
        int64_t len = write(data.c_str() + offset, left);
        if (len <= 0)
        {
            if (attempt)
            {
                if (attempt->request_bytes_started)
                {
                    attempt->phase = HttpAttemptPhase::PARTIAL_WRITE_OR_UNKNOWN;
                    attempt->may_have_submitted = true;
                }
                else
                {
                    attempt->phase = HttpAttemptPhase::SEND_FAILED_BEFORE_BODY;
                    attempt->may_have_submitted = false;
                }
            }
            return len;
        }

        if (attempt)
        {
            attempt->request_bytes_started = true;
            attempt->request_bytes_sent += len;
        }
        offset += len;
        left -= len;
    }

    if (attempt)
    {
        attempt->request_bytes_completed = true;
        attempt->may_have_submitted = true;
    }
    return data.size();
}

HttpResult::ptr HttpConnection::DoGet(const std::string &url,
                                      uint64_t timeout_ms,
                                      const std::map<std::string, std::string> &headers,
                                      const std::string &body)
{
    return DoGet(url, HttpRequestOptions::FromTimeout(timeout_ms), headers, body);
}

HttpResult::ptr HttpConnection::DoGet(const std::string &url,
                                      const HttpRequestOptions &options,
                                      const std::map<std::string, std::string> &headers,
                                      const std::string &body)
{
    Uri::ptr uri = Uri::Create(url);
    if (!uri)
    {
        return std::make_shared<HttpResult>((int)HttpResult::Error::INVALID_URL, nullptr,
                                            "invalid url: " + url);
    }
    return DoGet(uri, options, headers, body);
}

HttpResult::ptr HttpConnection::DoGet(Uri::ptr uri,
                                      uint64_t timeout_ms,
                                      const std::map<std::string, std::string> &headers,
                                      const std::string &body)
{
    return DoGet(uri, HttpRequestOptions::FromTimeout(timeout_ms), headers, body);
}

HttpResult::ptr HttpConnection::DoGet(Uri::ptr uri,
                                      const HttpRequestOptions &options,
                                      const std::map<std::string, std::string> &headers,
                                      const std::string &body)
{
    return DoRequest(HttpMethod::GET, uri, options, headers, body);
}

HttpResult::ptr HttpConnection::DoPost(const std::string &url,
                                       uint64_t timeout_ms,
                                       const std::map<std::string, std::string> &headers,
                                       const std::string &body)
{
    return DoPost(url, HttpRequestOptions::FromTimeout(timeout_ms), headers, body);
}

HttpResult::ptr HttpConnection::DoPost(const std::string &url,
                                       const HttpRequestOptions &options,
                                       const std::map<std::string, std::string> &headers,
                                       const std::string &body)
{
    Uri::ptr uri = Uri::Create(url);
    if (!uri)
    {
        return std::make_shared<HttpResult>((int)HttpResult::Error::INVALID_URL, nullptr,
                                            "invalid url: " + url);
    }
    return DoPost(uri, options, headers, body);
}

HttpResult::ptr HttpConnection::DoPost(Uri::ptr uri,
                                       uint64_t timeout_ms,
                                       const std::map<std::string, std::string> &headers,
                                       const std::string &body)
{
    return DoPost(uri, HttpRequestOptions::FromTimeout(timeout_ms), headers, body);
}

HttpResult::ptr HttpConnection::DoPost(Uri::ptr uri,
                                       const HttpRequestOptions &options,
                                       const std::map<std::string, std::string> &headers,
                                       const std::string &body)
{
    return DoRequest(HttpMethod::POST, uri, options, headers, body);
}

HttpResult::ptr HttpConnection::DoRequest(HttpMethod method,
                                          const std::string &url,
                                          uint64_t timeout_ms,
                                          const std::map<std::string, std::string> &headers,
                                          const std::string &body)
{
    return DoRequest(method, url, HttpRequestOptions::FromTimeout(timeout_ms), headers, body);
}

HttpResult::ptr HttpConnection::DoRequest(HttpMethod method,
                                          const std::string &url,
                                          const HttpRequestOptions &options,
                                          const std::map<std::string, std::string> &headers,
                                          const std::string &body)
{
    Uri::ptr uri = Uri::Create(url);
    if (!uri)
    {
        return std::make_shared<HttpResult>((int)HttpResult::Error::INVALID_URL, nullptr,
                                            "invalid url: " + url);
    }
    return DoRequest(method, uri, options, headers, body);
}

HttpResult::ptr HttpConnection::DoRequest(HttpMethod method,
                                          Uri::ptr uri,
                                          uint64_t timeout_ms,
                                          const std::map<std::string, std::string> &headers,
                                          const std::string &body)
{
    return DoRequest(method, uri, HttpRequestOptions::FromTimeout(timeout_ms), headers, body);
}

HttpResult::ptr HttpConnection::DoRequest(HttpMethod method,
                                          Uri::ptr uri,
                                          const HttpRequestOptions &options,
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
    return DoRequest(req, uri, options);
}

HttpResult::ptr HttpConnection::DoRequest(HttpRequest::ptr req, Uri::ptr uri, uint64_t timeout_ms)
{
    return DoRequest(req, uri, HttpRequestOptions::FromTimeout(timeout_ms));
}

HttpResult::ptr
HttpConnection::DoRequest(HttpRequest::ptr req, Uri::ptr uri, const HttpRequestOptions &options)
{
    uint64_t start_ms = sylar::GetCurrentMS();
    HttpAttemptOutcome attempt;
    bool is_ssl = uri->getScheme() == "https";
    Address::ptr addr = uri->createAddress();
    if (!addr)
    {
        attempt.detail = "invalid_host";
        return MakeHttpResult((int)HttpResult::Error::INVALID_HOST, nullptr,
                              "invalid host: " + uri->getHost(), attempt);
    }
    Socket::ptr sock = is_ssl ? SSLSocket::CreateTCP(addr) : Socket::CreateTCP(addr);
    if (!sock)
    {
        attempt.detail = "create_socket_error";
        return MakeHttpResult((int)HttpResult::Error::CREATE_SOCKET_ERROR, nullptr,
                              "create socket fail: " + addr->toString() +
                                  " errno=" + std::to_string(errno) +
                                  " errstr=" + std::string(strerror(errno)),
                              attempt);
    }
    if (is_ssl)
    {
        auto ssl_socket = std::dynamic_pointer_cast<SSLSocket>(sock);
        ssl_socket->setClientOptions(BuildTlsClientOptions(uri->getHost(), options));
    }
    uint64_t connect_timeout_ms =
        MergeTimeout(options.connect_timeout_ms, start_ms, options.total_timeout_ms);
    if (!sock->connect(addr, connect_timeout_ms))
    {
        attempt.detail = is_ssl ? "connect_or_tls_handshake_fail" : "connect_fail";
        return MakeHttpResult((int)HttpResult::Error::CONNECT_FAIL, nullptr,
                              "connect fail: " + addr->toString(), attempt);
    }
    attempt.tcp_connected = true;
    attempt.tls_handshake_completed = is_ssl;
    sock->setSendTimeOut(MergeTimeout(options.send_timeout_ms, start_ms, options.total_timeout_ms));
    sock->setRecvTimeOut(MergeTimeout(options.recv_timeout_ms, start_ms, options.total_timeout_ms));
    HttpConnection::ptr conn = std::make_shared<HttpConnection>(sock);
    int rt = conn->sendRequest(req, &attempt);
    if (rt == 0)
    {
        return MakeHttpResult((int)HttpResult::Error::SEND_CLOSE_BY_PEER, nullptr,
                              "send request closed by peer: " + addr->toString(), attempt);
    }
    if (rt < 0)
    {
        return MakeHttpResult((int)HttpResult::Error::SEND_SOCKET_ERROR, nullptr,
                              "send request socket error errno=" + std::to_string(errno) +
                                  " errstr=" + std::string(strerror(errno)),
                              attempt);
    }
    sock->setRecvTimeOut(MergeTimeout(options.recv_timeout_ms, start_ms, options.total_timeout_ms));
    auto rsp = conn->recvResponse();
    if (!rsp)
    {
        attempt.phase = HttpAttemptPhase::RESPONSE_TIMEOUT_AFTER_SEND;
        attempt.may_have_submitted = true;
        return MakeHttpResult((int)HttpResult::Error::TIMEOUT, nullptr,
                              "recv response timeout: " + addr->toString() +
                                  " recv_timeout_ms:" + std::to_string(options.recv_timeout_ms) +
                                  " total_timeout_ms:" +
                                  std::to_string(options.total_timeout_ms),
                              attempt);
    }
    attempt.phase = HttpAttemptPhase::RESPONSE_RECEIVED;
    attempt.response_started = true;
    attempt.response_completed = true;
    attempt.http_status = (int)rsp->getStatus();
    return MakeHttpResult((int)HttpResult::Error::OK, rsp, "ok", attempt);
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
        return nullptr;
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
    return getConnection(timeout_ms, HttpRequestOptions());
}

HttpConnection::ptr HttpConnectionPool::getConnection(uint64_t timeout_ms,
                                                      const HttpRequestOptions &options)
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
            if (m_isHttps)
            {
                auto ssl_socket = std::dynamic_pointer_cast<SSLSocket>(sock);
                ssl_socket->setClientOptions(BuildTlsClientOptions(m_host, options));
            }
            uint64_t connect_timeout_ms = MergeTimeout(timeout_ms, start_ms, timeout_ms);
            if (!sock->connect(addr, connect_timeout_ms))
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
    return doGet(url, HttpRequestOptions::FromTimeout(timeout_ms), headers, body);
}

HttpResult::ptr HttpConnectionPool::doGet(const std::string &url,
                                          const HttpRequestOptions &options,
                                          const std::map<std::string, std::string> &headers,
                                          const std::string &body)
{
    return doRequest(HttpMethod::GET, url, options, headers, body);
}

HttpResult::ptr HttpConnectionPool::doGet(Uri::ptr uri,
                                          uint64_t timeout_ms,
                                          const std::map<std::string, std::string> &headers,
                                          const std::string &body)
{
    return doGet(uri, HttpRequestOptions::FromTimeout(timeout_ms), headers, body);
}

HttpResult::ptr HttpConnectionPool::doGet(Uri::ptr uri,
                                          const HttpRequestOptions &options,
                                          const std::map<std::string, std::string> &headers,
                                          const std::string &body)
{
    std::stringstream ss;
    ss << uri->getPath() << (uri->getQuery().empty() ? "" : "?") << uri->getQuery()
       << (uri->getFragment().empty() ? "" : "#") << uri->getFragment();
    return doGet(ss.str(), options, headers, body);
}

HttpResult::ptr HttpConnectionPool::doPost(const std::string &url,
                                           uint64_t timeout_ms,
                                           const std::map<std::string, std::string> &headers,
                                           const std::string &body)
{
    return doPost(url, HttpRequestOptions::FromTimeout(timeout_ms), headers, body);
}

HttpResult::ptr HttpConnectionPool::doPost(const std::string &url,
                                           const HttpRequestOptions &options,
                                           const std::map<std::string, std::string> &headers,
                                           const std::string &body)
{
    return doRequest(HttpMethod::POST, url, options, headers, body);
}

HttpResult::ptr HttpConnectionPool::doPost(Uri::ptr uri,
                                           uint64_t timeout_ms,
                                           const std::map<std::string, std::string> &headers,
                                           const std::string &body)
{
    return doPost(uri, HttpRequestOptions::FromTimeout(timeout_ms), headers, body);
}

HttpResult::ptr HttpConnectionPool::doPost(Uri::ptr uri,
                                           const HttpRequestOptions &options,
                                           const std::map<std::string, std::string> &headers,
                                           const std::string &body)
{
    std::stringstream ss;
    ss << uri->getPath() << (uri->getQuery().empty() ? "" : "?") << uri->getQuery()
       << (uri->getFragment().empty() ? "" : "#") << uri->getFragment();
    return doPost(ss.str(), options, headers, body);
}

HttpResult::ptr HttpConnectionPool::doRequest(HttpMethod method,
                                              const std::string &url,
                                              uint64_t timeout_ms,
                                              const std::map<std::string, std::string> &headers,
                                              const std::string &body)
{
    return doRequest(method, url, HttpRequestOptions::FromTimeout(timeout_ms), headers, body);
}

HttpResult::ptr HttpConnectionPool::doRequest(HttpMethod method,
                                              const std::string &url,
                                              const HttpRequestOptions &options,
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
    return doRequest(req, options);
}

HttpResult::ptr HttpConnectionPool::doRequest(HttpMethod method,
                                              Uri::ptr uri,
                                              uint64_t timeout_ms,
                                              const std::map<std::string, std::string> &headers,
                                              const std::string &body)
{
    return doRequest(method, uri, HttpRequestOptions::FromTimeout(timeout_ms), headers, body);
}

HttpResult::ptr HttpConnectionPool::doRequest(HttpMethod method,
                                              Uri::ptr uri,
                                              const HttpRequestOptions &options,
                                              const std::map<std::string, std::string> &headers,
                                              const std::string &body)
{
    std::stringstream ss;
    ss << uri->getPath() << (uri->getQuery().empty() ? "" : "?") << uri->getQuery()
       << (uri->getFragment().empty() ? "" : "#") << uri->getFragment();
    return doRequest(method, ss.str(), options, headers, body);
}

HttpResult::ptr HttpConnectionPool::doRequest(HttpRequest::ptr req, uint64_t timeout_ms)
{
    return doRequest(req, HttpRequestOptions::FromTimeout(timeout_ms));
}

HttpResult::ptr HttpConnectionPool::doRequest(HttpRequest::ptr req,
                                              const HttpRequestOptions &options)
{
    uint64_t start_ms = sylar::GetCurrentMS();
    HttpAttemptOutcome attempt;
    uint64_t connect_timeout_ms =
        MergeTimeout(options.connect_timeout_ms, start_ms, options.total_timeout_ms);
    auto conn = getConnection(connect_timeout_ms, options);
    if (!conn)
    {
        attempt.detail = m_isHttps ? "pool_get_connection_or_tls_fail" : "pool_get_connection";
        return MakeHttpResult((int)HttpResult::Error::POOL_GET_CONNECTION, nullptr,
                              "pool host:" + m_host + " port:" + std::to_string(m_port), attempt);
    }
    auto sock = conn->getSocket();
    if (!sock)
    {
        attempt.detail = "pool_invalid_connection";
        return MakeHttpResult((int)HttpResult::Error::POOL_INVALID_CONNECTION, nullptr,
                              "pool host:" + m_host + " port:" + std::to_string(m_port), attempt);
    }
    attempt.tcp_connected = true;
    attempt.tls_handshake_completed = m_isHttps;
    sock->setSendTimeOut(MergeTimeout(options.send_timeout_ms, start_ms, options.total_timeout_ms));
    sock->setRecvTimeOut(MergeTimeout(options.recv_timeout_ms, start_ms, options.total_timeout_ms));
    int rt = conn->sendRequest(req, &attempt);
    if (rt == 0)
    {
        return MakeHttpResult((int)HttpResult::Error::SEND_CLOSE_BY_PEER, nullptr,
                              "send request closed by peer: " +
                                  sock->getRemoteAddress()->toString(),
                              attempt);
    }
    if (rt < 0)
    {
        return MakeHttpResult((int)HttpResult::Error::SEND_SOCKET_ERROR, nullptr,
                              "send request socket error errno=" + std::to_string(errno) +
                                  " errstr=" + std::string(strerror(errno)),
                              attempt);
    }
    sock->setRecvTimeOut(MergeTimeout(options.recv_timeout_ms, start_ms, options.total_timeout_ms));
    auto rsp = conn->recvResponse();
    if (!rsp)
    {
        attempt.phase = HttpAttemptPhase::RESPONSE_TIMEOUT_AFTER_SEND;
        attempt.may_have_submitted = true;
        return MakeHttpResult((int)HttpResult::Error::TIMEOUT, nullptr,
                              "recv response timeout: " + sock->getRemoteAddress()->toString() +
                                  " recv_timeout_ms:" +
                                  std::to_string(options.recv_timeout_ms) +
                                  " total_timeout_ms:" +
                                  std::to_string(options.total_timeout_ms),
                              attempt);
    }
    if (rsp->isClose())
    {
        conn->close();
    }
    attempt.phase = HttpAttemptPhase::RESPONSE_RECEIVED;
    attempt.response_started = true;
    attempt.response_completed = true;
    attempt.http_status = (int)rsp->getStatus();
    return MakeHttpResult((int)HttpResult::Error::OK, rsp, "ok", attempt);
}

} // namespace http
} // namespace sylar
