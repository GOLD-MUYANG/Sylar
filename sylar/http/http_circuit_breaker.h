#ifndef __SYLAR_HTTP_CIRCUIT_BREAKER_H__
#define __SYLAR_HTTP_CIRCUIT_BREAKER_H__

#include "http_connection.h"   // 使用 HttpResult，判断 HTTP 请求结果
#include "sylar/mutex.h"       // 使用 sylar::Mutex 做线程安全保护
#include "sylar/noncopyable.h" // 禁止对象拷贝
#include <deque>               // 用于保存最近请求结果窗口
#include <map>                 // endpoint_key -> EndpointState
#include <memory>
#include <stdint.h>
#include <string>

namespace sylar
{
namespace http
{

/**
 * @brief Endpoint 熔断状态。
 *
 * CLOSED:
 *   正常状态，请求允许通过，并统计失败情况。
 *
 * OPEN:
 *   熔断打开状态，请求直接拒绝。
 *   只有等待 open_timeout_ms 后，才可能进入 HALF_OPEN。
 *
 * HALF_OPEN:
 *   半开探测状态。
 *   只允许少量请求通过，用来测试后端是否恢复。
 */
enum class HttpCircuitBreakerState
{
    CLOSED = 0,
    OPEN = 1,
    HALF_OPEN = 2,
};

/**
 * @brief HTTP 客户端熔断参数。
 *
 * 说明：
 *   这里的熔断是按 endpoint_key 维度做的。
 *   每个 endpoint_key 会有自己独立的状态和失败统计。
 *
 * 注意：
 *   0 表示对应统计规则不生效。
 */
struct HttpCircuitBreakerOptions
{
    // 是否启用熔断器。
    // 默认 false，避免接入后改变旧客户端行为。
    bool enabled = false;

    // 连续失败阈值。
    // 例如值为 5，表示连续失败 5 次后打开熔断。
    // 如果设置为 0，则不根据连续失败次数触发熔断。
    uint32_t consecutive_failure_threshold = 5;

    // 最近窗口失败率阈值，单位是百分比。
    // 例如 50 表示最近窗口中失败率 >= 50% 时触发熔断。
    // 如果设置为 0，则不根据失败率触发熔断。
    uint32_t failure_rate_threshold = 50;

    // 触发失败率统计所需的最少请求数。
    // 例如值为 10，表示 recentResults 至少有 10 条记录后，
    // 才会计算失败率，避免样本太少导致误判。
    uint32_t failure_rate_min_request = 10;

    // 最近结果窗口大小。
    // 例如值为 20，表示最多记录最近 20 次请求成功/失败结果。
    uint32_t failure_window_size = 20;

    // OPEN 状态持续时间。
    // 熔断打开后，至少等待这么久才允许进入 HALF_OPEN 探测。
    uint64_t open_timeout_ms = 5000;

    // HALF_OPEN 状态下允许同时放行的探测请求数。
    // 通常第一版设置为 1 最安全。
    uint32_t half_open_max_requests = 1;
};

class HttpCircuitBreaker;

/**
 * @brief 熔断请求准入守卫。
 *
 * tryAcquire() 返回非空 Guard，表示本次请求可以发出。
 * tryAcquire() 返回 nullptr，表示当前 endpoint 被熔断，请求不应该发出。
 *
 * 注意：
 *   这个 Guard 当前只表示“准入成功”。
 *   它没有在析构函数里自动调用 onRequestComplete()。
 *   所以调用方请求完成后，仍然必须手动调用 onRequestComplete() 上报结果。
 */
class HttpCircuitBreakerGuard : Noncopyable
{
public:
    typedef std::shared_ptr<HttpCircuitBreakerGuard> ptr;

private:
    // 只允许 HttpCircuitBreaker 创建 Guard。
    // 外部不能直接 new HttpCircuitBreakerGuard。
    friend class HttpCircuitBreaker;

    explicit HttpCircuitBreakerGuard(const std::string &endpoint_key);

private:
    // 当前 Guard 对应的 endpoint。
    // 当前实现里只是保存了这个 key，没有主动用于自动回收或自动上报。
    std::string m_endpointKey;
};

/**
 * @brief Endpoint 维度 HTTP 熔断器。
 *
 * 职责：
 *   1. 判断请求是否允许发出。
 *   2. 统计请求成功/失败。
 *   3. 管理 CLOSED / OPEN / HALF_OPEN 状态切换。
 *
 * 不负责：
 *   1. 不负责选择 Endpoint。
 *   2. 不负责真正发送 HTTP 请求。
 *   3. 不负责构造降级响应。
 */
class HttpCircuitBreaker : public std::enable_shared_from_this<HttpCircuitBreaker>, Noncopyable
{
public:
    typedef std::shared_ptr<HttpCircuitBreaker> ptr;
    typedef Mutex MutexType;

    /**
     * @brief 创建熔断器实例。
     */
    static HttpCircuitBreaker::ptr Create(const HttpCircuitBreakerOptions &options);

    /**
     * @brief 请求准入判断。
     *
     * 返回值：
     *   非空：允许请求。
     *   nullptr：当前 endpoint 被熔断，请求应直接失败。
     */
    HttpCircuitBreakerGuard::ptr tryAcquire(const std::string &endpoint_key);

    /**
     * @brief 请求完成后上报结果。
     *
     * 请求真正完成后，调用方必须把 HttpResult 传回来。
     * 熔断器根据 result 判断成功/失败，并更新状态。
     */
    void onRequestComplete(const std::string &endpoint_key, const HttpResult::ptr &result);

    /**
     * @brief 获取某个 endpoint 当前熔断状态。
     */
    HttpCircuitBreakerState getState(const std::string &endpoint_key);

    /**
     * @brief 重置某个 endpoint 的熔断状态和统计数据。
     */
    void reset(const std::string &endpoint_key);

private:
    /**
     * @brief 单个 endpoint 的内部状态。
     */
    struct EndpointState
    {
        // 当前熔断状态，默认 CLOSED。
        HttpCircuitBreakerState state = HttpCircuitBreakerState::CLOSED;

        // 当前连续失败次数。
        // 成功一次会清零。
        uint32_t consecutiveFailures = 0;

        // HALF_OPEN 状态下，当前正在执行的探测请求数。
        uint32_t halfOpenActive = 0;

        // 最近一次进入 OPEN 状态的时间，单位毫秒。
        uint64_t openedAtMs = 0;

        // 最近请求结果窗口。
        // true  表示失败。
        // false 表示成功。
        std::deque<bool> recentResults;
    };

    /**
     * @brief 构造函数私有化，只能通过 Create 创建。
     */
    explicit HttpCircuitBreaker(const HttpCircuitBreakerOptions &options);

    /**
     * @brief 判断请求结果是否算失败。
     */
    bool isFailure(const HttpResult::ptr &result) const;

    /**
     * @brief 判断当前状态是否满足打开熔断条件。
     */
    bool shouldOpen(const EndpointState &state) const;

    /**
     * @brief 进入 OPEN 状态。
     */
    void open(EndpointState &state, uint64_t now_ms);

    /**
     * @brief 关闭熔断，恢复 CLOSED 状态，并清空统计数据。
     */
    void close(EndpointState &state);

    /**
     * @brief 获取 endpoint 状态。
     *
     * 如果 endpoint_key 不存在，会创建一个默认 EndpointState。
     */
    EndpointState &getOrCreateState(const std::string &endpoint_key);

private:
    // 熔断参数。
    HttpCircuitBreakerOptions m_options;

    // 每个 endpoint 独立维护状态。
    std::map<std::string, EndpointState> m_states;

    // 保护 m_states 和 EndpointState。
    MutexType m_mutex;
};

} // namespace http
} // namespace sylar

#endif