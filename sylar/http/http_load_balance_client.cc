#include "http_load_balance_client.h"
#include "sylar/log.h"
#include <cstdlib>
#include <limits>
#include <sstream>
#include <unistd.h>

namespace sylar
{
namespace http
{

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

HttpEndpoint::ptr HttpEndpoint::Create(const std::string &host,
                                       uint32_t port,
                                       bool ssl,
                                       uint32_t weight,
                                       HttpEndpointStatus status,
                                       const std::string &vhost,
                                       uint32_t max_size,
                                       uint32_t max_alive_time,
                                       uint32_t max_request)
{
    if (host.empty())
    {
        SYLAR_LOG_ERROR(g_logger) << "HttpEndpoint empty host";
        return nullptr;
    }

    HttpConnectionPool::ptr pool(
        new HttpConnectionPool(host, vhost, port, ssl, max_size, max_alive_time, max_request));
    return HttpEndpoint::ptr(new HttpEndpoint(host, port, ssl, weight, status, pool));
}

HttpEndpoint::HttpEndpoint(const std::string &host,
                           uint32_t port,
                           bool ssl,
                           uint32_t weight,
                           HttpEndpointStatus status,
                           HttpConnectionPool::ptr pool)
    : m_host(host), m_port(port), m_ssl(ssl), m_weight(weight == 0 ? 1 : weight), m_status(status),
      m_pool(pool)
{
}

HttpEndpointStatus HttpEndpoint::getStatus() const
{
    MutexType::Lock lock(m_mutex);
    return m_status;
}

void HttpEndpoint::setStatus(HttpEndpointStatus status)
{
    MutexType::Lock lock(m_mutex);
    m_status = status;
}

uint32_t HttpEndpoint::getActiveRequestCount() const
{
    MutexType::Lock lock(m_mutex);
    return m_activeRequests;
}

std::string HttpEndpoint::getLimitKey() const
{
    std::stringstream ss;
    ss << m_host << ":" << m_port;
    return ss.str();
}

void HttpEndpoint::beginRequest()
{
    MutexType::Lock lock(m_mutex);
    ++m_activeRequests;
}

void HttpEndpoint::endRequest()
{
    MutexType::Lock lock(m_mutex);
    if (m_activeRequests > 0)
    {
        --m_activeRequests;
    }
}

HttpLoadBalanceClient::ptr
HttpLoadBalanceClient::Create(const std::vector<HttpEndpoint::ptr> &endpoints,
                              HttpLoadBalanceStrategy strategy,
                              const HttpConcurrencyLimitOptions &limit_options,
                              const HttpCircuitBreakerOptions &circuit_options)
{
    std::vector<HttpEndpoint::ptr> valid;
    for (auto &endpoint : endpoints)
    {
        if (endpoint && endpoint->m_pool)
        {
            valid.push_back(endpoint);
        }
    }

    if (valid.empty())
    {
        SYLAR_LOG_ERROR(g_logger) << "HttpLoadBalanceClient empty endpoints";
        return nullptr;
    }

    return HttpLoadBalanceClient::ptr(
        new HttpLoadBalanceClient(valid, strategy, limit_options, circuit_options));
}

HttpLoadBalanceClient::HttpLoadBalanceClient(const std::vector<HttpEndpoint::ptr> &endpoints,
                                             HttpLoadBalanceStrategy strategy,
                                             const HttpConcurrencyLimitOptions &limit_options,
                                             const HttpCircuitBreakerOptions &circuit_options)
    : m_endpoints(endpoints), m_strategy(strategy),
      m_limiter(HttpConcurrencyLimiter::Create(limit_options)),
      m_circuitBreaker(HttpCircuitBreaker::Create(circuit_options))
{
}

HttpResult::ptr HttpLoadBalanceClient::request(HttpMethod method,
                                               const std::string &path,
                                               uint64_t timeout_ms,
                                               const std::map<std::string, std::string> &headers,
                                               const std::string &body)
{
    return request(method, path, HttpRequestOptions::FromTimeout(timeout_ms), headers, body);
}

HttpResult::ptr HttpLoadBalanceClient::request(HttpMethod method,
                                               const std::string &path,
                                               const HttpRequestOptions &options,
                                               const std::map<std::string, std::string> &headers,
                                               const std::string &body)
{
    return request(method, path, options, HttpRetryOptions(), headers, body);
}

HttpResult::ptr HttpLoadBalanceClient::request(HttpMethod method,
                                               const std::string &path,
                                               const HttpRequestOptions &options,
                                               const HttpRetryOptions &retry_options,
                                               const std::map<std::string, std::string> &headers,
                                               const std::string &body)
{
    std::string request_path = path.empty() ? "/" : path;
    HttpResult::ptr result;

    for (uint32_t retry_index = 0; retry_index <= retry_options.max_retry; ++retry_index)
    {
        //选择一个终端
        HttpEndpoint::ptr endpoint = selectEndpoint();
        if (!endpoint)
        {
            return std::make_shared<HttpResult>((int)HttpResult::Error::CONNECT_FAIL, nullptr,
                                                "no available endpoint");
        }
        SYLAR_LOG_DEBUG(g_logger) << "HttpLoadBalanceClient request method="
                                  << HttpMethodToString(method)
                                  << " endpoint=" << endpoint->getHost() << ":"
                                  << endpoint->getPort() << " path=" << request_path;

        // 用选择出来的终端发请求；selectEndpoint() 已经占用活跃请求名额。
        {
            std::string endpoint_key = endpoint->getLimitKey();
            HttpConcurrencyLimitGuard::ptr limit_guard =
                m_limiter ? m_limiter->tryAcquire(endpoint_key) : nullptr;
            if (!limit_guard)
            {
                endpoint->endRequest();
                return std::make_shared<HttpResult>((int)HttpResult::Error::RATE_LIMITED, nullptr,
                                                    "http client concurrency limited");
            }

            HttpCircuitBreakerGuard::ptr circuit_guard =
                m_circuitBreaker ? m_circuitBreaker->tryAcquire(endpoint_key) : nullptr;
            if (!circuit_guard)
            {
                endpoint->endRequest();
                return std::make_shared<HttpResult>((int)HttpResult::Error::CIRCUIT_OPEN, nullptr,
                                                    "http client circuit open");
            }

            struct RequestGuard
            {
                explicit RequestGuard(HttpEndpoint::ptr ep) : endpoint(ep)
                {
                }
                ~RequestGuard()
                {
                    endpoint->endRequest();
                }
                HttpEndpoint::ptr endpoint;
            } guard(endpoint);
            result = HttpClient::NormalizeResult(
                endpoint->m_pool->doRequest(method, request_path, options, headers, body));
            if (m_circuitBreaker)
            {
                m_circuitBreaker->onRequestComplete(endpoint_key, result);
            }
        }
        if (retry_index >= retry_options.max_retry ||
            !HttpClient::ShouldRetry(method, result, retry_options))
        {
            return result;
        }

        uint64_t interval_ms = HttpClient::GetRetryInterval(retry_options, retry_index + 1);
        SYLAR_LOG_INFO(g_logger) << "HttpLoadBalanceClient retry method="
                                 << HttpMethodToString(method) << " path=" << request_path
                                 << " retry_index=" << (retry_index + 1)
                                 << " result=" << (result ? result->result : -1)
                                 << " interval_ms=" << interval_ms;
        if (interval_ms > 0)
        {
            usleep(interval_ms * 1000);
        }
    }

    return result;
}

HttpResult::ptr HttpLoadBalanceClient::get(const std::string &path,
                                           uint64_t timeout_ms,
                                           const std::map<std::string, std::string> &headers,
                                           const std::string &body)
{
    return get(path, HttpRequestOptions::FromTimeout(timeout_ms), headers, body);
}

HttpResult::ptr HttpLoadBalanceClient::get(const std::string &path,
                                           const HttpRequestOptions &options,
                                           const std::map<std::string, std::string> &headers,
                                           const std::string &body)
{
    return request(HttpMethod::GET, path, options, headers, body);
}

HttpResult::ptr HttpLoadBalanceClient::get(const std::string &path,
                                           const HttpRequestOptions &options,
                                           const HttpRetryOptions &retry_options,
                                           const std::map<std::string, std::string> &headers,
                                           const std::string &body)
{
    return request(HttpMethod::GET, path, options, retry_options, headers, body);
}

HttpResult::ptr HttpLoadBalanceClient::post(const std::string &path,
                                            uint64_t timeout_ms,
                                            const std::map<std::string, std::string> &headers,
                                            const std::string &body)
{
    return post(path, HttpRequestOptions::FromTimeout(timeout_ms), headers, body);
}

HttpResult::ptr HttpLoadBalanceClient::post(const std::string &path,
                                            const HttpRequestOptions &options,
                                            const std::map<std::string, std::string> &headers,
                                            const std::string &body)
{
    return request(HttpMethod::POST, path, options, headers, body);
}

HttpResult::ptr HttpLoadBalanceClient::post(const std::string &path,
                                            const HttpRequestOptions &options,
                                            const HttpRetryOptions &retry_options,
                                            const std::map<std::string, std::string> &headers,
                                            const std::string &body)
{
    return request(HttpMethod::POST, path, options, retry_options, headers, body);
}

size_t HttpLoadBalanceClient::checkHealth(const std::string &path, uint64_t timeout_ms)
{
    return checkHealth(path, HttpRequestOptions::FromTimeout(timeout_ms));
}

size_t HttpLoadBalanceClient::checkHealth(const std::string &path,
                                          const HttpRequestOptions &options)
{
    std::string request_path = path.empty() ? "/" : path;
    std::vector<HttpEndpoint::ptr> endpoints;
    {
        MutexType::Lock lock(m_mutex);
        endpoints = m_endpoints;
    }

    size_t available = 0;
    for (auto &endpoint : endpoints)
    {
        if (!endpoint || !endpoint->m_pool)
        {
            continue;
        }

        //对这些终端做一次短的请求，如果能正常返回消息，那么就认为是UP状态，否则认为是DOWN状态
        HttpResult::ptr result = HttpClient::NormalizeResult(
            endpoint->m_pool->doRequest(HttpMethod::GET, request_path, options));
        if (result && result->result == (int)HttpResult::Error::OK)
        {
            endpoint->setStatus(HttpEndpointStatus::UP);
            ++available;
        }
        else
        {
            endpoint->setStatus(HttpEndpointStatus::DOWN);
        }
    }
    return available;
}

HttpEndpoint::ptr HttpLoadBalanceClient::selectEndpoint()
{
    switch (m_strategy)
    {
    case HttpLoadBalanceStrategy::RANDOM:
        return selectRandom();
    case HttpLoadBalanceStrategy::WEIGHTED_ROUND_ROBIN:
        return selectWeightedRoundRobin();
    case HttpLoadBalanceStrategy::LEAST_CONNECTION:
        return selectLeastConnection();
    case HttpLoadBalanceStrategy::ROUND_ROBIN:
    default:
        return selectRoundRobin();
    }
}

HttpEndpoint::ptr HttpLoadBalanceClient::selectRoundRobin()
{
    // 加锁，保护 m_endpoints 和 m_nextIndex。
    // 因为多个线程可能同时调用 selectRoundRobin，
    // 如果不加锁，m_nextIndex 可能被并发修改，导致选择混乱。
    MutexType::Lock lock(m_mutex);

    // 如果当前没有任何 Endpoint，直接返回空。
    if (m_endpoints.empty())
    {
        return nullptr;
    }

    // 记录 Endpoint 总数量。
    // 后面最多扫描 size 次，也就是最多扫描一整圈。
    size_t size = m_endpoints.size();

    // 从 m_nextIndex 开始，最多遍历一圈。
    // 目的是找到第一个状态为 UP 的 Endpoint。
    for (size_t i = 0; i < size; ++i)
    {
        // 计算当前要检查的下标。
        //
        // m_nextIndex 表示下一次应该优先尝试的位置。
        // i 表示从这个位置往后偏移几个节点。
        // % size 用来实现循环数组效果：
        // 例如 size = 3，下标超过 2 后又回到 0。
        size_t index = (m_nextIndex + i) % size;

        // 取出当前下标对应的 Endpoint。
        HttpEndpoint::ptr endpoint = m_endpoints[index];

        // 如果 endpoint 不为空，并且状态是 UP，
        // 说明这个节点当前可以被使用。
        if (endpoint && endpoint->getStatus() == HttpEndpointStatus::UP)
        {
            // 更新下一次轮询的起点。
            //
            // 假设这次选中了 index，
            // 那么下次应该从 index + 1 开始尝试，
            // 避免每次都选同一个节点。
            m_nextIndex = (index + 1) % size;

            // 返回选中的可用 Endpoint。
            endpoint->beginRequest();
            return endpoint;
        }
    }

    // 如果扫描了一整圈都没有找到 UP 的节点，
    // 说明所有 Endpoint 都不可用。
    return nullptr;
}

HttpEndpoint::ptr HttpLoadBalanceClient::selectRandom()
{
    // 加锁，保护 m_endpoints。
    // 因为其他线程可能正在增删 Endpoint 或修改 Endpoint 状态。
    MutexType::Lock lock(m_mutex);

    // 用来保存当前可用的 Endpoint。
    std::vector<HttpEndpoint::ptr> available;

    // 遍历所有 Endpoint。
    for (auto &endpoint : m_endpoints)
    {
        // 只把非空并且状态为 UP 的 Endpoint 放入 available。
        if (endpoint && endpoint->getStatus() == HttpEndpointStatus::UP)
        {
            available.push_back(endpoint);
        }
    }

    // 如果没有任何可用节点，返回空。
    if (available.empty())
    {
        return nullptr;
    }

    // 从 available 里面随机选一个。
    //
    // rand() % available.size() 会生成一个范围：
    // [0, available.size() - 1]
    //
    // 例如 available 有 3 个节点：
    // rand() % 3 的结果可能是 0、1、2。
    HttpEndpoint::ptr endpoint = available[rand() % available.size()];
    endpoint->beginRequest();
    return endpoint;
}

/**
 * @brief 按权重轮询选择一个可用 Endpoint。
 *
 * 这里的实现方式是：
 * - 每个 Endpoint 根据 weight 连续返回多次；
 * - 例如权重分别为 3、1；
 * - 那么选择顺序大致是：A、A、A、B、A、A、A、B...
 *
 * @return 选中的 Endpoint；如果没有可用 Endpoint，返回 nullptr
 */
HttpEndpoint::ptr HttpLoadBalanceClient::selectWeightedRoundRobin()
{
    MutexType::Lock lock(m_mutex);

    // 没有任何 Endpoint，直接返回空
    if (m_endpoints.empty())
    {
        return nullptr;
    }

    size_t size = m_endpoints.size();

    // 最多扫描一圈，避免所有 Endpoint 都不可用时死循环
    for (size_t i = 0; i < size; ++i)
    {
        // 如果当前加权轮询下标越界，重置到第一个 Endpoint
        // m_weightedIndex是第几个Endpoint，m_weightedReturned是当前Endpoint被返回的次数，超过了数量m_weightedIndex就++
        if (m_weightedIndex >= size)
        {
            m_weightedIndex = 0;
            m_weightedReturned = 0;
        }

        HttpEndpoint::ptr endpoint = m_endpoints[m_weightedIndex];

        // 只选择状态为 UP 的 Endpoint
        if (endpoint && endpoint->getStatus() == HttpEndpointStatus::UP)
        {
            uint32_t weight = endpoint->getWeight();

            // 当前 Endpoint 已经被返回一次
            ++m_weightedReturned;

            // 如果当前 Endpoint 已经按权重返回了足够多次，
            // 下次选择时切换到下一个 Endpoint
            if (m_weightedReturned >= weight)
            {
                m_weightedIndex = (m_weightedIndex + 1) % size;
                m_weightedReturned = 0;
            }

            // 记录该 Endpoint 开始处理一个请求
            //
            // 注意：调用方完成请求后，需要对应调用 endRequest()
            // 否则 LeastConnection 策略里的活跃请求数会不准确
            endpoint->beginRequest();

            return endpoint;
        }

        // 当前 Endpoint 不存在或不可用，跳过它，继续检查下一个
        m_weightedIndex = (m_weightedIndex + 1) % size;
        m_weightedReturned = 0;
    }

    // 扫描一圈后仍没有可用 Endpoint
    return nullptr;
}

/**
 * @brief 按最少活跃连接数选择一个可用 Endpoint。
 *
 * 选择规则：
 * - 只考虑状态为 UP 的 Endpoint；
 * - 比较每个 Endpoint 当前正在处理的请求数；
 * - 选择 active request 数量最少的那个。
 *
 * @return 选中的 Endpoint；如果没有可用 Endpoint，返回 nullptr
 */
HttpEndpoint::ptr HttpLoadBalanceClient::selectLeastConnection()
{
    MutexType::Lock lock(m_mutex);

    // 当前选中的 Endpoint
    HttpEndpoint::ptr selected;

    // 当前最小活跃请求数，初始化为 uint32_t 最大值
    uint32_t selected_active = std::numeric_limits<uint32_t>::max();

    //等遍历一遍之后真正拿到最小的才会开始请求。
    for (auto &endpoint : m_endpoints)
    {
        // 跳过空 Endpoint 和非 UP 状态的 Endpoint
        if (!endpoint || endpoint->getStatus() != HttpEndpointStatus::UP)
        {
            continue;
        }

        // 获取该 Endpoint 当前正在处理的请求数量
        uint32_t active = endpoint->getActiveRequestCount();

        // 如果还没选中 Endpoint，或者当前 Endpoint 的活跃请求数更少，
        // 就更新选中对象
        if (!selected || active < selected_active)
        {
            selected = endpoint;
            selected_active = active;
        }
    }

    // 选中后，记录该 Endpoint 开始处理一个请求
    //
    // 注意：调用方完成请求后，需要对应调用 endRequest()
    // 否则活跃请求数会持续增加，导致 LeastConnection 选择结果失真
    if (selected)
    {
        selected->beginRequest();
    }

    return selected;
}

} // namespace http
} // namespace sylar
