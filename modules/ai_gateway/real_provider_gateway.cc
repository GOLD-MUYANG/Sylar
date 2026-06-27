#include "real_provider_gateway.h"

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

} // namespace

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
    : m_router(router), m_handler(handler)
{
}

sylar::http::HttpResult::ptr
ProviderAttemptExecutor::execute(const GatewayChatRequest &request) const
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
    // 当前没有做负载均衡、随机、权重、熔断过滤。
    // candidate 顺序就是实际尝试顺序。
    for (const auto &candidate : candidates)
    {
        last_result = m_handler(candidate, request);
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
    if (!result)
    {
        return true;
    }

    if (result->result == (int)sylar::http::HttpResult::Error::OK)
    {
        return false;
    }

    if (result->attempt.may_have_submitted &&
        result->attempt.phase != sylar::http::HttpAttemptPhase::RESPONSE_RECEIVED)
    {
        return false;
    }

    if (result->result == (int)sylar::http::HttpResult::Error::HTTP_STATUS_ERROR &&
        result->response)
    {
        auto status = result->response->getStatus();
        return status == sylar::http::HttpStatus::INTERNAL_SERVER_ERROR ||
               status == sylar::http::HttpStatus::BAD_GATEWAY ||
               status == sylar::http::HttpStatus::SERVICE_UNAVAILABLE ||
               status == sylar::http::HttpStatus::GATEWAY_TIMEOUT;
    }

    return result->attempt.phase == sylar::http::HttpAttemptPhase::NOT_SENT ||
           result->attempt.phase == sylar::http::HttpAttemptPhase::SEND_FAILED_BEFORE_BODY;
}

} // namespace ai_gateway
} // namespace sylar
