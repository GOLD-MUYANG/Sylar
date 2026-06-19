#ifndef __SYLAR_HTTP_CONCURRENCY_LIMITER_H__
#define __SYLAR_HTTP_CONCURRENCY_LIMITER_H__

#include "sylar/mutex.h"
#include <map>
#include <memory>
#include <stdint.h>
#include <string>

namespace sylar
{
namespace http
{

/**
 * @brief HTTP 客户端并发数限制参数。
 *
 * 0 表示对应维度不限制。
 */
struct HttpConcurrencyLimitOptions
{
    uint32_t max_global_concurrency = 0;   //进程内所有 limiter 实例共享
    uint32_t max_service_concurrency = 0;  //当前 HttpConcurrencyLimiter 对象内部共享
    uint32_t max_endpoint_concurrency = 0; //当前 limiter 内，每个 endpoint 单独计数
    uint32_t max_global_qps = 0;           //进程内所有 limiter 实例共享
    uint32_t max_service_qps = 0;          //当前 HttpConcurrencyLimiter 对象内部共享
    uint32_t max_endpoint_qps = 0;         //当前 limiter 内，每个 endpoint 单独计数
};

class HttpConcurrencyLimiter;

/**
 * @brief 并发配额占用守卫。
 *
 * tryAcquire() 成功后返回该对象；对象析构时自动归还配额。
 */
class HttpConcurrencyLimitGuard : Noncopyable
{
public:
    typedef std::shared_ptr<HttpConcurrencyLimitGuard> ptr;

    ~HttpConcurrencyLimitGuard();

private:
    friend class HttpConcurrencyLimiter;

    HttpConcurrencyLimitGuard(std::shared_ptr<HttpConcurrencyLimiter> limiter,
                              const std::string &endpoint_key,
                              bool global_acquired);

private:
    std::shared_ptr<HttpConcurrencyLimiter> m_limiter;
    std::string m_endpointKey;
    bool m_globalAcquired = false;
};

/**
 * @brief HTTP 客户端并发限流器。
 *
 * 负责全局、服务和 Endpoint 三个维度的并发计数；调用方只需要持有返回的 guard。
 */
class HttpConcurrencyLimiter : public std::enable_shared_from_this<HttpConcurrencyLimiter>,
                               Noncopyable
{
public:
    typedef std::shared_ptr<HttpConcurrencyLimiter> ptr;
    typedef Mutex MutexType;

    static HttpConcurrencyLimiter::ptr Create(const HttpConcurrencyLimitOptions &options);

    HttpConcurrencyLimitGuard::ptr tryAcquire(const std::string &endpoint_key);

private:
    friend class HttpConcurrencyLimitGuard;

    struct TokenBucket
    {
        uint32_t capacity = 0;     //桶容量，也就是最大能存多少令牌
        double tokens = 0;         //当前剩余令牌数
        uint64_t lastRefillMs = 0; //上次补充令牌的时间，毫秒
    };

    explicit HttpConcurrencyLimiter(const HttpConcurrencyLimitOptions &options);

    bool tryAcquireGlobal();
    void releaseGlobal();
    bool tryAcquireLocal(const std::string &endpoint_key);
    void releaseLocal(const std::string &endpoint_key);
    bool tryAcquireQps(const std::string &endpoint_key);
    static bool tryAcquireToken(TokenBucket &bucket, uint32_t qps, uint64_t now_ms);
    static void releaseToken(TokenBucket &bucket);

private:
    HttpConcurrencyLimitOptions m_options;
    uint32_t m_serviceActive = 0;
    std::map<std::string, uint32_t> m_endpointActive;
    TokenBucket m_serviceQpsBucket;
    std::map<std::string, TokenBucket> m_endpointQpsBuckets;
    MutexType m_mutex;
};

} // namespace http
} // namespace sylar

#endif
