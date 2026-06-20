#ifndef __SYLAR_HTTP_LOAD_BALANCE_CLIENT_H__
#define __SYLAR_HTTP_LOAD_BALANCE_CLIENT_H__

#include "http_client.h"
#include "http_concurrency_limiter.h"
#include "http_circuit_breaker.h"
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
 * @brief 多实例 HTTP 客户端负载均衡策略。
 */
enum class HttpLoadBalanceStrategy
{
    ROUND_ROBIN = 0,
    RANDOM = 1,
    WEIGHTED_ROUND_ROBIN = 2,
    LEAST_CONNECTION = 3,
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
    uint32_t getActiveRequestCount() const;
    std::string getLimitKey() const;

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

    void beginRequest();
    void endRequest();

private:
    std::string m_host;
    uint32_t m_port = 0;
    bool m_ssl = false;
    uint32_t m_weight = 1;
    HttpEndpointStatus m_status = HttpEndpointStatus::UP;
    uint32_t m_activeRequests = 0;
    HttpConnectionPool::ptr m_pool;
    mutable MutexType m_mutex;
};

/**
 * @brief 面向多实例服务的 HTTP 客户端。
 *
 * 负责从多个 Endpoint 里选择一个可用实例，然后复用 HttpConnectionPool 完成请求。
 * Endpoint 维度的状态、连接池和活跃请求数都在这里维护，后续熔断和统计也可以沿着
 * 这个边界继续扩展。
 */
class HttpLoadBalanceClient
{
public:
    typedef std::shared_ptr<HttpLoadBalanceClient> ptr;
    typedef Mutex MutexType;

    static HttpLoadBalanceClient::ptr Create(
        const std::vector<HttpEndpoint::ptr> &endpoints,
        HttpLoadBalanceStrategy strategy = HttpLoadBalanceStrategy::ROUND_ROBIN,
        const HttpConcurrencyLimitOptions &limit_options = HttpConcurrencyLimitOptions(),
        const HttpCircuitBreakerOptions &circuit_options = HttpCircuitBreakerOptions());

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

    size_t checkHealth(const std::string &path = "/", uint64_t timeout_ms = 1000);
    size_t checkHealth(const std::string &path, const HttpRequestOptions &options);

private:
    HttpLoadBalanceClient(const std::vector<HttpEndpoint::ptr> &endpoints,
                          HttpLoadBalanceStrategy strategy,
                          const HttpConcurrencyLimitOptions &limit_options,
                          const HttpCircuitBreakerOptions &circuit_options);

    HttpEndpoint::ptr selectEndpoint();
    HttpEndpoint::ptr selectRoundRobin();
    HttpEndpoint::ptr selectRandom();
    HttpEndpoint::ptr selectWeightedRoundRobin();
    HttpEndpoint::ptr selectLeastConnection();

private:
    std::vector<HttpEndpoint::ptr> m_endpoints;
    HttpLoadBalanceStrategy m_strategy = HttpLoadBalanceStrategy::ROUND_ROBIN;
    size_t m_nextIndex = 0;
    size_t m_weightedIndex = 0;
    uint32_t m_weightedReturned = 0;
    HttpConcurrencyLimiter::ptr m_limiter;
    HttpCircuitBreaker::ptr m_circuitBreaker;
    MutexType m_mutex;
};

} // namespace http
} // namespace sylar

#endif
