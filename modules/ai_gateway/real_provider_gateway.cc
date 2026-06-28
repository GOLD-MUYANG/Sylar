#include "real_provider_gateway.h"

#include "ai_gateway_protocol.h"
#include "sylar/util.h"

#include <exception>

namespace sylar
{
namespace ai_gateway
{
namespace
{

void SetError(std::string *error, const std::string &message)
{
    if (error)
    {
        *error = message;
    }
}

// 构造 ProviderAttemptExecutor 自己产生的错误结果。
//
// 注意：
// 这里不是上游 provider 返回的错误，而是网关执行器层面的错误。
// 例如：
// - 没有注入 attempt handler；
// - 逻辑模型没有候选 provider；
// - 没有任何 provider 被实际尝试。
sylar::http::HttpResult::ptr MakeExecutorError(sylar::http::HttpResult::Error error,
                                               const std::string &message)
{
    sylar::http::HttpResult::ptr result =
        std::make_shared<sylar::http::HttpResult>((int)error, nullptr, message);
    result->attempt.detail = message;
    return result;
}

sylar::http::HttpResult::ptr MakeBudgetExhaustedError(RequestExecutionBudget *budget)
{
    if (!budget)
    {
        return MakeExecutorError(sylar::http::HttpResult::Error::CONNECT_FAIL,
                                 "request execution budget missing");
    }
    RequestExecutionBudget::StopReason reason =
        budget ? budget->stopReason() : RequestExecutionBudget::StopReason::NONE;
    sylar::http::HttpResult::Error error =
        reason == RequestExecutionBudget::StopReason::DEADLINE_EXHAUSTED
            ? sylar::http::HttpResult::Error::TIMEOUT
            : sylar::http::HttpResult::Error::CONNECT_FAIL;
    std::string message = reason == RequestExecutionBudget::StopReason::DEADLINE_EXHAUSTED
                              ? "request execution deadline exhausted"
                              : "request execution attempts exhausted";
    return MakeExecutorError(error, message);
}

sylar::http::HttpResult::ptr MakeProtectionError(sylar::http::HttpResult::Error error,
                                                 const std::string &message)
{
    sylar::http::HttpResult::ptr result = MakeExecutorError(error, message);
    result->attempt.phase = sylar::http::HttpAttemptPhase::NOT_SENT;
    result->attempt.may_have_submitted = false;
    return result;
}

sylar::http::HttpResult::ptr MakeHandlerExceptionError(const std::string &message)
{
    sylar::http::HttpResult::ptr result =
        MakeExecutorError(sylar::http::HttpResult::Error::CONNECT_FAIL, message);
    result->attempt.phase = sylar::http::HttpAttemptPhase::PARTIAL_WRITE_OR_UNKNOWN;
    result->attempt.may_have_submitted = true;
    result->attempt.detail = "provider attempt handler exception";
    return result;
}

sylar::http::HttpResult::ptr MakeBreakerNonFailureResult()
{
    return std::make_shared<sylar::http::HttpResult>(
        (int)sylar::http::HttpResult::Error::OK, nullptr, "not a breaker failure");
}

void ReportBreakerResult(const sylar::http::HttpCircuitBreaker::ptr &breaker,
                         const std::string &endpoint_key,
                         const sylar::http::HttpResult::ptr &result)
{
    if (!breaker)
    {
        return;
    }
    ProviderErrorDecision decision = ClassifyProviderAttemptResult(result);
    if (decision.category == ProviderErrorCategory::NONE || decision.breaker_failure)
    {
        breaker->onRequestComplete(endpoint_key, result);
        return;
    }
    breaker->onRequestComplete(endpoint_key, MakeBreakerNonFailureResult());
}

// 从 HttpResult 中提取 Provider 实际返回的 HTTP 状态码。
int GetProviderHttpStatus(const sylar::http::HttpResult::ptr &result)
{
    if (!result)
    {
        return 0;
    }
    if (result->attempt.http_status > 0)
    {
        return result->attempt.http_status;
    }
    if (result->response)
    {
        return (int)result->response->getStatus();
    }
    return 0;
}

bool IsProvider5xx(int status)
{
    return status >= 500 && status <= 599;
}

bool IsProviderRequestOrModel4xx(int status)
{
    return status == (int)sylar::http::HttpStatus::BAD_REQUEST ||
           status == (int)sylar::http::HttpStatus::NOT_FOUND ||
           status == (int)sylar::http::HttpStatus::UNPROCESSABLE_ENTITY;
}

bool IsKnownNotSubmitted(const sylar::http::HttpAttemptOutcome &attempt)
{
    return !attempt.may_have_submitted &&
           (attempt.phase == sylar::http::HttpAttemptPhase::NOT_SENT ||
            attempt.phase == sylar::http::HttpAttemptPhase::SEND_FAILED_BEFORE_BODY);
}

} // namespace

// 对一次 Provider 调用结果进行错误归类，并生成后续处理决策。
//
// 这个函数的目标不是直接构造响应，而是回答三个问题：
// 1. 本次错误属于哪一类？
// 2. 是否应该尝试下一个 ProviderCandidate？
// 3. 是否应该计入熔断失败？
//
// 分类结果会被 BuildProviderGatewayErrorResult 和路由/熔断逻辑复用。
ProviderErrorDecision ClassifyProviderAttemptResult(const sylar::http::HttpResult::ptr &result)
{
    ProviderErrorDecision decision;
    if (!result)
    {
        decision.category = ProviderErrorCategory::RETRYABLE_TRANSPORT;
        decision.try_next_candidate = true;
        decision.breaker_failure = true;
        decision.error_type = "server_error";
        decision.error_code = "UPSTREAM_UNAVAILABLE";
        decision.message = "上游 Provider 调用没有返回结果";
        return decision;
    }

    if (result->result == (int)sylar::http::HttpResult::Error::OK)
    {
        decision.category = ProviderErrorCategory::NONE;
        decision.gateway_status = sylar::http::HttpStatus::OK;
        return decision;
    }

    if (result->result == (int)sylar::http::HttpResult::Error::RATE_LIMITED)
    {
        decision.category = ProviderErrorCategory::RATE_LIMITED;
        decision.try_next_candidate = true;
        decision.gateway_status = sylar::http::HttpStatus::TOO_MANY_REQUESTS;
        decision.error_type = "rate_limit_error";
        decision.error_code = "PROVIDER_ADMISSION_LIMITED";
        decision.message = "上游 Provider 当前不可准入";
        return decision;
    }

    if (result->result == (int)sylar::http::HttpResult::Error::CIRCUIT_OPEN)
    {
        decision.category = ProviderErrorCategory::RETRYABLE_UPSTREAM;
        decision.try_next_candidate = true;
        decision.gateway_status = sylar::http::HttpStatus::BAD_GATEWAY;
        decision.error_type = "server_error";
        decision.error_code = "PROVIDER_CIRCUIT_OPEN";
        decision.message = "上游 Provider 熔断打开";
        return decision;
    }

    if (result->attempt.may_have_submitted &&
        result->attempt.phase != sylar::http::HttpAttemptPhase::RESPONSE_RECEIVED)
    {
        decision.category = ProviderErrorCategory::RESULT_UNKNOWN;
        decision.breaker_failure = true;
        decision.gateway_status = sylar::http::HttpStatus::GATEWAY_TIMEOUT;
        decision.error_type = "timeout_error";
        decision.error_code = "PROVIDER_RESULT_UNKNOWN";
        decision.message = "上游请求可能已提交，结果未知";
        return decision;
    }

    const int status = GetProviderHttpStatus(result);
    if (result->result == (int)sylar::http::HttpResult::Error::HTTP_STATUS_ERROR && status > 0)
    {
        if (status == (int)sylar::http::HttpStatus::UNAUTHORIZED ||
            status == (int)sylar::http::HttpStatus::FORBIDDEN)
        {
            decision.category = ProviderErrorCategory::AUTHORIZATION_FAILED;
            decision.gateway_status = sylar::http::HttpStatus::BAD_GATEWAY;
            decision.error_type = "authentication_error";
            decision.error_code = "PROVIDER_AUTH_FAILED";
            decision.message = "上游 Provider 认证或权限校验失败";
            return decision;
        }

        if (status == (int)sylar::http::HttpStatus::TOO_MANY_REQUESTS)
        {
            decision.category = ProviderErrorCategory::RATE_LIMITED;
            decision.try_next_candidate = true;
            decision.gateway_status = sylar::http::HttpStatus::TOO_MANY_REQUESTS;
            decision.error_type = "rate_limit_error";
            decision.error_code = "PROVIDER_RATE_LIMITED";
            decision.message = "上游 Provider 当前限流";
            return decision;
        }

        if (IsProviderRequestOrModel4xx(status) || (status >= 400 && status <= 499))
        {
            decision.category = ProviderErrorCategory::CALLER_OR_MODEL_REJECTED;
            decision.gateway_status = sylar::http::HttpStatus::BAD_GATEWAY;
            decision.error_type = "invalid_request_error";
            decision.error_code = "PROVIDER_REQUEST_REJECTED";
            decision.message = "上游 Provider 拒绝了请求或模型映射";
            return decision;
        }

        if (IsProvider5xx(status))
        {
            decision.category = ProviderErrorCategory::RETRYABLE_UPSTREAM;
            decision.try_next_candidate = true;
            decision.breaker_failure = true;
            decision.gateway_status = sylar::http::HttpStatus::BAD_GATEWAY;
            decision.error_type = "server_error";
            decision.error_code = "UPSTREAM_UNAVAILABLE";
            decision.message = "上游 Provider 暂时不可用";
            return decision;
        }
    }

    if (result->result == (int)sylar::http::HttpResult::Error::RESPONSE_PARSE_FAIL)
    {
        decision.category = ProviderErrorCategory::RESPONSE_CONTRACT_INVALID;
        decision.try_next_candidate = true;
        decision.breaker_failure = true;
        decision.gateway_status = sylar::http::HttpStatus::BAD_GATEWAY;
        decision.error_type = "server_error";
        decision.error_code = "PROVIDER_RESPONSE_INVALID";
        decision.message = "上游 Provider 返回了无效响应";
        return decision;
    }

    if (IsKnownNotSubmitted(result->attempt))
    {
        decision.category = ProviderErrorCategory::RETRYABLE_TRANSPORT;
        decision.try_next_candidate = true;
        decision.breaker_failure = true;
        decision.gateway_status = sylar::http::HttpStatus::BAD_GATEWAY;
        decision.error_type = "server_error";
        decision.error_code = "UPSTREAM_UNAVAILABLE";
        decision.message = "上游 Provider 当前不可用";
        return decision;
    }

    decision.category = ProviderErrorCategory::UNKNOWN;
    decision.breaker_failure = true;
    decision.gateway_status = sylar::http::HttpStatus::BAD_GATEWAY;
    decision.error_type = "server_error";
    decision.error_code = "UPSTREAM_UNAVAILABLE";
    decision.message = "上游 Provider 调用失败";
    return decision;
}
// 把 Provider 调用失败结果转换成网关对外返回的 HttpResult。
sylar::http::HttpResult::ptr
BuildProviderGatewayErrorResult(const sylar::http::HttpResult::ptr &provider_result)
{
    ProviderErrorDecision decision = ClassifyProviderAttemptResult(provider_result);
    if (decision.category == ProviderErrorCategory::NONE)
    {
        return provider_result;
    }

    sylar::http::HttpResponse::ptr response(new sylar::http::HttpResponse);
    response->setStatus(decision.gateway_status);
    response->setHeader("Content-Type", "application/json");
    response->setBody(
        BuildErrorResponse(decision.message, decision.error_type, decision.error_code));

    sylar::http::HttpResult::ptr result = std::make_shared<sylar::http::HttpResult>(
        provider_result ? provider_result->result
                        : (int)sylar::http::HttpResult::Error::CONNECT_FAIL,
        response, decision.message);
    if (provider_result)
    {
        result->attempt = provider_result->attempt;
        if (result->attempt.http_status == 0 && provider_result->response)
        {
            result->attempt.http_status = (int)provider_result->response->getStatus();
        }
    }
    result->attempt.detail = decision.error_code;
    return result;
}

RequestExecutionBudget::RequestExecutionBudget(uint64_t deadline_ms,
                                               uint32_t max_total_attempts,
                                               const std::string &request_id)
    : m_startMs(sylar::GetCurrentMS()), m_deadlineMs(deadline_ms),
      m_maxTotalAttempts(max_total_attempts), m_requestId(request_id)
{
}
// 计算当前距离业务请求总 deadline 还剩多少毫秒。
//
// 返回值：
//   (uint64_t)-1：没有显式 deadline 限制；
//   0：总时间预算已经耗尽；
//   其他正数：还剩余的毫秒数。
//
uint64_t RequestExecutionBudget::remainingMs() const
{
    if (m_deadlineMs == (uint64_t)-1)
    {
        return (uint64_t)-1;
    }

    uint64_t now = sylar::GetCurrentMS();
    uint64_t elapsed = now >= m_startMs ? now - m_startMs : 0;
    if (elapsed >= m_deadlineMs)
    {
        return 0;
    }
    return m_deadlineMs - elapsed;
}

// 尝试消耗一次下游 HTTP 尝试额度。
//
// 调用方应该在真正发起一次 Provider HTTP 请求前调用它。
// 返回 true 表示本次尝试额度消耗成功，可以继续执行 HTTP I/O。
// 返回 false 表示不能继续尝试，具体原因通过 stopReason() 获取。

// 检查顺序：
//   1. 先检查 deadline；
//   2. 再检查尝试次数；
//   3. 两者都未耗尽，才消耗一次尝试额度。
bool RequestExecutionBudget::tryConsumeAttempt()
{
    if (remainingMs() == 0)
    {
        m_stopReason = StopReason::DEADLINE_EXHAUSTED;
        return false;
    }
    if (m_maxTotalAttempts > 0 && m_consumedAttempts >= m_maxTotalAttempts)
    {
        m_stopReason = StopReason::ATTEMPTS_EXHAUSTED;
        return false;
    }
    // 通过预算检查，正式消耗一次尝试额度。
    //
    // 注意：
    //   这里递增并不代表 HTTP 请求一定成功完成；
    //   只代表调用方已经获得了一次“可以发起下游尝试”的许可。
    ++m_consumedAttempts;
    m_stopReason = StopReason::NONE;
    return true;
}

const char *ProviderAdapterTypeToString(ProviderAdapterType type)
{
    switch (type)
    {
    case ProviderAdapterType::OPENAI_COMPATIBLE:
        return "openai_compatible";
    default:
        return "unknown";
    }
}

bool LogicalModelRouter::addCandidate(const ProviderCandidate &candidate, std::string *error)
{
    if (error)
    {
        error->clear();
    }

    if (!candidate.enabled)
    {
        return true;
    }

    if (candidate.name.empty() || candidate.logical_model.empty() || candidate.base_url.empty() ||
        candidate.chat_path.empty() || candidate.upstream_model.empty() ||
        candidate.api_key_env.empty())
    {
        SetError(error, "provider candidate 字段不能为空");
        return false;
    }
    // 检查同一个 logical_model 下的 compatibility_key 是否一致。
    //
    // 目的：
    // 防止把能力差异太大的 provider 放进同一个模型池。
    auto compatibility = m_compatibilityKeys.find(candidate.logical_model);
    if (compatibility == m_compatibilityKeys.end())
    {
        // 第一次看到这个 logical_model，记录它的兼容性分组。
        m_compatibilityKeys[candidate.logical_model] = candidate.compatibility_key;
    }
    else if (compatibility->second != candidate.compatibility_key)
    {
        SetError(error, "逻辑模型池包含不兼容的 provider candidate: " + candidate.logical_model);
        return false;
    }
    // 加入 logical_model 对应的候选列表。
    // 当前顺序就是后续尝试顺序：
    // 先加入的 candidate 会先被尝试。
    m_candidates[candidate.logical_model].push_back(candidate);
    return true;
}

std::vector<ProviderCandidate>
LogicalModelRouter::findCandidates(const std::string &logical_model) const
{
    auto it = m_candidates.find(logical_model);
    if (it == m_candidates.end())
    {
        return {};
    }
    return it->second;
}

bool LogicalModelRouter::hasModel(const std::string &logical_model) const
{
    auto it = m_candidates.find(logical_model);
    return it != m_candidates.end() && !it->second.empty();
}

ProviderAttemptExecutor::ProviderAttemptExecutor(const LogicalModelRouter &router,
                                                 AttemptHandler handler)
    : m_router(router),
      m_handler([handler](const ProviderCandidate &candidate,
                          const GatewayChatRequest &request,
                          RequestExecutionBudget *)
                { return handler ? handler(candidate, request) : sylar::http::HttpResult::ptr(); })
{
}

ProviderAttemptExecutor::ProviderAttemptExecutor(const LogicalModelRouter &router,
                                                 BudgetedAttemptHandler handler)
    : m_router(router), m_handler(handler)
{
}

ProviderAttemptExecutor::ProviderAttemptExecutor(const LogicalModelRouter &router,
                                                 BudgetedAttemptHandler handler,
                                                 const ProviderExecutionControls &controls)
    : m_router(router), m_handler(handler), m_controls(controls)
{
}

sylar::http::HttpResult::ptr
ProviderAttemptExecutor::execute(const GatewayChatRequest &request) const
{
    return execute(request, nullptr);
}

sylar::http::HttpResult::ptr ProviderAttemptExecutor::execute(const GatewayChatRequest &request,
                                                              RequestExecutionBudget *budget) const
{
    if (!m_handler)
    {
        return MakeExecutorError(sylar::http::HttpResult::Error::CONNECT_FAIL,
                                 "provider attempt handler missing");
    }
    // 根据调用方传入的逻辑模型名查找候选 provider。
    auto candidates = m_router.findCandidates(request.model);
    if (candidates.empty())
    {
        return MakeExecutorError(sylar::http::HttpResult::Error::CONNECT_FAIL,
                                 "logical model has no provider candidate");
    }

    sylar::http::HttpResult::ptr last_result;

    // 按候选 provider 顺序逐个尝试。
    // 当前没有做负载均衡、随机和权重。
    // candidate 顺序就是实际尝试顺序。
    for (const auto &candidate : candidates)
    {
        if (budget && !budget->tryConsumeAttempt())
        {
            return MakeBudgetExhaustedError(budget);
        }

        const std::string endpoint_key = candidate.name;
        sylar::http::HttpConcurrencyLimitGuard::ptr limit_guard =
            m_controls.limiter ? m_controls.limiter->tryAcquire(endpoint_key) : nullptr;
        if (m_controls.limiter && !limit_guard)
        {
            last_result = MakeProtectionError(sylar::http::HttpResult::Error::RATE_LIMITED,
                                              "provider concurrency limited");
            if (!shouldTryNextCandidate(last_result))
            {
                return last_result;
            }
            continue;
        }

        sylar::http::HttpCircuitBreakerGuard::ptr circuit_guard =
            m_controls.circuit_breaker ? m_controls.circuit_breaker->tryAcquire(endpoint_key)
                                       : nullptr;
        if (m_controls.circuit_breaker && !circuit_guard)
        {
            last_result = MakeProtectionError(sylar::http::HttpResult::Error::CIRCUIT_OPEN,
                                              "provider circuit open");
            if (!shouldTryNextCandidate(last_result))
            {
                return last_result;
            }
            continue;
        }

        try
        {
            last_result = m_handler(candidate, request, budget);
        }
        catch (const std::exception &ex)
        {
            last_result =
                MakeHandlerExceptionError(std::string("provider attempt handler exception: ") +
                                          ex.what());
        }
        catch (...)
        {
            last_result = MakeHandlerExceptionError("provider attempt handler exception");
        }

        if (m_controls.circuit_breaker && circuit_guard)
        {
            ReportBreakerResult(m_controls.circuit_breaker, endpoint_key, last_result);
        }
        if (!shouldTryNextCandidate(last_result))
        {
            return last_result;
        }
    }

    return last_result ? last_result
                       : MakeExecutorError(sylar::http::HttpResult::Error::CONNECT_FAIL,
                                           "no provider attempt executed");
}

bool ProviderAttemptExecutor::shouldTryNextCandidate(const sylar::http::HttpResult::ptr &result)
{
    return ClassifyProviderAttemptResult(result).try_next_candidate;
}

} // namespace ai_gateway
} // namespace sylar
