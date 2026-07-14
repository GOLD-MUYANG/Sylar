#ifndef __SYLAR_HTTP_CONNECTION_H__
#define __SYLAR_HTTP_CONNECTION_H__

#include "http.h"
#include "http_request_options.h"
#include "sylar/socket_stream.h"
#include "sylar/thread.h"
#include "sylar/uri.h"
#include <list>
namespace sylar
{
namespace http
{
// 单次 HTTP 请求尝试所处的关键阶段。
// 这个枚举主要用于判断：本次失败后，是否还能安全重试。
enum class HttpAttemptPhase
{
    // 还没有真正发起请求。
    // 通常表示连接前失败、参数错误、连接池获取失败等。
    NOT_SENT = 0,
    // 请求体还没开始发送前就失败。
    // 例如 TCP/TLS 建立失败，或者发送请求头前失败。
    SEND_FAILED_BEFORE_BODY = 1,
    // 请求已经开始发送，但没有确认完整写完，或者状态无法确定。
    // 例如写了一部分就断开、send 返回异常、连接状态不明。
    PARTIAL_WRITE_OR_UNKNOWN = 2,
    // 请求已经完整发出，但等待响应超时。
    // 服务端很可能已经处理了请求，只是客户端没有等到结果。
    RESPONSE_TIMEOUT_AFTER_SEND = 3,
    // 已经收到 HTTP 响应。
    // 后续是否重试应主要根据 HTTP 状态码、响应是否完整、业务错误类型判断。
    RESPONSE_RECEIVED = 4,
};
// 将请求阶段转换成字符串，主要用于日志、错误信息和测试断言。
const char *HttpAttemptPhaseToString(HttpAttemptPhase phase);

// 单次 HTTP 请求尝试的结果记录。
// 它不只记录“成功/失败”，还记录请求到底走到了哪一步，
// 便于上层做幂等重试、错误分类、日志诊断和统计。
struct HttpAttemptOutcome
{
    // 本次尝试的粗粒度阶段，用于快速判断重试风险。
    HttpAttemptPhase phase = HttpAttemptPhase::NOT_SENT;

    // 是否已经建立 TCP 连接。
    bool tcp_connected = false;

    // TLS 握手是否完成。
    // 非 HTTPS 请求可以一直保持 false，或者由调用方约定忽略该字段。
    bool tls_handshake_completed = false;

    // 是否已经开始写请求字节。
    // 一旦为 true，就不能简单认为服务端完全没有收到请求。
    bool request_bytes_started = false;

    // 请求字节是否已经完整写出。
    // 注意：完整写出只代表客户端写入成功，不代表服务端一定处理完成。
    bool request_bytes_completed = false;

    // 是否已经开始接收响应。
    // 例如已经读到响应头或部分响应数据。
    bool response_started = false;

    // 响应是否完整接收。
    // 如果为 false，但 response_started 为 true，说明响应中途断开或超时。
    bool response_completed = false;

    // 本次请求是否“可能已经提交给服务端处理”。
    // 这是重试判断里的关键字段：
    // - false：通常可以安全重试；
    // - true：非幂等请求重试可能造成重复提交。
    bool may_have_submitted = false;

    // 已经成功写出的请求字节数。
    // 用于区分“完全没发出去”和“发了一部分后失败”。
    uint64_t request_bytes_sent = 0;

    // HTTP 状态码。
    // 0 表示还没有拿到有效 HTTP 响应。
    int http_status = 0;

    // 失败或异常的补充说明。
    // 例如 errno、超时阶段、解析错误、连接关闭原因等。
    std::string detail;
};

/**
 * @brief HTTP响应结果
 */
struct HttpResult
{
    /// 智能指针类型定义
    typedef std::shared_ptr<HttpResult> ptr;
    /**
     * @brief 错误码定义
     */
    enum class Error
    {
        /// 正常
        OK = 0,
        /// 非法URL
        INVALID_URL = 1,
        /// 无法解析HOST
        INVALID_HOST = 2,
        /// 连接失败
        CONNECT_FAIL = 3,
        /// 连接被对端关闭
        SEND_CLOSE_BY_PEER = 4,
        /// 发送请求产生Socket错误
        SEND_SOCKET_ERROR = 5,
        /// 超时
        TIMEOUT = 6,
        /// 创建Socket失败
        CREATE_SOCKET_ERROR = 7,
        /// 从连接池中取连接失败
        POOL_GET_CONNECTION = 8,
        /// 无效的连接
        POOL_INVALID_CONNECTION = 9,
        /// 连接超时
        CONNECT_TIMEOUT = 10,
        /// 发送失败
        SEND_FAIL = 11,
        /// 发送超时
        SEND_TIMEOUT = 12,
        /// 接收响应超时
        RECV_TIMEOUT = 13,
        /// 响应解析失败
        RESPONSE_PARSE_FAIL = 14,
        /// HTTP响应状态码错误
        HTTP_STATUS_ERROR = 15,
        /// 客户端并发限流
        RATE_LIMITED = 16,
        /// 客户端熔断打开
        CIRCUIT_OPEN = 17,
    };

    /**
     * @brief 构造函数
     * @param[in] _result 错误码
     * @param[in] _response HTTP响应结构体
     * @param[in] _error 错误描述
     */
    HttpResult(int _result, HttpResponse::ptr _response, const std::string &_error)
        : result(_result), response(_response), error(_error)
    {
    }

    /// 错误码
    int result;
    /// HTTP响应结构体
    HttpResponse::ptr response;
    /// 错误描述
    std::string error;
    /// 单次 HTTP 尝试的阶段元数据，用于判断是否可能已提交给上游。
    HttpAttemptOutcome attempt;

    std::string toString() const;
};

class HttpConnectionPool;
/**
 * @brief HTTP客户端类
 */
class HttpConnection : public SocketStream
{
    friend class HttpConnectionPool;

public:
    /// HTTP客户端类智能指针
    typedef std::shared_ptr<HttpConnection> ptr;

    /**
     * @brief 发送HTTP的GET请求
     * @param[in] url 请求的url
     * @param[in] timeout_ms 超时时间(毫秒)
     * @param[in] headers HTTP请求头部参数
     * @param[in] body 请求消息体
     * @return 返回HTTP结果结构体
     */
    static HttpResult::ptr DoGet(const std::string &url,
                                 uint64_t timeout_ms,
                                 const std::map<std::string, std::string> &headers = {},
                                 const std::string &body = "");
    static HttpResult::ptr DoGet(const std::string &url,
                                 const HttpRequestOptions &options,
                                 const std::map<std::string, std::string> &headers = {},
                                 const std::string &body = "");

    /**
     * @brief 发送HTTP的GET请求
     * @param[in] uri URI结构体
     * @param[in] timeout_ms 超时时间(毫秒)
     * @param[in] headers HTTP请求头部参数
     * @param[in] body 请求消息体
     * @return 返回HTTP结果结构体
     */
    static HttpResult::ptr DoGet(Uri::ptr uri,
                                 uint64_t timeout_ms,
                                 const std::map<std::string, std::string> &headers = {},
                                 const std::string &body = "");
    static HttpResult::ptr DoGet(Uri::ptr uri,
                                 const HttpRequestOptions &options,
                                 const std::map<std::string, std::string> &headers = {},
                                 const std::string &body = "");

    /**
     * @brief 发送HTTP的POST请求
     * @param[in] url 请求的url
     * @param[in] timeout_ms 超时时间(毫秒)
     * @param[in] headers HTTP请求头部参数
     * @param[in] body 请求消息体
     * @return 返回HTTP结果结构体
     */
    static HttpResult::ptr DoPost(const std::string &url,
                                  uint64_t timeout_ms,
                                  const std::map<std::string, std::string> &headers = {},
                                  const std::string &body = "");
    static HttpResult::ptr DoPost(const std::string &url,
                                  const HttpRequestOptions &options,
                                  const std::map<std::string, std::string> &headers = {},
                                  const std::string &body = "");

    /**
     * @brief 发送HTTP的POST请求
     * @param[in] uri URI结构体
     * @param[in] timeout_ms 超时时间(毫秒)
     * @param[in] headers HTTP请求头部参数
     * @param[in] body 请求消息体
     * @return 返回HTTP结果结构体
     */
    static HttpResult::ptr DoPost(Uri::ptr uri,
                                  uint64_t timeout_ms,
                                  const std::map<std::string, std::string> &headers = {},
                                  const std::string &body = "");
    static HttpResult::ptr DoPost(Uri::ptr uri,
                                  const HttpRequestOptions &options,
                                  const std::map<std::string, std::string> &headers = {},
                                  const std::string &body = "");

    /**
     * @brief 发送HTTP请求
     * @param[in] method 请求类型
     * @param[in] uri 请求的url
     * @param[in] timeout_ms 超时时间(毫秒)
     * @param[in] headers HTTP请求头部参数
     * @param[in] body 请求消息体
     * @return 返回HTTP结果结构体
     */
    static HttpResult::ptr DoRequest(HttpMethod method,
                                     const std::string &url,
                                     uint64_t timeout_ms,
                                     const std::map<std::string, std::string> &headers = {},
                                     const std::string &body = "");
    static HttpResult::ptr DoRequest(HttpMethod method,
                                     const std::string &url,
                                     const HttpRequestOptions &options,
                                     const std::map<std::string, std::string> &headers = {},
                                     const std::string &body = "");

    /**
     * @brief 发送HTTP请求
     * @param[in] method 请求类型
     * @param[in] uri URI结构体
     * @param[in] timeout_ms 超时时间(毫秒)
     * @param[in] headers HTTP请求头部参数
     * @param[in] body 请求消息体
     * @return 返回HTTP结果结构体
     */
    static HttpResult::ptr DoRequest(HttpMethod method,
                                     Uri::ptr uri,
                                     uint64_t timeout_ms,
                                     const std::map<std::string, std::string> &headers = {},
                                     const std::string &body = "");
    static HttpResult::ptr DoRequest(HttpMethod method,
                                     Uri::ptr uri,
                                     const HttpRequestOptions &options,
                                     const std::map<std::string, std::string> &headers = {},
                                     const std::string &body = "");

    /**
     * @brief 发送HTTP请求
     * @param[in] req 请求结构体
     * @param[in] uri URI结构体
     * @param[in] timeout_ms 超时时间(毫秒)
     * @return 返回HTTP结果结构体
     */
    static HttpResult::ptr DoRequest(HttpRequest::ptr req, Uri::ptr uri, uint64_t timeout_ms);
    static HttpResult::ptr
    DoRequest(HttpRequest::ptr req, Uri::ptr uri, const HttpRequestOptions &options);

    /**
     * @brief 构造函数
     * @param[in] sock Socket类
     * @param[in] owner 是否掌握所有权
     */
    HttpConnection(Socket::ptr sock, bool owner = true);

    /**
     * @brief 析构函数
     */
    ~HttpConnection();

    /**
     * @brief 接收HTTP响应
     */
    HttpResponse::ptr recvResponse();

    /**
     * @brief 发送HTTP请求
     * @param[in] req HTTP请求结构
     */
    int sendRequest(HttpRequest::ptr req);
    int sendRequest(HttpRequest::ptr req, HttpAttemptOutcome *attempt);

private:
    uint64_t m_createTime = 0;
    uint64_t m_request = 0;
};

class HttpConnectionPool
{
public:
    typedef std::shared_ptr<HttpConnectionPool> ptr;
    typedef Mutex MutexType;
    /**
     * @brief 从 URI 创建连接池。
     * @return URI 无法解析时返回 nullptr。
     */
    static HttpConnectionPool::ptr Create(const std::string &uri,
                                          const std::string &vhost,
                                          uint32_t max_size,
                                          uint32_t max_alive_time,
                                          uint32_t max_request);
    HttpConnectionPool(const std::string &host,
                       const std::string &vhost,
                       uint32_t port,
                       bool is_https,
                       uint32_t max_size,
                       uint32_t max_alive_time,
                       uint32_t max_request);

    HttpConnection::ptr getConnection();

    /**
     * @brief 发送HTTP的GET请求
     * @param[in] url 请求的url
     * @param[in] timeout_ms 超时时间(毫秒)
     * @param[in] headers HTTP请求头部参数
     * @param[in] body 请求消息体
     * @return 返回HTTP结果结构体
     */
    HttpResult::ptr doGet(const std::string &url,
                          uint64_t timeout_ms,
                          const std::map<std::string, std::string> &headers = {},
                          const std::string &body = "");
    HttpResult::ptr doGet(const std::string &url,
                          const HttpRequestOptions &options,
                          const std::map<std::string, std::string> &headers = {},
                          const std::string &body = "");

    /**
     * @brief 发送HTTP的GET请求
     * @param[in] uri URI结构体
     * @param[in] timeout_ms 超时时间(毫秒)
     * @param[in] headers HTTP请求头部参数
     * @param[in] body 请求消息体
     * @return 返回HTTP结果结构体
     */
    HttpResult::ptr doGet(Uri::ptr uri,
                          uint64_t timeout_ms,
                          const std::map<std::string, std::string> &headers = {},
                          const std::string &body = "");
    HttpResult::ptr doGet(Uri::ptr uri,
                          const HttpRequestOptions &options,
                          const std::map<std::string, std::string> &headers = {},
                          const std::string &body = "");

    /**
     * @brief 发送HTTP的POST请求
     * @param[in] url 请求的url
     * @param[in] timeout_ms 超时时间(毫秒)
     * @param[in] headers HTTP请求头部参数
     * @param[in] body 请求消息体
     * @return 返回HTTP结果结构体
     */
    HttpResult::ptr doPost(const std::string &url,
                           uint64_t timeout_ms,
                           const std::map<std::string, std::string> &headers = {},
                           const std::string &body = "");
    HttpResult::ptr doPost(const std::string &url,
                           const HttpRequestOptions &options,
                           const std::map<std::string, std::string> &headers = {},
                           const std::string &body = "");

    /**
     * @brief 发送HTTP的POST请求
     * @param[in] uri URI结构体
     * @param[in] timeout_ms 超时时间(毫秒)
     * @param[in] headers HTTP请求头部参数
     * @param[in] body 请求消息体
     * @return 返回HTTP结果结构体
     */
    HttpResult::ptr doPost(Uri::ptr uri,
                           uint64_t timeout_ms,
                           const std::map<std::string, std::string> &headers = {},
                           const std::string &body = "");
    HttpResult::ptr doPost(Uri::ptr uri,
                           const HttpRequestOptions &options,
                           const std::map<std::string, std::string> &headers = {},
                           const std::string &body = "");

    /**
     * @brief 发送HTTP请求
     * @param[in] method 请求类型
     * @param[in] uri 请求的url
     * @param[in] timeout_ms 超时时间(毫秒)
     * @param[in] headers HTTP请求头部参数
     * @param[in] body 请求消息体
     * @return 返回HTTP结果结构体
     */
    HttpResult::ptr doRequest(HttpMethod method,
                              const std::string &url,
                              uint64_t timeout_ms,
                              const std::map<std::string, std::string> &headers = {},
                              const std::string &body = "");
    HttpResult::ptr doRequest(HttpMethod method,
                              const std::string &url,
                              const HttpRequestOptions &options,
                              const std::map<std::string, std::string> &headers = {},
                              const std::string &body = "");

    /**
     * @brief 发送HTTP请求
     * @param[in] method 请求类型
     * @param[in] uri URI结构体
     * @param[in] timeout_ms 超时时间(毫秒)
     * @param[in] headers HTTP请求头部参数
     * @param[in] body 请求消息体
     * @return 返回HTTP结果结构体
     */
    HttpResult::ptr doRequest(HttpMethod method,
                              Uri::ptr uri,
                              uint64_t timeout_ms,
                              const std::map<std::string, std::string> &headers = {},
                              const std::string &body = "");
    HttpResult::ptr doRequest(HttpMethod method,
                              Uri::ptr uri,
                              const HttpRequestOptions &options,
                              const std::map<std::string, std::string> &headers = {},
                              const std::string &body = "");

    /**
     * @brief 发送HTTP请求
     * @param[in] req 请求结构体
     * @param[in] timeout_ms 超时时间(毫秒)
     * @return 返回HTTP结果结构体
     */
    HttpResult::ptr doRequest(HttpRequest::ptr req, uint64_t timeout_ms);
    HttpResult::ptr doRequest(HttpRequest::ptr req, const HttpRequestOptions &options);

private:
    struct Waiter
    {
        typedef std::shared_ptr<Waiter> ptr;
        sylar::Scheduler *scheduler = nullptr;
        sylar::Fiber::ptr fiber;
        bool notified = false;
        bool timeout = false;
    };

    HttpConnection::ptr getConnection(uint64_t timeout_ms);
    HttpConnection::ptr getConnection(uint64_t timeout_ms, const HttpRequestOptions &options);
    static void ReleasePtr(HttpConnection *ptr, HttpConnectionPool *pool);
    bool isConnectionReusable(HttpConnection *ptr, uint64_t now_ms) const;
    void notifyWaiter();

private:
    std::string m_host;
    std::string m_vhost;
    uint32_t m_port;
    //最大连接数量
    uint32_t m_maxSize;
    //单条连接最大存活时间
    uint32_t m_maxAliveTime;
    //单条连接最大使用次数
    uint32_t m_maxRequest;
    bool m_isHttps;

    MutexType m_mutex;
    //当前空闲连接
    std::list<HttpConnection *> m_conns;
    //等待连接的协程
    std::list<std::shared_ptr<Waiter>> m_waiters;
    //空闲和借出连接的总数
    std::atomic<int32_t> m_total = {0};
};

} // namespace http
} // namespace sylar

#endif
