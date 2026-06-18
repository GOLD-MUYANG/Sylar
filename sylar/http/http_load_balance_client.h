#ifndef __SYLAR_HTTP_LOAD_BALANCE_CLIENT_H__
#define __SYLAR_HTTP_LOAD_BALANCE_CLIENT_H__

#include "http_client.h"
#include "sylar/mutex.h"
#include <stdint.h>
#include <vector>

namespace sylar
{
namespace http
{

/**
 * @brief Endpoint 当前可用状态。
 *
 * 第一版只提供手动状态，健康检查留到第五步后续部分。
 */
enum class HttpEndpointStatus
{
    UP = 0,
    DOWN = 1,
};

/**
 * @brief 第一版负载均衡策略。
 *
 * 加权轮询、最少连接数等策略属于文档第五步后续部分，本次不实现。
 */
enum class HttpLoadBalanceStrategy
{
    ROUND_ROBIN = 0,
    RANDOM = 1,
};

/**
 * @brief HTTP 服务实例描述。
 *
 * 每个 Endpoint 持有自己的 HttpConnectionPool，便于后续在 Endpoint 维度扩展健康检查、
 * 熔断和统计。
 */
class HttpEndpoint
{
public:
    typedef std::shared_ptr<HttpEndpoint> ptr;

    static HttpEndpoint::ptr Create(const std::string &host,
                                    uint32_t port,
                                    bool ssl = false,
                                    uint32_t weight = 1,
                                    HttpEndpointStatus status = HttpEndpointStatus::UP,
                                    const std::string &vhost = "",
                                    uint32_t max_size = 10,
                                    uint32_t max_alive_time = 30000,
                                    uint32_t max_request = 1000);

    const std::string &getHost() const
    {
        return m_host;
    }

    uint32_t getPort() const
    {
        return m_port;
    }

    bool isSsl() const
    {
        return m_ssl;
    }

    uint32_t getWeight() const
    {
        return m_weight;
    }

    HttpEndpointStatus getStatus() const;
    void setStatus(HttpEndpointStatus status);

private:
    HttpEndpoint(const std::string &host,
                 uint32_t port,
                 bool ssl,
                 uint32_t weight,
                 HttpEndpointStatus status,
                 HttpConnectionPool::ptr pool);

private:
    friend class HttpLoadBalanceClient;
    typedef Mutex MutexType;

    std::string m_host;
    uint32_t m_port = 0;
    bool m_ssl = false;
    uint32_t m_weight = 1;
    HttpEndpointStatus m_status = HttpEndpointStatus::UP;
    HttpConnectionPool::ptr m_pool;
    mutable MutexType m_mutex;
};

/**
 * @brief 面向多实例服务的 HTTP 客户端。
 *
 * 第一版只负责从多个 Endpoint 里选择一个可用实例，然后复用 HttpConnectionPool 完成请求。
 * 不包含健康检查、加权策略和最少连接策略。
 */
class HttpLoadBalanceClient
{
public:
    typedef std::shared_ptr<HttpLoadBalanceClient> ptr;
    typedef Mutex MutexType;

    static HttpLoadBalanceClient::ptr Create(
        const std::vector<HttpEndpoint::ptr> &endpoints,
        HttpLoadBalanceStrategy strategy = HttpLoadBalanceStrategy::ROUND_ROBIN);

    HttpResult::ptr request(HttpMethod method,
                            const std::string &path,
                            uint64_t timeout_ms,
                            const std::map<std::string, std::string> &headers = {},
                            const std::string &body = "");
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

    HttpResult::ptr get(const std::string &path,
                        uint64_t timeout_ms,
                        const std::map<std::string, std::string> &headers = {},
                        const std::string &body = "");
    HttpResult::ptr get(const std::string &path,
                        const HttpRequestOptions &options,
                        const std::map<std::string, std::string> &headers = {},
                        const std::string &body = "");
    HttpResult::ptr get(const std::string &path,
                        const HttpRequestOptions &options,
                        const HttpRetryOptions &retry_options,
                        const std::map<std::string, std::string> &headers = {},
                        const std::string &body = "");

    HttpResult::ptr post(const std::string &path,
                         uint64_t timeout_ms,
                         const std::map<std::string, std::string> &headers = {},
                         const std::string &body = "");
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
    HttpLoadBalanceClient(const std::vector<HttpEndpoint::ptr> &endpoints,
                          HttpLoadBalanceStrategy strategy);

    HttpEndpoint::ptr selectEndpoint();
    HttpEndpoint::ptr selectRoundRobin();
    HttpEndpoint::ptr selectRandom();

private:
    std::vector<HttpEndpoint::ptr> m_endpoints;
    HttpLoadBalanceStrategy m_strategy = HttpLoadBalanceStrategy::ROUND_ROBIN;
    size_t m_nextIndex = 0;
    MutexType m_mutex;
};

} // namespace http
} // namespace sylar

#endif
