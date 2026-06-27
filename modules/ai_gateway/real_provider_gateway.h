#ifndef __SYLAR_AI_GATEWAY_REAL_PROVIDER_GATEWAY_H__
#define __SYLAR_AI_GATEWAY_REAL_PROVIDER_GATEWAY_H__

#include "ai_gateway_protocol.h"
#include "sylar/http/http_connection.h"

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

    ProviderAttemptExecutor(const LogicalModelRouter &router, AttemptHandler handler);

    sylar::http::HttpResult::ptr execute(const GatewayChatRequest &request) const;

private:
    // 判断当前结果是否允许尝试下一个 provider。
    static bool shouldTryNextCandidate(const sylar::http::HttpResult::ptr &result);

private:
    LogicalModelRouter m_router;
    AttemptHandler m_handler;
};

} // namespace ai_gateway
} // namespace sylar

#endif
