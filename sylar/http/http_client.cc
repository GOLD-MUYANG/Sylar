#include "http_client.h"
#include "sylar/log.h"
#include <limits>
#include <sstream> // std::stringstream，用于拼接请求 path
#include <unistd.h>

namespace sylar
{
namespace http
{

// 当前文件使用的日志器。这里挂在 system 日志名下，便于统一输出调试信息。
static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

/**
 * @brief 保存已经解析好的 URI 和连接池。
 *
 * 构造函数是 private，只能被 Create() 调用。
 * 这样可以保证 HttpClient 对象创建出来时，m_uri 和 m_pool 都已经准备好。
 */
HttpClient::HttpClient(Uri::ptr uri, HttpConnectionPool::ptr pool) : m_uri(uri), m_pool(pool)
{
}

/**
 * @brief 创建 HttpClient，并初始化连接池。
 *
 * 这里做了三件事：
 * 1. 解析 URI；
 * 2. 检查 host 是否存在；
 * 3. 根据 host、port、scheme 创建 HttpConnectionPool。
 */
HttpClient::ptr HttpClient::Create(const std::string &uri,
                                   const std::string &vhost,
                                   uint32_t max_size,
                                   uint32_t max_alive_time,
                                   uint32_t max_request)
{
    // 把字符串形式的 URI 解析成 Uri 对象，后续可以取 host、port、scheme 等字段。
    Uri::ptr parsed = Uri::Create(uri);

    // URI 解析失败，或者没有 host，说明不是一个可用于发起 HTTP 连接的地址。
    if (!parsed || parsed->getHost().empty())
    {
        SYLAR_LOG_ERROR(g_logger) << "HttpClient invalid uri=" << uri;
        return nullptr;
    }

    // 创建连接池。
    // parsed->getHost()：目标服务器 host。
    // vhost：虚拟主机名，可影响 Host 请求头，具体行为看 HttpConnectionPool 实现。
    // parsed->getPort()：目标端口。
    // parsed->getScheme() == "https"：是否创建 HTTPS 连接。
    // max_size / max_alive_time / max_request：连接池控制参数。
    HttpConnectionPool::ptr pool(new HttpConnectionPool(parsed->getHost(), vhost, parsed->getPort(),
                                                        parsed->getScheme() == "https", max_size,
                                                        max_alive_time, max_request));

    // 构造函数私有，只能在类成员函数内部 new。
    return HttpClient::ptr(new HttpClient(parsed, pool));
}

/**
 * @brief 静态 Request 的 timeout_ms 版本。
 *
 * 这个函数只是适配层：把一个简单 timeout_ms 转成 HttpRequestOptions，
 * 然后交给 options 版本处理，避免两套请求逻辑重复。
 */
HttpResult::ptr HttpClient::Request(HttpMethod method,
                                    const std::string &url,
                                    uint64_t timeout_ms,
                                    const std::map<std::string, std::string> &headers,
                                    const std::string &body)
{
    return Request(method, url, HttpRequestOptions::FromTimeout(timeout_ms), headers, body);
}

/**
 * @brief 静态 Request 的核心实现。
 *
 * 静态接口接收完整 URL，所以它会临时创建一个 HttpClient，
 * 然后调用实例 request() 完成真正发送。
 */
HttpResult::ptr HttpClient::Request(HttpMethod method,
                                    const std::string &url,
                                    const HttpRequestOptions &options,
                                    const std::map<std::string, std::string> &headers,
                                    const std::string &body)
{
    return Request(method, url, options, HttpRetryOptions(), headers, body);
}

HttpResult::ptr HttpClient::Request(HttpMethod method,
                                    const std::string &url,
                                    const HttpRequestOptions &options,
                                    const HttpRetryOptions &retry_options,
                                    const std::map<std::string, std::string> &headers,
                                    const std::string &body)
{
    // 先解析一遍 URL，主要为了校验 URL，并提取 path/query/fragment。
    Uri::ptr uri = Uri::Create(url);
    if (!uri || uri->getHost().empty())
    {
        return InvalidUrlResult(url);
    }

    // 静态调用不复用外部已有 client，所以这里根据 URL 创建一个临时 client。
    HttpClient::ptr client = Create(url);
    if (!client)
    {
        return InvalidUrlResult(url);
    }

    // HTTP 请求行里一般只放 path + query，而不是完整 URL。
    // 例如完整 URL 是 http://example.com/a?x=1，真正请求 path 是 /a?x=1。
    return client->request(method, BuildPath(uri), options, retry_options, headers, body);
}

/**
 * @brief 静态 GET 的 timeout_ms 版本。
 */
HttpResult::ptr HttpClient::Get(const std::string &url,
                                uint64_t timeout_ms,
                                const std::map<std::string, std::string> &headers,
                                const std::string &body)
{
    return Get(url, HttpRequestOptions::FromTimeout(timeout_ms), headers, body);
}

/**
 * @brief 静态 GET 的 options 版本。
 *
 * GET 本身不写一套请求逻辑，只是把 method 固定成 HttpMethod::GET。
 */
HttpResult::ptr HttpClient::Get(const std::string &url,
                                const HttpRequestOptions &options,
                                const std::map<std::string, std::string> &headers,
                                const std::string &body)
{
    return Request(HttpMethod::GET, url, options, headers, body);
}

HttpResult::ptr HttpClient::Get(const std::string &url,
                                const HttpRequestOptions &options,
                                const HttpRetryOptions &retry_options,
                                const std::map<std::string, std::string> &headers,
                                const std::string &body)
{
    return Request(HttpMethod::GET, url, options, retry_options, headers, body);
}

/**
 * @brief 静态 POST 的 timeout_ms 版本。
 */
HttpResult::ptr HttpClient::Post(const std::string &url,
                                 uint64_t timeout_ms,
                                 const std::map<std::string, std::string> &headers,
                                 const std::string &body)
{
    return Post(url, HttpRequestOptions::FromTimeout(timeout_ms), headers, body);
}

/**
 * @brief 静态 POST 的 options 版本。
 *
 * POST 本身不写一套请求逻辑，只是把 method 固定成 HttpMethod::POST。
 */
HttpResult::ptr HttpClient::Post(const std::string &url,
                                 const HttpRequestOptions &options,
                                 const std::map<std::string, std::string> &headers,
                                 const std::string &body)
{
    return Request(HttpMethod::POST, url, options, headers, body);
}

HttpResult::ptr HttpClient::Post(const std::string &url,
                                 const HttpRequestOptions &options,
                                 const HttpRetryOptions &retry_options,
                                 const std::map<std::string, std::string> &headers,
                                 const std::string &body)
{
    return Request(HttpMethod::POST, url, options, retry_options, headers, body);
}

/**
 * @brief 实例 request 的 timeout_ms 版本。
 *
 * 和静态 Request 的 timeout_ms 版本一样，也是适配层。
 */
HttpResult::ptr HttpClient::request(HttpMethod method,
                                    const std::string &path,
                                    uint64_t timeout_ms,
                                    const std::map<std::string, std::string> &headers,
                                    const std::string &body)
{
    return request(method, path, HttpRequestOptions::FromTimeout(timeout_ms), headers, body);
}

/**
 * @brief 实例 request 的核心实现。
 *
 * 这个函数才是真正进入连接池发送请求的位置。
 */
HttpResult::ptr HttpClient::request(HttpMethod method,
                                    const std::string &path,
                                    const HttpRequestOptions &options,
                                    const std::map<std::string, std::string> &headers,
                                    const std::string &body)
{
    return request(method, path, options, HttpRetryOptions(), headers, body);
}

HttpResult::ptr HttpClient::request(HttpMethod method,
                                    const std::string &path,
                                    const HttpRequestOptions &options,
                                    const HttpRetryOptions &retry_options,
                                    const std::map<std::string, std::string> &headers,
                                    const std::string &body)
{
    // 理论上通过 Create() 创建的 client 都应该有连接池。
    // 这里仍然做防御式判断，避免空指针崩溃。
    if (!m_pool)
    {
        return std::make_shared<HttpResult>((int)HttpResult::Error::POOL_INVALID_CONNECTION,
                                            nullptr, "http client pool invalid");
    }

    // 空 path 统一当作根路径 /。
    // 否则请求行可能变成 GET  HTTP/1.1 这种非法格式。
    std::string request_path = path.empty() ? "/" : path;

    SYLAR_LOG_DEBUG(g_logger) << "HttpClient request method=" << HttpMethodToString(method)
                              << " path=" << request_path;

    // 真正发送请求的是连接池。HttpClient 在每次尝试后整理错误，再决定是否重试。
    HttpResult::ptr result;
    for (uint32_t retry_index = 0; retry_index <= retry_options.max_retry; ++retry_index)
    {
        if (retry_options.max_total_attempts > 0 &&
            retry_index >= retry_options.max_total_attempts)
        {
            return result ? result
                          : std::make_shared<HttpResult>(
                                (int)HttpResult::Error::CONNECT_FAIL, nullptr,
                                "http client max total attempts exhausted");
        }

        result = NormalizeResult(m_pool->doRequest(method, request_path, options, headers, body));
        if (retry_index >= retry_options.max_retry ||
            !ShouldRetry(method, result, retry_options))
        {
            return result;
        }

        uint64_t interval_ms = GetRetryInterval(retry_options, retry_index + 1);
        SYLAR_LOG_INFO(g_logger) << "HttpClient retry method=" << HttpMethodToString(method)
                                 << " path=" << request_path << " retry_index="
                                 << (retry_index + 1) << " result="
                                 << (result ? result->result : -1)
                                 << " interval_ms=" << interval_ms;
        if (interval_ms > 0)
        {
            usleep(interval_ms * 1000);
        }
    }

    return result;
}

/**
 * @brief 实例 GET 的 timeout_ms 版本。
 */
HttpResult::ptr HttpClient::get(const std::string &path,
                                uint64_t timeout_ms,
                                const std::map<std::string, std::string> &headers,
                                const std::string &body)
{
    return get(path, HttpRequestOptions::FromTimeout(timeout_ms), headers, body);
}

/**
 * @brief 实例 GET 的 options 版本。
 */
HttpResult::ptr HttpClient::get(const std::string &path,
                                const HttpRequestOptions &options,
                                const std::map<std::string, std::string> &headers,
                                const std::string &body)
{
    return request(HttpMethod::GET, path, options, headers, body);
}

HttpResult::ptr HttpClient::get(const std::string &path,
                                const HttpRequestOptions &options,
                                const HttpRetryOptions &retry_options,
                                const std::map<std::string, std::string> &headers,
                                const std::string &body)
{
    return request(HttpMethod::GET, path, options, retry_options, headers, body);
}

/**
 * @brief 实例 POST 的 timeout_ms 版本。
 */
HttpResult::ptr HttpClient::post(const std::string &path,
                                 uint64_t timeout_ms,
                                 const std::map<std::string, std::string> &headers,
                                 const std::string &body)
{
    return post(path, HttpRequestOptions::FromTimeout(timeout_ms), headers, body);
}

/**
 * @brief 实例 POST 的 options 版本。
 */
HttpResult::ptr HttpClient::post(const std::string &path,
                                 const HttpRequestOptions &options,
                                 const std::map<std::string, std::string> &headers,
                                 const std::string &body)
{
    return request(HttpMethod::POST, path, options, headers, body);
}

HttpResult::ptr HttpClient::post(const std::string &path,
                                 const HttpRequestOptions &options,
                                 const HttpRetryOptions &retry_options,
                                 const std::map<std::string, std::string> &headers,
                                 const std::string &body)
{
    return request(HttpMethod::POST, path, options, retry_options, headers, body);
}

/**
 * @brief 从 URI 中构造 HTTP 请求使用的 path。
 *
 * 例子：
 * - http://example.com            -> /
 * - http://example.com/a/b        -> /a/b
 * - http://example.com/a?x=1      -> /a?x=1
 * - http://example.com/a?x=1#top  -> /a?x=1#top  当前代码会保留 fragment
 *
 * 注意：严格来说，URL fragment（# 后面的部分）通常不应该发送给服务器。
 * 如果想更符合 HTTP 请求习惯，可以考虑不拼接 fragment。
 */
std::string HttpClient::BuildPath(Uri::ptr uri)
{
    std::stringstream ss;

    // 没写 path 时，默认请求根路径 /。
    std::string path = uri->getPath().empty() ? "/" : uri->getPath();

    // 拼接 path、query、fragment。
    // query 为空就不加 ?；fragment 为空就不加 #。
    ss << path << (uri->getQuery().empty() ? "" : "?") << uri->getQuery()
       << (uri->getFragment().empty() ? "" : "#") << uri->getFragment();

    return ss.str();
}

/**
 * @brief 构造 URL 非法的统一错误结果。
 */
HttpResult::ptr HttpClient::InvalidUrlResult(const std::string &url)
{
    return std::make_shared<HttpResult>((int)HttpResult::Error::INVALID_URL, nullptr,
                                        "invalid url: " + url);
}

/**
 * @brief 统一整理底层返回结果。
 *
 * 这里的作用是把底层错误改造成业务层更容易理解的错误：
 * - 发送时对端关闭 / socket 错误 -> SEND_FAIL
 * - 底层 TIMEOUT -> RECV_TIMEOUT
 * - 连接池取不到连接 -> CONNECT_FAIL
 * - HTTP 状态码 >= 400 -> HTTP_STATUS_ERROR
 */
HttpResult::ptr HttpClient::NormalizeResult(HttpResult::ptr result)
{
    // 底层返回空指针，说明没有拿到有效 HttpResult。
    if (!result)
    {
        return std::make_shared<HttpResult>((int)HttpResult::Error::RESPONSE_PARSE_FAIL, nullptr,
                                            "empty http result");
    }

    // HttpResult::result 是 int，这里转回枚举，方便 switch 判断。
    HttpResult::Error error = (HttpResult::Error)result->result;
    switch (error)
    {
    // 发送阶段失败，不再区分是对端关闭还是本地 socket 错误，统一给 SEND_FAIL。
    case HttpResult::Error::SEND_CLOSE_BY_PEER:
    case HttpResult::Error::SEND_SOCKET_ERROR:
        result->result = (int)HttpResult::Error::SEND_FAIL;
        break;

    // 底层 TIMEOUT 在业务层归为接收超时。
    case HttpResult::Error::TIMEOUT:
        result->result = (int)HttpResult::Error::RECV_TIMEOUT;
        break;

    // 连接池拿连接失败，业务层一般会理解成连接失败。
    case HttpResult::Error::POOL_GET_CONNECTION:
        result->result = (int)HttpResult::Error::CONNECT_FAIL;
        break;

    default:
        break;
    }

    // 网络层请求成功，不代表 HTTP 语义成功。
    // 例如服务端返回 404/500，此时 response 存在，但业务上通常应该认为请求失败。
    if (result->result == (int)HttpResult::Error::OK && result->response &&
        (int)result->response->getStatus() >= 400)
    {
        result->result = (int)HttpResult::Error::HTTP_STATUS_ERROR;
        result->error = "http status error: " + std::to_string((int)result->response->getStatus());
    }

    return result;
}

bool HttpClient::ShouldRetry(HttpMethod method,
                             const HttpResult::ptr &result,
                             const HttpRetryOptions &retry_options)
{
    // 默认只重试幂等方法；POST/RPC 类请求需要调用方显式允许。
    bool idempotent = method == HttpMethod::GET || method == HttpMethod::HEAD ||
                      method == HttpMethod::PUT || method == HttpMethod::DELETE ||
                      method == HttpMethod::OPTIONS || method == HttpMethod::TRACE;
    if (!idempotent && !retry_options.retry_non_idempotent)
    {
        return false;
    }

    if (!result)
    {
        return true;
    }

    HttpResult::Error error = (HttpResult::Error)result->result;
    switch (error)
    {
    case HttpResult::Error::CONNECT_FAIL:
    case HttpResult::Error::CONNECT_TIMEOUT:
    case HttpResult::Error::RECV_TIMEOUT:
        return true;

    case HttpResult::Error::HTTP_STATUS_ERROR:
        if (!result->response)
        {
            return false;
        }
        return result->response->getStatus() == HttpStatus::INTERNAL_SERVER_ERROR ||
               result->response->getStatus() == HttpStatus::BAD_GATEWAY ||
               result->response->getStatus() == HttpStatus::SERVICE_UNAVAILABLE ||
               result->response->getStatus() == HttpStatus::GATEWAY_TIMEOUT;

    default:
        return false;
    }
}

uint64_t HttpClient::GetRetryInterval(const HttpRetryOptions &retry_options,
                                      uint32_t retry_index)
{
    if (retry_options.retry_interval_ms == 0 || retry_index == 0)
    {
        return 0;
    }

    uint64_t multiplier = 1;
    switch (retry_options.backoff)
    {
    case HttpRetryOptions::Backoff::FIXED:
        multiplier = 1;
        break;
    case HttpRetryOptions::Backoff::LINEAR:
        multiplier = retry_index;
        break;
    case HttpRetryOptions::Backoff::EXPONENTIAL:
        for (uint32_t i = 1; i < retry_index; ++i)
        {
            if (multiplier > std::numeric_limits<uint64_t>::max() / 2)
            {
                multiplier = std::numeric_limits<uint64_t>::max();
                break;
            }
            multiplier *= 2;
        }
        break;
    }

    if (retry_options.retry_interval_ms > std::numeric_limits<uint64_t>::max() / multiplier)
    {
        return std::numeric_limits<uint64_t>::max();
    }
    return retry_options.retry_interval_ms * multiplier;
}

} // namespace http
} // namespace sylar
