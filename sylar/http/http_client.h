#ifndef __SYLAR_HTTP_CLIENT_H__
#define __SYLAR_HTTP_CLIENT_H__

#include "http_connection.h"
#include "http_request_options.h"
#include "sylar/uri.h"
#include <map>
#include <memory>
#include <string>

namespace sylar
{
namespace http
{

class HttpLoadBalanceClient;

/**
 * @brief HTTP 客户端失败重试参数。
 *
 * HttpRequestOptions 只描述单次请求的超时；这里描述多次请求之间的重试策略。
 */
struct HttpRetryOptions
{
    //固定、线性、指数退避。
    enum class Backoff
    {
        FIXED = 0,
        LINEAR = 1,
        EXPONENTIAL = 2,
    };

    /// 最大重试次数，不包含首次请求。0 表示不重试，保持旧行为。
    uint32_t max_retry = 0;

    /// 单次业务请求允许的最大下游尝试次数，0 表示不额外限制。
    uint32_t max_total_attempts = 0;

    /// 每次重试前等待的基础间隔，单位毫秒。0 表示立即重试。
    uint64_t retry_interval_ms = 0;

    /// 重试间隔退避策略。
    Backoff backoff = Backoff::FIXED;

    /// 是否允许 POST/PATCH 等非幂等请求自动重试，默认关闭。
    bool retry_non_idempotent = false;
};

/**
 * @brief 面向业务层的 HTTP 客户端封装。
 *
 * 这个类不是直接操作 Socket 的底层连接类，而是对 HttpConnectionPool
 * 再包一层，给业务代码提供更简单的 Get/Post/Request 接口。
 *
 * 使用方式分两类：
 * 1. 静态接口：HttpClient::Get("http://example.com/a", 1000)
 *    - 每次根据完整 URL 创建一个临时 HttpClient 和连接池。
 *    - 适合偶发请求。
 *
 * 2. 实例接口：client->get("/a", 1000)
 *    - 先通过 Create() 创建客户端，内部持有连接池。
 *    - 后续只传 path，复用同一个 host 的连接池。
 *    - 适合对同一个服务反复请求。
 */
class HttpClient
{
    friend class HttpLoadBalanceClient;

public:
    typedef std::shared_ptr<HttpClient> ptr;

    /**
     * @brief 创建一个绑定到指定 URI 的 HttpClient。
     *
     * @param uri 完整 URI，例如 http://example.com 或 https://api.xxx.com:443。
     *            这里主要使用其中的 scheme、host、port。
     * @param vhost 虚拟主机名。为空时通常由底层连接池使用 host 作为 Host 头。
     * @param max_size 连接池最大连接数。
     * @param max_alive_time 连接最大存活时间，单位通常是毫秒，具体取决于 HttpConnectionPool 实现。
     * @param max_request 单个连接最多承载多少次请求，超过后通常会重建连接。
     * @return 创建成功返回 HttpClient 智能指针；URI 非法时返回 nullptr。
     */
    static HttpClient::ptr Create(const std::string &uri,
                                  const std::string &vhost = "",
                                  uint32_t max_size = 10,
                                  uint32_t max_alive_time = 30000,
                                  uint32_t max_request = 1000);

    /**
     * @brief 静态通用请求接口，使用 timeout_ms 构造默认请求选项。
     *
     * @param method HTTP 方法，例如 GET、POST。
     * @param url 完整 URL，例如 http://example.com/index?a=1。
     * @param timeout_ms 请求超时时间，最终会被包装成 HttpRequestOptions。
     * @param headers 额外请求头。
     * @param body 请求体，GET 通常为空，POST 常用。
     */
    static HttpResult::ptr Request(HttpMethod method,
                                   const std::string &url,
                                   uint64_t timeout_ms,
                                   const std::map<std::string, std::string> &headers = {},
                                   const std::string &body = "");

    /**
     * @brief 静态通用请求接口，直接使用更完整的 HttpRequestOptions。
     *
     * 这个重载比 timeout_ms 版本更灵活。如果以后 options 里加入
     * connect_timeout、read_timeout、keepalive 策略等字段，调用者可以直接控制。
     */
    static HttpResult::ptr Request(HttpMethod method,
                                   const std::string &url,
                                   const HttpRequestOptions &options,
                                   const std::map<std::string, std::string> &headers = {},
                                   const std::string &body = "");
    static HttpResult::ptr Request(HttpMethod method,
                                   const std::string &url,
                                   const HttpRequestOptions &options,
                                   const HttpRetryOptions &retry_options,
                                   const std::map<std::string, std::string> &headers = {},
                                   const std::string &body = "");

    /**
     * @brief 静态 GET 请求，timeout_ms 版本。
     */
    static HttpResult::ptr Get(const std::string &url,
                               uint64_t timeout_ms,
                               const std::map<std::string, std::string> &headers = {},
                               const std::string &body = "");

    /**
     * @brief 静态 GET 请求，options 版本。
     */
    static HttpResult::ptr Get(const std::string &url,
                               const HttpRequestOptions &options,
                               const std::map<std::string, std::string> &headers = {},
                               const std::string &body = "");
    static HttpResult::ptr Get(const std::string &url,
                               const HttpRequestOptions &options,
                               const HttpRetryOptions &retry_options,
                               const std::map<std::string, std::string> &headers = {},
                               const std::string &body = "");

    /**
     * @brief 静态 POST 请求，timeout_ms 版本。
     */
    static HttpResult::ptr Post(const std::string &url,
                                uint64_t timeout_ms,
                                const std::map<std::string, std::string> &headers = {},
                                const std::string &body = "");

    /**
     * @brief 静态 POST 请求，options 版本。
     */
    static HttpResult::ptr Post(const std::string &url,
                                const HttpRequestOptions &options,
                                const std::map<std::string, std::string> &headers = {},
                                const std::string &body = "");
    static HttpResult::ptr Post(const std::string &url,
                                const HttpRequestOptions &options,
                                const HttpRetryOptions &retry_options,
                                const std::map<std::string, std::string> &headers = {},
                                const std::string &body = "");

    /**
     * @brief 实例通用请求接口，timeout_ms 版本。
     *
     * @param path 请求路径，不是完整 URL。例如 /api/list?a=1。
     *             因为 host、port、https 信息已经在 Create() 时绑定到连接池了。
     */
    HttpResult::ptr request(HttpMethod method,
                            const std::string &path,
                            uint64_t timeout_ms,
                            const std::map<std::string, std::string> &headers = {},
                            const std::string &body = "");

    /**
     * @brief 实例通用请求接口，options 版本。
     */
    HttpResult::ptr request(HttpMethod method,
                            const std::string &path,
                            const HttpRequestOptions &options,
                            const std::map<std::string, std::string> &headers = {},
                            const std::string &body = "");
    HttpResult::ptr request(HttpMethod method,
                            const std::string &path,
                            const HttpRequestOptions &options,
                            const HttpRetryOptions &retry_options,
                            const std::map<std::string, std::string> &headers = {},
                            const std::string &body = "");

    /**
     * @brief 实例 GET 请求，timeout_ms 版本。
     */
    HttpResult::ptr get(const std::string &path,
                        uint64_t timeout_ms,
                        const std::map<std::string, std::string> &headers = {},
                        const std::string &body = "");

    /**
     * @brief 实例 GET 请求，options 版本。
     */
    HttpResult::ptr get(const std::string &path,
                        const HttpRequestOptions &options,
                        const std::map<std::string, std::string> &headers = {},
                        const std::string &body = "");
    HttpResult::ptr get(const std::string &path,
                        const HttpRequestOptions &options,
                        const HttpRetryOptions &retry_options,
                        const std::map<std::string, std::string> &headers = {},
                        const std::string &body = "");

    /**
     * @brief 实例 POST 请求，timeout_ms 版本。
     */
    HttpResult::ptr post(const std::string &path,
                         uint64_t timeout_ms,
                         const std::map<std::string, std::string> &headers = {},
                         const std::string &body = "");

    /**
     * @brief 实例 POST 请求，options 版本。
     */
    HttpResult::ptr post(const std::string &path,
                         const HttpRequestOptions &options,
                         const std::map<std::string, std::string> &headers = {},
                         const std::string &body = "");
    HttpResult::ptr post(const std::string &path,
                         const HttpRequestOptions &options,
                         const HttpRetryOptions &retry_options,
                         const std::map<std::string, std::string> &headers = {},
                         const std::string &body = "");

private:
    /**
     * @brief 构造函数私有化，强制调用者通过 Create() 创建对象。
     *
     * 这样可以保证 HttpClient 创建时一定带着解析后的 Uri 和连接池，
     * 避免业务层手动 new 出一个状态不完整的客户端。
     */
    HttpClient(Uri::ptr uri, HttpConnectionPool::ptr pool);

    /**
     * @brief 从完整 URI 中提取 HTTP 请求行里需要的 path 部分。
     *
     * 例如：
     * http://example.com/a/b?x=1 会变成 /a/b?x=1。
     */
    static std::string BuildPath(Uri::ptr uri);

    /**
     * @brief 统一构造“URL 非法”的失败结果。
     */
    static HttpResult::ptr InvalidUrlResult(const std::string &url);

    /**
     * @brief 统一整理底层返回结果。
     *
     * 底层 HttpConnectionPool 可能返回更细的错误，比如发送阶段 socket error、
     * 对端关闭、连接池取连接失败等；业务层通常不需要分得太细，
     * 所以这里把错误归一化成更好理解的类型。
     */
    static HttpResult::ptr NormalizeResult(HttpResult::ptr result);

    /**
     * @brief 判断当前错误是否应该触发下一次重试。
     */
    static bool ShouldRetry(HttpMethod method,
                            const HttpResult::ptr &result,
                            const HttpRetryOptions &retry_options);

    /**
     * @brief 计算第 retry_index 次重试前的等待时间。
     */
    static uint64_t GetRetryInterval(const HttpRetryOptions &retry_options, uint32_t retry_index);

private:
    /// Create() 时解析出的 URI，保存 scheme、host、port、path 等信息。
    Uri::ptr m_uri;

    /// 连接池，真正负责连接创建、连接复用和发送 HTTP 请求。
    HttpConnectionPool::ptr m_pool;
};

} // namespace http
} // namespace sylar

#endif
