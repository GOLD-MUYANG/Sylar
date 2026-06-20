#include "http_circuit_breaker.h"
#include "sylar/util.h" // 使用 sylar::GetCurrentMS() 获取当前毫秒时间

namespace sylar
{
namespace http
{

/**
 * @brief Guard 构造函数。
 *
 * endpoint_key 表示本次准入对应哪个后端节点。
 */
HttpCircuitBreakerGuard::HttpCircuitBreakerGuard(const std::string &endpoint_key)
    : m_endpointKey(endpoint_key)
{
}

/**
 * @brief 工厂函数。
 *
 * 使用 new 调私有构造函数，返回 shared_ptr。
 */
HttpCircuitBreaker::ptr HttpCircuitBreaker::Create(const HttpCircuitBreakerOptions &options)
{
    return HttpCircuitBreaker::ptr(new HttpCircuitBreaker(options));
}

/**
 * @brief 构造熔断器，并修正非法参数。
 */
HttpCircuitBreaker::HttpCircuitBreaker(const HttpCircuitBreakerOptions &options)
    : m_options(options)
{
    // 窗口大小不能为 0。
    // 如果为 0，后面 recentResults.size() / pop_front 等逻辑没有意义。
    // 这里修正为 1，保证至少能存一条结果。
    if (m_options.failure_window_size == 0)
    {
        m_options.failure_window_size = 1;
    }

    // HALF_OPEN 至少要允许 1 个探测请求。
    // 如果为 0，熔断器进入 HALF_OPEN 后永远没有请求能通过，也就无法恢复。
    if (m_options.half_open_max_requests == 0)
    {
        m_options.half_open_max_requests = 1;
    }
}

/**
 * @brief 请求前调用：判断某个 endpoint 当前是否允许发请求。
 *
 * 返回：
 *   非空 Guard：允许请求。
 *   nullptr：不允许请求，调用方应直接返回 CIRCUIT_OPEN 之类的错误。
 */
HttpCircuitBreakerGuard::ptr HttpCircuitBreaker::tryAcquire(const std::string &endpoint_key)
{
    // 熔断器未启用时，永远放行。
    // 这样不会影响旧客户端行为。
    if (!m_options.enabled)
    {
        return HttpCircuitBreakerGuard::ptr(new HttpCircuitBreakerGuard(endpoint_key));
    }

    // 获取当前时间，用于判断 OPEN 是否已经过了冷却期。
    uint64_t now_ms = sylar::GetCurrentMS();

    // 所有 endpoint 状态访问都要加锁。
    MutexType::Lock lock(m_mutex);

    // 找到当前 endpoint 的状态。
    // 如果不存在，会创建默认 CLOSED 状态。
    EndpointState &state = getOrCreateState(endpoint_key);

    // 如果当前是 OPEN，说明熔断正在打开。
    // 此时不能立刻放请求，要先判断冷却时间是否结束。
    if (state.state == HttpCircuitBreakerState::OPEN)
    {
        // now_ms < openedAtMs 是防御性判断：
        // 如果系统时间异常回拨，就继续拒绝请求。
        //
        // now_ms - openedAtMs < open_timeout_ms：
        // 表示还没过冷却时间，继续拒绝。
        if (now_ms < state.openedAtMs || now_ms - state.openedAtMs < m_options.open_timeout_ms)
        {
            return nullptr;
        }

        // 冷却时间到了，进入 HALF_OPEN。
        // 不是直接恢复 CLOSED，而是先放少量请求探测。
        state.state = HttpCircuitBreakerState::HALF_OPEN;

        // 刚进入 HALF_OPEN，没有正在进行的探测请求。
        state.halfOpenActive = 0;
    }

    // HALF_OPEN 状态只允许有限数量的探测请求通过。
    if (state.state == HttpCircuitBreakerState::HALF_OPEN)
    {
        // 如果当前探测请求数已经达到上限，拒绝。
        if (state.halfOpenActive >= m_options.half_open_max_requests)
        {
            return nullptr;
        }

        // 放行一个探测请求，计数 +1。
        // 后面请求完成时，需要在 onRequestComplete() 里减回来。
        ++state.halfOpenActive;
    }

    // CLOSED 状态会直接走到这里。
    // HALF_OPEN 未超限也会走到这里。
    return HttpCircuitBreakerGuard::ptr(new HttpCircuitBreakerGuard(endpoint_key));
}

/**
 * @brief 请求完成后调用：上报请求结果，并更新熔断状态。
 */
void HttpCircuitBreaker::onRequestComplete(const std::string &endpoint_key,
                                           const HttpResult::ptr &result)
{
    // 未启用熔断时，不做任何统计。
    if (!m_options.enabled)
    {
        return;
    }

    // 先把 HttpResult 转成是否失败。
    bool failure = isFailure(result);

    // 当前时间用于打开熔断时记录 openedAtMs。
    uint64_t now_ms = sylar::GetCurrentMS();

    MutexType::Lock lock(m_mutex);
    EndpointState &state = getOrCreateState(endpoint_key);

    // HALF_OPEN 是恢复探测状态。
    // 这个状态下的请求结果会直接决定：
    //   成功：关闭熔断，恢复 CLOSED。
    //   失败：重新打开熔断，回到 OPEN。
    if (state.state == HttpCircuitBreakerState::HALF_OPEN)
    {
        // 探测请求完成了，把正在探测的数量减掉。
        if (state.halfOpenActive > 0)
        {
            --state.halfOpenActive;
        }

        if (failure)
        {
            // 探测失败，说明后端还没恢复，重新打开熔断。
            open(state, now_ms);
        }
        else
        {
            // 探测成功，认为后端恢复，关闭熔断。
            close(state);
        }

        return;
    }

    // 如果已经是 OPEN，正常情况下请求不会被放行。
    // 这里做防御：即使有结果上报，也不再统计。
    if (state.state == HttpCircuitBreakerState::OPEN)
    {
        return;
    }

    // 走到这里，说明当前是 CLOSED。
    // 记录本次结果到最近窗口。
    // true  = 失败
    // false = 成功
    state.recentResults.push_back(failure);

    // 窗口超过配置大小时，删除最老的结果。
    while (state.recentResults.size() > m_options.failure_window_size)
    {
        state.recentResults.pop_front();
    }

    // 更新连续失败次数。
    if (failure)
    {
        ++state.consecutiveFailures;
    }
    else
    {
        // 成功一次，连续失败清零。
        state.consecutiveFailures = 0;
    }

    // 判断是否满足熔断打开条件。
    if (shouldOpen(state))
    {
        open(state, now_ms);
    }
}

/**
 * @brief 获取某个 endpoint 的当前状态。
 */
HttpCircuitBreakerState HttpCircuitBreaker::getState(const std::string &endpoint_key)
{
    // 未启用时，外部看到的状态永远是 CLOSED。
    if (!m_options.enabled)
    {
        return HttpCircuitBreakerState::CLOSED;
    }

    MutexType::Lock lock(m_mutex);
    return getOrCreateState(endpoint_key).state;
}

/**
 * @brief 清空某个 endpoint 的状态。
 *
 * 下一次访问这个 endpoint 时，会重新创建默认 CLOSED 状态。
 */
void HttpCircuitBreaker::reset(const std::string &endpoint_key)
{
    MutexType::Lock lock(m_mutex);
    m_states.erase(endpoint_key);
}

/**
 * @brief 判断 HttpResult 是否算失败。
 *
 * 这里不是所有非 OK 都算失败。
 * RATE_LIMITED / CIRCUIT_OPEN 不算失败，因为它们是客户端保护机制主动拒绝，
 * 不应该反过来导致 endpoint 熔断。
 */
bool HttpCircuitBreaker::isFailure(const HttpResult::ptr &result) const
{
    // 没有结果，视为失败。
    if (!result)
    {
        return true;
    }

    HttpResult::Error error = (HttpResult::Error)result->result;

    switch (error)
    {
    case HttpResult::Error::OK:
        // 请求成功。
        return false;

    case HttpResult::Error::HTTP_STATUS_ERROR:
        // HTTP 状态码错误时，只把 5xx 算作后端失败。
        // 4xx 通常是客户端请求问题，不一定代表服务端故障。
        return result->response && (int)result->response->getStatus() >= 500;

    case HttpResult::Error::RATE_LIMITED:
    case HttpResult::Error::CIRCUIT_OPEN:
        // 限流和熔断打开是客户端主动保护。
        // 如果把它们也算失败，会导致保护机制互相放大。
        return false;

    default:
        // 其他错误，例如连接失败、超时、解析失败等，算失败。
        return true;
    }
}

/**
 * @brief 判断是否应该打开熔断。
 *
 * 当前有两套触发规则：
 *   1. 连续失败次数达到阈值。
 *   2. 最近窗口失败率达到阈值。
 */
bool HttpCircuitBreaker::shouldOpen(const EndpointState &state) const
{
    // 规则 1：连续失败次数触发熔断。
    if (m_options.consecutive_failure_threshold > 0 &&
        state.consecutiveFailures >= m_options.consecutive_failure_threshold)
    {
        return true;
    }

    // 如果失败率阈值关闭，或者样本数不足，则不使用失败率规则。
    if (m_options.failure_rate_threshold == 0 ||
        state.recentResults.size() < m_options.failure_rate_min_request)
    {
        return false;
    }

    // 统计最近窗口中的失败次数。
    uint32_t failure_count = 0;
    for (bool failure : state.recentResults)
    {
        if (failure)
        {
            ++failure_count;
        }
    }

    // 计算失败率，整数百分比。
    uint32_t failure_rate = failure_count * 100 / state.recentResults.size();

    // 规则 2：失败率达到阈值，打开熔断。
    return failure_rate >= m_options.failure_rate_threshold;
}

/**
 * @brief 打开熔断。
 */
void HttpCircuitBreaker::open(EndpointState &state, uint64_t now_ms)
{
    // 进入 OPEN 状态。
    state.state = HttpCircuitBreakerState::OPEN;

    // 记录打开时间，用于后面判断冷却时间是否结束。
    state.openedAtMs = now_ms;

    // OPEN 状态下没有探测请求。
    state.halfOpenActive = 0;
}

/**
 * @brief 关闭熔断，恢复正常状态。
 */
void HttpCircuitBreaker::close(EndpointState &state)
{
    // 恢复 CLOSED。
    state.state = HttpCircuitBreakerState::CLOSED;

    // 清空连续失败次数。
    state.consecutiveFailures = 0;

    // 清空 HALF_OPEN 探测计数。
    state.halfOpenActive = 0;

    // 清空 OPEN 时间。
    state.openedAtMs = 0;

    // 清空最近窗口。
    // 这样恢复后重新统计，避免旧失败结果影响新状态。
    state.recentResults.clear();
}

/**
 * @brief 获取或创建 endpoint 状态。
 *
 * std::map::operator[] 的特点：
 *   如果 endpoint_key 已存在，返回已有状态。
 *   如果 endpoint_key 不存在，插入一个默认 EndpointState。
 */
HttpCircuitBreaker::EndpointState &
HttpCircuitBreaker::getOrCreateState(const std::string &endpoint_key)
{
    return m_states[endpoint_key];
}

} // namespace http
} // namespace sylar