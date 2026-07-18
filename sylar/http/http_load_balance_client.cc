#include "http_load_balance_client.h"
#include "sylar/log.h"
#include <sstream>
#include <unistd.h>

namespace sylar
{
namespace http
{

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

const char *HttpEndpointStatusToString(HttpEndpointStatus status)
{
    switch (status)
    {
    case HttpEndpointStatus::UP:
        return "UP";
    case HttpEndpointStatus::DOWN:
        return "DOWN";
    default:
        return "UNKNOWN";
    }
}

const char *HttpCircuitBreakerStateToString(HttpCircuitBreakerState state)
{
    switch (state)
    {
    case HttpCircuitBreakerState::CLOSED:
        return "CLOSED";
    case HttpCircuitBreakerState::OPEN:
        return "OPEN";
    case HttpCircuitBreakerState::HALF_OPEN:
        return "HALF_OPEN";
    default:
        return "UNKNOWN";
    }
}

static void AppendTraceAttempt(HttpLoadBalanceRequestTrace *trace,
                               const std::string &endpoint_key,
                               const std::string &outcome,
                               const HttpResult::ptr &result,
                               const std::string &reason)
{
    if (!trace)
    {
        return;
    }

    HttpLoadBalanceAttemptTrace attempt;
    attempt.endpoint_key = endpoint_key;
    attempt.outcome = outcome;
    attempt.result = result ? result->result : 0;
    attempt.http_status = result && result->response ? (int)result->response->getStatus() : 0;
    attempt.reason = reason;
    trace->attempts.push_back(attempt);
}

static void AppendDownEndpointTrace(HttpLoadBalanceRequestTrace *trace,
                                    const std::vector<HttpEndpoint::ptr> &endpoints)
{
    if (!trace)
    {
        return;
    }

    for (auto &endpoint : endpoints)
    {
        if (endpoint && endpoint->getStatus() == HttpEndpointStatus::DOWN)
        {
            AppendTraceAttempt(trace, endpoint->getLimitKey(), "skipped_down", nullptr,
                               "endpoint health down");
        }
    }
}

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

HttpEndpointStatusSnapshot HttpEndpoint::snapshot(HttpCircuitBreakerState circuit_state) const
{
    MutexType::Lock lock(m_mutex);

    HttpEndpointStatusSnapshot result;
    result.endpoint_key = getLimitKey();
    result.host = m_host;
    result.port = m_port;
    result.ssl = m_ssl;
    result.health_status = m_status;
    result.circuit_state = circuit_state;
    result.active_requests = m_activeRequests;
    result.success_count = m_successCount;
    result.failure_count = m_failureCount;
    result.rate_limited_count = m_rateLimitedCount;
    result.last_failure_reason = m_lastFailureReason;
    return result;
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

void HttpEndpoint::recordSuccess()
{
    MutexType::Lock lock(m_mutex);
    ++m_successCount;
}

void HttpEndpoint::recordFailure(const std::string &reason)
{
    MutexType::Lock lock(m_mutex);
    ++m_failureCount;
    m_lastFailureReason = reason;
}

void HttpEndpoint::recordRateLimited(const std::string &reason)
{
    MutexType::Lock lock(m_mutex);
    ++m_rateLimitedCount;
    m_lastFailureReason = reason;
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

    sylar::load_balance::CandidateAccessors<HttpEndpoint::ptr> accessors;
    accessors.key = [](const HttpEndpoint::ptr &endpoint) {
        return endpoint ? endpoint->getLimitKey() : std::string();
    };
    accessors.available = [](const HttpEndpoint::ptr &endpoint) {
        return endpoint && endpoint->getStatus() == HttpEndpointStatus::UP;
    };
    accessors.weight =
        [](const HttpEndpoint::ptr &endpoint) { return endpoint ? endpoint->getWeight() : 0; };
    accessors.active_requests = [](const HttpEndpoint::ptr &endpoint) {
        return endpoint ? endpoint->getActiveRequestCount() : 0;
    };
    accessors.on_selected = [](const HttpEndpoint::ptr &endpoint) {
        if (endpoint)
        {
            endpoint->beginRequest();
        }
    };

    auto selector = sylar::load_balance::CreateCandidateSelector(strategy, accessors);
    if (!selector)
    {
        SYLAR_LOG_ERROR(g_logger) << "HttpLoadBalanceClient selector create failed";
        return nullptr;
    }
    return HttpLoadBalanceClient::ptr(new HttpLoadBalanceClient(
        valid, selector, limit_options, circuit_options));
}

HttpLoadBalanceClient::HttpLoadBalanceClient(const std::vector<HttpEndpoint::ptr> &endpoints,
                                             sylar::load_balance::CandidateSelector<
                                                 HttpEndpoint::ptr>::ptr selector,
                                             const HttpConcurrencyLimitOptions &limit_options,
                                             const HttpCircuitBreakerOptions &circuit_options)
    : m_endpoints(endpoints), m_selector(selector),
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
                                               const std::string &body,
                                               HttpLoadBalanceRequestTrace *trace)
{
    std::string request_path = path.empty() ? "/" : path;
    HttpResult::ptr result;
    size_t endpoint_count = 0;
    {
        MutexType::Lock lock(m_mutex);
        endpoint_count = m_endpoints.size();
        AppendDownEndpointTrace(trace, m_endpoints);
    }

    uint32_t total_attempts = 0;
    for (uint32_t retry_index = 0; retry_index <= retry_options.max_retry; ++retry_index)
    {
        // 记录当前这一轮请求已经尝试过的 endpoint。
        // 如果某个 endpoint 被限流、熔断或请求失败，本轮会排除它，再选其他 endpoint。
        std::vector<std::string> tried_endpoint_keys;
        for (size_t endpoint_try = 0; endpoint_try < endpoint_count; ++endpoint_try)
        {
            if (retry_options.max_total_attempts > 0 &&
                total_attempts >= retry_options.max_total_attempts)
            {
                if (result)
                {
                    return result;
                }
                return std::make_shared<HttpResult>(
                    (int)HttpResult::Error::CONNECT_FAIL, nullptr,
                    "http load balance client max total attempts exhausted");
            }

            //选择一个终端
            HttpEndpoint::ptr endpoint = selectEndpoint(tried_endpoint_keys);
            if (!endpoint)
            {
                break;
            }
            ++total_attempts;
            std::string endpoint_key = endpoint->getLimitKey();
            tried_endpoint_keys.push_back(endpoint_key);

            SYLAR_LOG_DEBUG(g_logger)
                << "HttpLoadBalanceClient request method=" << HttpMethodToString(method)
                << " endpoint=" << endpoint->getHost() << ":" << endpoint->getPort()
                << " path=" << request_path;

            // 用选择出来的终端发请求；selectEndpoint() 已经占用活跃请求名额。
            HttpConcurrencyLimitGuard::ptr limit_guard =
                m_limiter ? m_limiter->tryAcquire(endpoint_key) : nullptr;
            if (!limit_guard)
            {
                endpoint->recordRateLimited("http client concurrency limited");
                endpoint->endRequest();
                result = std::make_shared<HttpResult>((int)HttpResult::Error::RATE_LIMITED, nullptr,
                                                      "http client concurrency limited");
                AppendTraceAttempt(trace, endpoint_key, "rate_limited", result, result->error);
                continue;
            }

            HttpCircuitBreakerGuard::ptr circuit_guard =
                m_circuitBreaker ? m_circuitBreaker->tryAcquire(endpoint_key) : nullptr;
            if (!circuit_guard)
            {
                endpoint->recordFailure("http client circuit open");
                endpoint->endRequest();
                result = std::make_shared<HttpResult>((int)HttpResult::Error::CIRCUIT_OPEN, nullptr,
                                                      "http client circuit open");
                AppendTraceAttempt(trace, endpoint_key, "circuit_open", result, result->error);
                continue;
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
            if (result && result->result == (int)HttpResult::Error::OK)
            {
                endpoint->recordSuccess();
                AppendTraceAttempt(trace, endpoint_key, "success", result, "");
            }
            else
            {
                endpoint->recordFailure(result ? result->error : "empty http result");
                AppendTraceAttempt(trace, endpoint_key, "failure", result,
                                   result ? result->error : "empty http result");
            }

            if (!HttpClient::ShouldRetry(method, result, retry_options))
            {
                return result;
            }
        }

        if (retry_index >= retry_options.max_retry)
        {
            if (result)
            {
                return result;
            }
            return std::make_shared<HttpResult>((int)HttpResult::Error::CONNECT_FAIL, nullptr,
                                                "no available endpoint");
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
                                            const std::string &body,
                                            HttpLoadBalanceRequestTrace *trace)
{
    return request(HttpMethod::POST, path, options, retry_options, headers, body, trace);
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

std::vector<HttpEndpointStatusSnapshot> HttpLoadBalanceClient::getStatusSnapshots()
{
    std::vector<HttpEndpoint::ptr> endpoints;
    {
        MutexType::Lock lock(m_mutex);
        endpoints = m_endpoints;
    }

    std::vector<HttpEndpointStatusSnapshot> snapshots;
    snapshots.reserve(endpoints.size());
    for (auto &endpoint : endpoints)
    {
        if (!endpoint)
        {
            continue;
        }

        HttpCircuitBreakerState circuit_state = HttpCircuitBreakerState::CLOSED;
        if (m_circuitBreaker)
        {
            circuit_state = m_circuitBreaker->getState(endpoint->getLimitKey());
        }
        snapshots.push_back(endpoint->snapshot(circuit_state));
    }
    return snapshots;
}

HttpEndpoint::ptr
HttpLoadBalanceClient::selectEndpoint(const std::vector<std::string> &tried_endpoint_keys)
{
    MutexType::Lock lock(m_mutex);
    if (!m_selector)
    {
        return nullptr;
    }
    HttpEndpoint::ptr selected;
    if (!m_selector->select("http", m_endpoints, tried_endpoint_keys, &selected))
    {
        return nullptr;
    }
    return selected;
}

} // namespace http
} // namespace sylar
