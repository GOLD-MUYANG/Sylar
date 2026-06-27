#ifndef __SYLAR_AI_GATEWAY_REAL_PROVIDER_GATEWAY_H__
#define __SYLAR_AI_GATEWAY_REAL_PROVIDER_GATEWAY_H__

#include "ai_gateway_protocol.h"
#include "sylar/http/http_connection.h"

#include <stdint.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sylar
{
namespace ai_gateway
{

enum class ProviderAdapterType
{
    OPENAI_COMPATIBLE = 0,
};

const char *ProviderAdapterTypeToString(ProviderAdapterType type);

// 一个可被路由选择的真实 Provider 候选项。
// 它表示：某个逻辑模型 logical_model 可以落到某个真实上游 provider。
struct ProviderCandidate
{
    std::string name;
    ProviderAdapterType adapter_type = ProviderAdapterType::OPENAI_COMPATIBLE;
    std::string logical_model;
    std::string base_url;
    std::string chat_path;
    std::string upstream_model;
    std::string api_key_env;
    std::string compatibility_key;
    bool enabled = false;
};

// 单次真实 Provider 业务请求共享的执行预算。
//
// 职责边界：
// 1. 统一记录本次业务请求还能发起多少次下游 HTTP 尝试；
// 2. 统一计算剩余 deadline，供 connect/TLS/send/recv 截断超时；
// 3. 不负责候选选择、HTTP I/O、错误分类或限流熔断状态。
class RequestExecutionBudget
{
public:
    // 停止继续尝试的原因。
    //
    // NONE:
    //   当前仍有执行预算，可以继续尝试。
    //
    // DEADLINE_EXHAUSTED:
    //   本次业务请求的总时间预算已经耗尽。
    //   后续不应该再发起新的下游 HTTP 请求。
    //
    // ATTEMPTS_EXHAUSTED:
    //   本次业务请求允许的最大尝试次数已经用完。
    //   即使还有剩余时间，也不应该继续尝试
    enum class StopReason
    {
        NONE = 0,
        DEADLINE_EXHAUSTED = 1,
        ATTEMPTS_EXHAUSTED = 2,
    };

    RequestExecutionBudget(uint64_t deadline_ms = (uint64_t)-1,
                           uint32_t max_total_attempts = 0,
                           const std::string &request_id = "");
    // 尝试消耗一次下游 HTTP 尝试额度。
    bool tryConsumeAttempt();
    // 返回当前业务请求剩余的时间预算，单位毫秒。
    uint64_t remainingMs() const;

    StopReason stopReason() const
    {
        return m_stopReason;
    }

    // 已经消耗的下游 HTTP 尝试次数。
    //
    // 该值只表示“准备发起过多少次下游尝试”，
    // 不代表这些尝试一定成功完成了完整 HTTP 交互。
    uint32_t consumedAttempts() const
    {
        return m_consumedAttempts;
    }

    // 本次业务请求允许的最大下游 HTTP 尝试次数。
    //
    // 具体是否允许 0 表示“不限制”，取决于构造函数和 tryConsumeAttempt() 的实现约定。
    uint32_t maxTotalAttempts() const
    {
        return m_maxTotalAttempts;
    }

    // 本次业务请求的追踪 ID。
    //
    // 主要用于日志、错误响应、链路排查和多次 Provider 尝试的关联。
    const std::string &requestId() const
    {
        return m_requestId;
    }

private:
    // 预算对象创建时的时间点，单位毫秒。
    uint64_t m_startMs = 0;
    // 表示从 Budget 创建开始计算的总超时时长，
    uint64_t m_deadlineMs = (uint64_t)-1;
    // 本次业务请求最多允许的下游 HTTP 尝试次数。== 0 表示不限制总尝试次数
    uint32_t m_maxTotalAttempts = 0;
    // 当前已经消耗的下游 HTTP 尝试次数。
    uint32_t m_consumedAttempts = 0;
    // 本次业务请求的追踪 ID。
    std::string m_requestId;
    StopReason m_stopReason = StopReason::NONE;
};

// 逻辑模型路由器。
//
// 职责：
// 1. 维护 logical_model -> provider candidates 的映射；
// 2. 校验同一个 logical_model 下的候选 provider 是否兼容；
// 3. 根据请求中的 logical_model 找到可尝试的 provider 列表。
//
class LogicalModelRouter
{
public:
    bool addCandidate(const ProviderCandidate &candidate, std::string *error = nullptr);
    std::vector<ProviderCandidate> findCandidates(const std::string &logical_model) const;
    bool hasModel(const std::string &logical_model) const;

private:
    std::map<std::string, std::vector<ProviderCandidate>> m_candidates;
    std::map<std::string, std::string> m_compatibilityKeys;
};

// Provider 尝试执行器。
//
// 它根据 LogicalModelRouter 找到候选 provider，
// 然后调用外部注入的 AttemptHandler 执行单次真实请求。
class ProviderAttemptExecutor
{
public:
    typedef std::function<sylar::http::HttpResult::ptr(const ProviderCandidate &,
                                                       const GatewayChatRequest &)>
        AttemptHandler;
    typedef std::function<sylar::http::HttpResult::ptr(
        const ProviderCandidate &, const GatewayChatRequest &, RequestExecutionBudget *)>
        BudgetedAttemptHandler;

    ProviderAttemptExecutor(const LogicalModelRouter &router, AttemptHandler handler);
    ProviderAttemptExecutor(const LogicalModelRouter &router, BudgetedAttemptHandler handler);

    sylar::http::HttpResult::ptr execute(const GatewayChatRequest &request) const;
    sylar::http::HttpResult::ptr execute(const GatewayChatRequest &request,
                                         RequestExecutionBudget *budget) const;

private:
    // 判断当前结果是否允许尝试下一个 provider。
    static bool shouldTryNextCandidate(const sylar::http::HttpResult::ptr &result);

private:
    LogicalModelRouter m_router;
    BudgetedAttemptHandler m_handler;
};

} // namespace ai_gateway
} // namespace sylar

#endif
