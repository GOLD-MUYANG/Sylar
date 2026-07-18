#include "real_provider_runtime.h"

#include "sylar/uri.h"
#include "sylar/util.h"

#include <cstdlib>

namespace sylar
{
namespace ai_gateway
{
namespace
{

bool SetError(std::string *error, const std::string &message)
{
    if (error)
    {
        *error = message;
    }
    return false;
}

uint32_t ReadUint32(const YAML::Node &node, const char *key, uint32_t default_value)
{
    return node && node[key] ? node[key].as<uint32_t>(default_value) : default_value;
}

uint64_t ReadUint64(const YAML::Node &node, const char *key, uint64_t default_value)
{
    return node && node[key] ? node[key].as<uint64_t>(default_value) : default_value;
}

bool ReadBool(const YAML::Node &node, const char *key, bool default_value)
{
    return node && node[key] ? node[key].as<bool>(default_value) : default_value;
}

std::string
ReadString(const YAML::Node &node, const char *key, const std::string &default_value = "")
{
    return node && node[key] ? node[key].as<std::string>(default_value) : default_value;
}

bool HasEnvValue(const std::string &name)
{
    const char *value = std::getenv(name.c_str());
    return value && *value;
}

const char *CategoryToString(ProviderErrorCategory category)
{
    switch (category)
    {
    case ProviderErrorCategory::NONE:
        return "none";
    case ProviderErrorCategory::CALLER_OR_MODEL_REJECTED:
        return "caller_or_model_rejected";
    case ProviderErrorCategory::AUTHORIZATION_FAILED:
        return "authorization_failed";
    case ProviderErrorCategory::RATE_LIMITED:
        return "rate_limited";
    case ProviderErrorCategory::RETRYABLE_UPSTREAM:
        return "retryable_upstream";
    case ProviderErrorCategory::RETRYABLE_TRANSPORT:
        return "retryable_transport";
    case ProviderErrorCategory::RESULT_UNKNOWN:
        return "result_unknown";
    case ProviderErrorCategory::RESPONSE_CONTRACT_INVALID:
        return "response_contract_invalid";
    case ProviderErrorCategory::CONFIGURATION_ERROR:
        return "configuration_error";
    default:
        return "unknown";
    }
}

std::string RedactedEndpoint(const ProviderCandidate &candidate)
{
    sylar::Uri::ptr uri = sylar::Uri::Create(candidate.base_url);
    if (!uri)
    {
        return "";
    }
    std::string endpoint = uri->getScheme() + "://" + uri->getHost();
    const uint32_t port = uri->getPort();
    if (port > 0 && !((uri->getScheme() == "https" && port == 443) ||
                      (uri->getScheme() == "http" && port == 80)))
    {
        endpoint += ":" + std::to_string(port);
    }
    return endpoint;
}

Json::Value MakeTraceEvent(const std::string &stage)
{
    Json::Value event(Json::objectValue);
    event["stage"] = stage;
    event["elapsed_ms"] = Json::UInt64(0);
    return event;
}

void AppendTrace(Json::Value *trace, const Json::Value &event)
{
    if (!trace)
    {
        return;
    }
    if (!trace->isObject())
    {
        *trace = Json::Value(Json::objectValue);
        (*trace)["object"] = "ai_gateway.real_provider.trace";
        (*trace)["events"] = Json::Value(Json::arrayValue);
    }
    (*trace)["events"].append(event);
}

Json::Value BuildCandidatesEvent(const std::vector<ProviderCandidate> &candidates)
{
    Json::Value event = MakeTraceEvent("candidates_selected");
    Json::Value items(Json::arrayValue);
    for (const auto &candidate : candidates)
    {
        Json::Value item(Json::objectValue);
        item["provider"] = candidate.name;
        item["adapter_type"] = ProviderAdapterTypeToString(candidate.adapter_type);
        item["endpoint_key"] = candidate.name;
        item["logical_model"] = candidate.logical_model;
        items.append(item);
    }
    event["candidates"] = items;
    return event;
}

Json::Value BuildAttemptEvent(const ProviderCandidate &candidate,
                              const sylar::http::HttpResult::ptr &result,
                              RequestExecutionBudget *budget,
                              uint64_t elapsed_ms)
{
    ProviderErrorDecision decision = ClassifyProviderAttemptResult(result);
    Json::Value event = MakeTraceEvent("attempt_result");
    event["provider"] = candidate.name;
    event["adapter_type"] = ProviderAdapterTypeToString(candidate.adapter_type);
    event["endpoint_key"] = candidate.name;
    event["attempt_phase"] =
        result ? sylar::http::HttpAttemptPhaseToString(result->attempt.phase) : "not_sent";
    event["may_have_submitted"] = result ? result->attempt.may_have_submitted : false;
    event["http_status"] = result ? result->attempt.http_status : 0;
    event["provider_error_category"] = CategoryToString(decision.category);
    event["gateway_decision"] = decision.error_code;
    event["try_next_candidate"] = decision.try_next_candidate;
    event["elapsed_ms"] = Json::UInt64(elapsed_ms);
    event["consumed_attempts"] = budget ? budget->consumedAttempts() : 0;
    event["remaining_deadline_ms"] = budget ? Json::UInt64(budget->remainingMs()) : Json::UInt64(0);
    return event;
}

} // namespace

bool LoadRealProviderRuntimeConfig(const YAML::Node &root,
                                   RealProviderRuntimeConfig *config,
                                   std::string *error)
{
    if (!config)
    {
        return SetError(error, "real provider config 接收对象不能为空");
    }

    RealProviderRuntimeConfig parsed;
    YAML::Node node = root["real_providers"];
    if (!node)
    {
        *config = parsed;
        if (error)
        {
            error->clear();
        }
        return true;
    }

    parsed.enabled = ReadBool(node, "enabled", parsed.enabled);
    parsed.server_name = ReadString(node, "server_name", parsed.server_name);
    parsed.route_path = ReadString(node, "route_path", parsed.route_path);
    parsed.demo_enabled = ReadBool(node, "demo_enabled", parsed.demo_enabled);
    parsed.load_balance = ReadString(node, "load_balance", parsed.load_balance);
    parsed.request_deadline_ms =
        ReadUint64(node, "request_deadline_ms", parsed.request_deadline_ms);
    parsed.max_total_attempts = ReadUint32(node, "max_total_attempts", parsed.max_total_attempts);
    parsed.request_options =
        sylar::http::HttpRequestOptions::FromTimeout(parsed.request_deadline_ms);

    YAML::Node limiter = node["limiter"];
    parsed.limiter_options.max_global_concurrency = ReadUint32(
        limiter, "max_global_concurrency", parsed.limiter_options.max_global_concurrency);
    parsed.limiter_options.max_service_concurrency = ReadUint32(
        limiter, "max_service_concurrency", parsed.limiter_options.max_service_concurrency);
    parsed.limiter_options.max_endpoint_concurrency = ReadUint32(
        limiter, "max_endpoint_concurrency", parsed.limiter_options.max_endpoint_concurrency);
    parsed.limiter_options.max_global_qps =
        ReadUint32(limiter, "max_global_qps", parsed.limiter_options.max_global_qps);
    parsed.limiter_options.max_service_qps =
        ReadUint32(limiter, "max_service_qps", parsed.limiter_options.max_service_qps);
    parsed.limiter_options.max_endpoint_qps =
        ReadUint32(limiter, "max_endpoint_qps", parsed.limiter_options.max_endpoint_qps);

    YAML::Node breaker = node["circuit_breaker"];
    parsed.circuit_breaker_options.enabled =
        ReadBool(breaker, "enabled", parsed.circuit_breaker_options.enabled);
    parsed.circuit_breaker_options.consecutive_failure_threshold =
        ReadUint32(breaker, "consecutive_failure_threshold",
                   parsed.circuit_breaker_options.consecutive_failure_threshold);
    parsed.circuit_breaker_options.open_timeout_ms =
        ReadUint64(breaker, "open_timeout_ms", parsed.circuit_breaker_options.open_timeout_ms);
    parsed.circuit_breaker_options.half_open_max_requests = ReadUint32(
        breaker, "half_open_max_requests", parsed.circuit_breaker_options.half_open_max_requests);

    YAML::Node providers = node["providers"];
    if (providers && providers.IsSequence())
    {
        for (const auto &provider : providers)
        {
            ProviderCandidate candidate;
            candidate.name = ReadString(provider, "name");
            candidate.enabled = ReadBool(provider, "enabled", false);
            candidate.logical_model = ReadString(provider, "logical_model");
            candidate.compatibility_key = ReadString(provider, "compatibility_key");
            candidate.base_url = ReadString(provider, "base_url");
            candidate.chat_path = ReadString(provider, "chat_path");
            candidate.upstream_model = ReadString(provider, "upstream_model");
            candidate.api_key_env = ReadString(provider, "api_key_env");
            candidate.tls_server_name = ReadString(provider, "tls_server_name");
            candidate.tls_ca_file = ReadString(provider, "tls_ca_file");
            candidate.tls_ca_path = ReadString(provider, "tls_ca_path");
            candidate.tls_verify_peer = ReadBool(provider, "tls_verify_peer", true);
            candidate.weight = ReadUint32(provider, "weight", candidate.weight);

            const std::string type = ReadString(provider, "type", "openai_compatible");
            if (type != "openai_compatible")
            {
                return SetError(error, "真实 Provider 类型不支持 provider=" + candidate.name);
            }
            if (!candidate.enabled)
            {
                continue;
            }
            if (candidate.name.empty() || candidate.logical_model.empty() ||
                candidate.compatibility_key.empty() || candidate.base_url.empty() ||
                candidate.chat_path.empty() || candidate.upstream_model.empty() ||
                candidate.api_key_env.empty())
            {
                return SetError(error, "真实 Provider 配置字段不能为空 provider=" + candidate.name);
            }
            if (parsed.enabled && !HasEnvValue(candidate.api_key_env))
            {
                return SetError(error,
                                "真实 Provider API Key 环境变量不存在 provider=" + candidate.name);
            }
            parsed.providers.push_back(candidate);
        }
    }

    if (parsed.enabled)
    {
        if (parsed.server_name.empty() || parsed.route_path.empty())
        {
            return SetError(error, "real_providers 启用时 server_name/route_path 不能为空");
        }
        if (parsed.providers.empty())
        {
            return SetError(error, "real_providers 启用时至少需要一个 enabled provider");
        }
        sylar::load_balance::LoadBalanceStrategy strategy;
        if (!sylar::load_balance::ParseLoadBalanceStrategy(parsed.load_balance, &strategy))
        {
            return SetError(error,
                            "real_providers load_balance 不支持: " + parsed.load_balance);
        }

        LogicalModelRouter router;
        for (const auto &candidate : parsed.providers)
        {
            std::string router_error;
            if (!router.addCandidate(candidate, &router_error))
            {
                return SetError(error, router_error);
            }
        }
    }

    *config = parsed;
    if (error)
    {
        error->clear();
    }
    return true;
}

std::string DumpRealProviderTrace(const Json::Value &trace)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, trace);
}

RealProviderRuntime::ptr RealProviderRuntime::Create(const RealProviderRuntimeConfig &config,
                                                     const ProviderHttpPost &post)
{
    RealProviderRuntime::ptr runtime(new RealProviderRuntime(config));
    std::string error;
    if (!runtime->init(post, &error))
    {
        return nullptr;
    }
    return runtime;
}

RealProviderRuntime::RealProviderRuntime(const RealProviderRuntimeConfig &config) : m_config(config)
{
}

bool RealProviderRuntime::init(const ProviderHttpPost &post, std::string *error)
{
    for (const auto &candidate : m_config.providers)
    {
        std::string router_error;
        if (!m_router.addCandidate(candidate, &router_error))
        {
            return SetError(error, router_error);
        }

        ProviderStats stats;
        stats.candidate = candidate;
        m_stats.push_back(stats);
    }

    m_controls.limiter = sylar::http::HttpConcurrencyLimiter::Create(m_config.limiter_options);
    m_controls.circuit_breaker =
        sylar::http::HttpCircuitBreaker::Create(m_config.circuit_breaker_options);

    m_handler = CreateOpenAICompatibleAttemptHandler(post, m_config.request_options);
    sylar::load_balance::LoadBalanceStrategy strategy;
    if (!sylar::load_balance::ParseLoadBalanceStrategy(m_config.load_balance, &strategy))
    {
        return SetError(error, "real provider selector 策略无效: " + m_config.load_balance);
    }

    m_selector = CreateProviderCandidateSelector(
        strategy, [this](const ProviderCandidate &candidate) {
            return getProviderInFlight(candidate.name);
        },
        [this](const ProviderCandidate &candidate) {
            recordAttemptBegin(candidate);
        });
    if (!m_selector)
    {
        return SetError(error, "real provider selector 创建失败");
    }
    if (error)
    {
        error->clear();
    }
    return true;
}

sylar::http::HttpResult::ptr RealProviderRuntime::execute(const GatewayChatRequest &request,
                                                          Json::Value *trace)
{
    Json::Value received = MakeTraceEvent("request_received");
    received["model"] = request.model;
    AppendTrace(trace, received);

    Json::Value parsed = MakeTraceEvent("model_parsed");
    parsed["model"] = request.model;
    AppendTrace(trace, parsed);

    std::vector<ProviderCandidate> candidates = m_router.findCandidates(request.model);

    uint64_t sequence = 0;
    {
        MutexType::Lock lock(m_mutex);
        sequence = ++m_requestSequence;
    }
    AppendTrace(trace, BuildCandidatesEvent(candidates));

    RequestExecutionBudget budget(m_config.request_deadline_ms, m_config.max_total_attempts,
                                  "real-provider-" + std::to_string(sequence));

    ProviderAttemptExecutor executor(
        m_router,
        [this, trace, &budget](const ProviderCandidate &candidate,
                               const GatewayChatRequest &inner_request,
                               RequestExecutionBudget *inner_budget)
        {
            const uint64_t started = sylar::GetCurrentMS();
            sylar::http::HttpResult::ptr result;
            try
            {
                result = m_handler(candidate, inner_request, inner_budget);
            }
            catch (...)
            {
                const uint64_t elapsed = sylar::GetCurrentMS() - started;
                recordAttemptEnd(candidate, nullptr);
                AppendTrace(trace, BuildAttemptEvent(candidate, nullptr, &budget, elapsed));
                throw;
            }
            const uint64_t elapsed = sylar::GetCurrentMS() - started;
            recordAttemptEnd(candidate, result);
            AppendTrace(trace, BuildAttemptEvent(candidate, result, &budget, elapsed));
            return result;
        },
        m_controls, m_selector);

    sylar::http::HttpResult::ptr result = executor.execute(request, &budget);
    Json::Value returned = MakeTraceEvent("response_returned");
    returned["result"] = result ? result->result : -1;
    returned["consumed_attempts"] = budget.consumedAttempts();
    returned["remaining_deadline_ms"] = Json::UInt64(budget.remainingMs());
    AppendTrace(trace, returned);
    return result;
}

Json::Value RealProviderRuntime::buildStatusJson() const
{
    Json::Value root(Json::objectValue);
    root["object"] = "ai_gateway.real_provider.status";
    root["load_balance"] = m_config.load_balance;
    Json::Value providers(Json::arrayValue);

    MutexType::Lock lock(m_mutex);
    for (const auto &stats : m_stats)
    {
        providers.append(buildCandidateStatus(stats));
    }
    root["providers"] = providers;
    return root;
}

void RealProviderRuntime::recordAttemptBegin(const ProviderCandidate &candidate)
{
    MutexType::Lock lock(m_mutex);
    for (auto &stats : m_stats)
    {
        if (stats.candidate.name == candidate.name)
        {
            ++stats.in_flight;
            stats.last_attempt_ms = sylar::GetCurrentMS();
            return;
        }
    }
}

void RealProviderRuntime::recordAttemptEnd(const ProviderCandidate &candidate,
                                           const sylar::http::HttpResult::ptr &result)
{
    ProviderErrorDecision decision = ClassifyProviderAttemptResult(result);
    MutexType::Lock lock(m_mutex);
    for (auto &stats : m_stats)
    {
        if (stats.candidate.name != candidate.name)
        {
            continue;
        }
        if (stats.in_flight > 0)
        {
            --stats.in_flight;
        }
        if (decision.category == ProviderErrorCategory::NONE)
        {
            ++stats.success_count;
            stats.last_failure_reason.clear();
        }
        else
        {
            if (decision.category == ProviderErrorCategory::RATE_LIMITED)
            {
                ++stats.rate_limited_count;
            }
            ++stats.failure_count;
            stats.last_failure_reason = decision.error_code;
        }
        return;
    }
}

uint32_t RealProviderRuntime::getProviderInFlight(const std::string &provider_name) const
{
    MutexType::Lock lock(m_mutex);
    for (const auto &stats : m_stats)
    {
        if (stats.candidate.name == provider_name)
        {
            return stats.in_flight;
        }
    }
    return 0;
}

Json::Value RealProviderRuntime::buildCandidateStatus(const ProviderStats &stats) const
{
    Json::Value item(Json::objectValue);
    const ProviderCandidate &candidate = stats.candidate;
    item["name"] = candidate.name;
    item["logical_model"] = candidate.logical_model;
    item["enabled"] = candidate.enabled;
    item["adapter_type"] = ProviderAdapterTypeToString(candidate.adapter_type);
    item["upstream_model"] = candidate.upstream_model;
    item["endpoint"] = RedactedEndpoint(candidate);
    item["health"] = "UP";
    item["circuit_breaker"] = m_controls.circuit_breaker
                                  ? sylar::http::HttpCircuitBreakerStateToString(
                                        m_controls.circuit_breaker->getState(candidate.name))
                                  : "CLOSED";
    item["in_flight"] = stats.in_flight;
    item["success_count"] = Json::UInt64(stats.success_count);
    item["failure_count"] = Json::UInt64(stats.failure_count);
    item["rate_limited_count"] = Json::UInt64(stats.rate_limited_count);
    item["last_failure_reason"] = stats.last_failure_reason;
    item["last_attempt_time"] = Json::UInt64(stats.last_attempt_ms);
    item["last_smoke_result"] = stats.last_smoke_result;
    return item;
}

} // namespace ai_gateway
} // namespace sylar
