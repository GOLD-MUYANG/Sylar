#include "http_concurrency_limiter.h"
#include "sylar/util.h"
/*
这个文件主要是实现的并发的限制，当有请求的时候就把并发的变量++，请求结束就--，主要是处理这里面的逻辑，因为比较繁琐，就容易出错。
其他的倒也不是很难理解。
*/
namespace sylar
{
namespace http
{

namespace
{

typedef Mutex GlobalLimitMutexType;

GlobalLimitMutexType g_global_limit_mutex;
uint32_t g_global_active_requests = 0;

} // namespace

HttpConcurrencyLimitGuard::HttpConcurrencyLimitGuard(
    std::shared_ptr<HttpConcurrencyLimiter> limiter,
    const std::string &endpoint_key,
    bool global_acquired)
    : m_limiter(limiter), m_endpointKey(endpoint_key), m_globalAcquired(global_acquired)
{
}

HttpConcurrencyLimitGuard::~HttpConcurrencyLimitGuard()
{
    if (!m_limiter)
    {
        return;
    }

    m_limiter->releaseLocal(m_endpointKey);
    if (m_globalAcquired)
    {
        m_limiter->releaseGlobal();
    }
}

HttpConcurrencyLimiter::ptr
HttpConcurrencyLimiter::Create(const HttpConcurrencyLimitOptions &options)
{
    return HttpConcurrencyLimiter::ptr(new HttpConcurrencyLimiter(options));
}

HttpConcurrencyLimiter::HttpConcurrencyLimiter(const HttpConcurrencyLimitOptions &options)
    : m_options(options)
{
}

HttpConcurrencyLimitGuard::ptr HttpConcurrencyLimiter::tryAcquire(const std::string &endpoint_key)
{
    //如果 max_global_concurrency > 0，说明需要全局限流
    bool global_acquired = m_options.max_global_concurrency > 0;
    if (global_acquired && !tryAcquireGlobal())
    {
        return nullptr;
    }

    /**
    全局名额已经成功获取；
    但是本地名额失败；
    这个请求最终不能执行；
    所以刚才占用的全局名额必须还回去。
    否则全局计数会泄漏。
    */
    if (!tryAcquireLocal(endpoint_key))
    {
        if (global_acquired)
        {
            releaseGlobal();
        }
        return nullptr;
    }
    if (!tryAcquireQps(endpoint_key))
    {
        releaseLocal(endpoint_key);
        if (global_acquired)
        {
            releaseGlobal();
        }
        return nullptr;
    }
    // 只要调用方持有这个 Guard，就表示：当前请求正在占用并发名额。
    return HttpConcurrencyLimitGuard::ptr(
        new HttpConcurrencyLimitGuard(shared_from_this(), endpoint_key, global_acquired));
}

bool HttpConcurrencyLimiter::tryAcquireGlobal()
{
    GlobalLimitMutexType::Lock lock(g_global_limit_mutex);
    if (g_global_active_requests >= m_options.max_global_concurrency)
    {
        return false;
    }
    ++g_global_active_requests;
    return true;
}

void HttpConcurrencyLimiter::releaseGlobal()
{
    GlobalLimitMutexType::Lock lock(g_global_limit_mutex);
    if (g_global_active_requests > 0)
    {
        --g_global_active_requests;
    }
}

bool HttpConcurrencyLimiter::tryAcquireLocal(const std::string &endpoint_key)
{
    MutexType::Lock lock(m_mutex);
    if (m_options.max_service_concurrency > 0 &&
        m_serviceActive >= m_options.max_service_concurrency)
    {
        return false;
    }

    uint32_t endpoint_active = m_endpointActive[endpoint_key];
    if (m_options.max_endpoint_concurrency > 0 &&
        endpoint_active >= m_options.max_endpoint_concurrency)
    {
        return false;
    }

    ++m_serviceActive;
    ++m_endpointActive[endpoint_key];
    return true;
}

void HttpConcurrencyLimiter::releaseLocal(const std::string &endpoint_key)
{
    MutexType::Lock lock(m_mutex);
    if (m_serviceActive > 0)
    {
        --m_serviceActive;
    }

    auto it = m_endpointActive.find(endpoint_key);
    if (it == m_endpointActive.end())
    {
        return;
    }
    if (it->second > 1)
    {
        --it->second;
    }
    else
    {
        m_endpointActive.erase(it);
    }
}

bool HttpConcurrencyLimiter::tryAcquireQps(const std::string &endpoint_key)
{
    uint64_t now_ms = sylar::GetCurrentMS();
    bool global_acquired = false;
    static TokenBucket s_global_qps_bucket;
    if (m_options.max_global_qps > 0)
    {
        GlobalLimitMutexType::Lock lock(g_global_limit_mutex);
        if (!tryAcquireToken(s_global_qps_bucket, m_options.max_global_qps, now_ms))
        {
            return false;
        }
        global_acquired = true;
    }

    {
        MutexType::Lock lock(m_mutex);
        if (!tryAcquireToken(m_serviceQpsBucket, m_options.max_service_qps, now_ms))
        {
            if (global_acquired)
            {
                GlobalLimitMutexType::Lock global_lock(g_global_limit_mutex);
                releaseToken(s_global_qps_bucket);
            }
            return false;
        }

        TokenBucket &endpoint_bucket = m_endpointQpsBuckets[endpoint_key];
        if (!tryAcquireToken(endpoint_bucket, m_options.max_endpoint_qps, now_ms))
        {
            releaseToken(m_serviceQpsBucket);
            if (global_acquired)
            {
                GlobalLimitMutexType::Lock global_lock(g_global_limit_mutex);
                releaseToken(s_global_qps_bucket);
            }
            return false;
        }
    }
    return true;
}

bool HttpConcurrencyLimiter::tryAcquireToken(TokenBucket &bucket, uint32_t qps, uint64_t now_ms)
{
    if (qps == 0)
    {
        return true;
    }
    // 初始化令牌桶,刚启动时允许瞬间突发 qps 个请求。
    if (bucket.lastRefillMs == 0 || bucket.capacity != qps)
    {
        bucket.capacity = qps;
        bucket.tokens = qps;
        bucket.lastRefillMs = now_ms;
    }
    else if (now_ms > bucket.lastRefillMs)
    {
        uint64_t elapsed_ms = now_ms - bucket.lastRefillMs;
        // 补充令牌数 = 经过毫秒数 × QPS / 1000
        bucket.tokens += (double)elapsed_ms * (double)qps / 1000.0;
        if (bucket.tokens > bucket.capacity)
        {
            bucket.tokens = bucket.capacity;
        }
        bucket.lastRefillMs = now_ms;
    }

    if (bucket.tokens < 1.0)
    {
        return false;
    }
    bucket.tokens -= 1.0;
    return true;
}

/**
这个函数不是给“请求结束”用的。
它是给 回滚失败路径 用的。

如果前面都成功了，但是最后一步请求失败了，那么
那前面已经消耗的：
全局 token
service token
应该还回去。
否则就会出现：
请求没有真正发出去但是 QPS 名额被白白消耗了
*/
void HttpConcurrencyLimiter::releaseToken(TokenBucket &bucket)
{
    if (bucket.capacity == 0)
    {
        return;
    }
    bucket.tokens += 1.0;
    if (bucket.tokens > bucket.capacity)
    {
        bucket.tokens = bucket.capacity;
    }
}

} // namespace http
} // namespace sylar
