#include "real_provider_smoke.h"

#include <cstdlib>
#include <ostream>
#include <sstream>

namespace sylar
{
namespace ai_gateway
{
namespace
{

const char *GetEnvValue(const char *name)
{
    const char *value = std::getenv(name);
    return value && *value ? value : nullptr;
}

std::string GetEnvString(const char *name, const std::string &default_value)
{
    const char *value = GetEnvValue(name);
    return value ? std::string(value) : default_value;
}

bool IsSmokeEnabled()
{
    const char *value = GetEnvValue("SYLAR_AI_GATEWAY_REAL_SMOKE");
    if (!value)
    {
        return false;
    }
    std::string flag(value);
    return flag == "1" || flag == "true" || flag == "TRUE" || flag == "on" || flag == "ON";
}

uint64_t GetEnvUint64(const char *name, uint64_t default_value)
{
    const char *value = GetEnvValue(name);
    if (!value)
    {
        return default_value;
    }
    char *end = nullptr;
    unsigned long long parsed = std::strtoull(value, &end, 10);
    if (!end || *end != '\0' || parsed == 0)
    {
        return default_value;
    }
    return static_cast<uint64_t>(parsed);
}

RealProviderSmokeResult MakeResult(RealProviderSmokeStatus status,
                                   const std::string &message,
                                   uint32_t attempts = 0,
                                   int http_status = 0)
{
    RealProviderSmokeResult result;
    result.status = status;
    result.message = message;
    result.attempts = attempts;
    result.http_status = http_status;
    return result;
}

bool CheckRequired(const std::string &value, const char *name, std::string *error)
{
    if (!value.empty())
    {
        return true;
    }
    if (error)
    {
        *error = std::string("missing required env: ") + name;
    }
    return false;
}

GatewayChatRequest BuildSmokeChatRequest(const RealProviderSmokeConfig &config)
{
    GatewayChatRequest request;
    request.model = config.logical_model;

    ChatMessage message;
    message.role = "user";
    message.content = config.prompt;
    request.messages.push_back(message);
    return request;
}

ProviderCandidate BuildSmokeCandidate(const RealProviderSmokeConfig &config)
{
    ProviderCandidate candidate;
    candidate.name = config.provider_name;
    candidate.adapter_type = ProviderAdapterType::OPENAI_COMPATIBLE;
    candidate.logical_model = config.logical_model;
    candidate.base_url = config.base_url;
    candidate.chat_path = config.chat_path;
    candidate.upstream_model = config.upstream_model;
    candidate.api_key_env = config.api_key_env;
    candidate.compatibility_key = config.compatibility_key;
    candidate.enabled = true;
    return candidate;
}

sylar::http::HttpRequestOptions BuildSmokeRequestOptions(const RealProviderSmokeConfig &config)
{
    sylar::http::HttpRequestOptions options =
        sylar::http::HttpRequestOptions::FromTimeout(config.request_deadline_ms);
    options.tls_server_name = config.tls_server_name;
    options.tls_ca_file = config.tls_ca_file;
    options.tls_ca_path = config.tls_ca_path;
    options.tls_verify_peer = true;
    return options;
}

int ExtractHttpStatus(const sylar::http::HttpResult::ptr &result)
{
    if (!result)
    {
        return 0;
    }
    if (result->response)
    {
        return static_cast<int>(result->response->getStatus());
    }
    return result->attempt.http_status;
}

} // namespace

const char *RealProviderSmokeStatusToString(RealProviderSmokeStatus status)
{
    switch (status)
    {
    case RealProviderSmokeStatus::SKIPPED:
        return "skipped";
    case RealProviderSmokeStatus::CONFIG_ERROR:
        return "config_error";
    case RealProviderSmokeStatus::REQUEST_FAILED:
        return "request_failed";
    case RealProviderSmokeStatus::OK:
        return "ok";
    default:
        return "unknown";
    }
}

std::ostream &operator<<(std::ostream &os, RealProviderSmokeStatus status)
{
    os << RealProviderSmokeStatusToString(status);
    return os;
}

RealProviderSmokeConfig LoadRealProviderSmokeConfigFromEnv()
{
    RealProviderSmokeConfig config;
    config.enabled = IsSmokeEnabled();
    config.provider_name =
        GetEnvString("SYLAR_AI_GATEWAY_REAL_PROVIDER_NAME", config.provider_name);
    config.logical_model =
        GetEnvString("SYLAR_AI_GATEWAY_REAL_LOGICAL_MODEL", config.logical_model);
    config.base_url = GetEnvString("SYLAR_AI_GATEWAY_REAL_BASE_URL", config.base_url);
    config.chat_path = GetEnvString("SYLAR_AI_GATEWAY_REAL_CHAT_PATH", config.chat_path);
    config.upstream_model =
        GetEnvString("SYLAR_AI_GATEWAY_REAL_UPSTREAM_MODEL", config.upstream_model);
    config.api_key_env = GetEnvString("SYLAR_AI_GATEWAY_REAL_API_KEY_ENV", config.api_key_env);
    config.compatibility_key =
        GetEnvString("SYLAR_AI_GATEWAY_REAL_COMPATIBILITY_KEY", config.compatibility_key);
    config.prompt = GetEnvString("SYLAR_AI_GATEWAY_REAL_PROMPT", config.prompt);
    config.request_deadline_ms =
        GetEnvUint64("SYLAR_AI_GATEWAY_REAL_REQUEST_DEADLINE_MS",
                     config.request_deadline_ms);
    config.tls_server_name =
        GetEnvString("SYLAR_AI_GATEWAY_REAL_TLS_SERVER_NAME", config.tls_server_name);
    config.tls_ca_file = GetEnvString("SYLAR_AI_GATEWAY_REAL_TLS_CA_FILE", config.tls_ca_file);
    config.tls_ca_path = GetEnvString("SYLAR_AI_GATEWAY_REAL_TLS_CA_PATH", config.tls_ca_path);
    return config;
}

RealProviderSmokeResult RunRealProviderSmoke(const RealProviderSmokeConfig &config,
                                             const ProviderHttpPost &post)
{
    if (!config.enabled)
    {
        return MakeResult(RealProviderSmokeStatus::SKIPPED,
                          "set SYLAR_AI_GATEWAY_REAL_SMOKE=1 to run real provider smoke");
    }
    if (!post)
    {
        return MakeResult(RealProviderSmokeStatus::CONFIG_ERROR,
                          "provider HTTP post handler missing");
    }

    std::string error;
    if (!CheckRequired(config.base_url, "SYLAR_AI_GATEWAY_REAL_BASE_URL", &error) ||
        !CheckRequired(config.upstream_model, "SYLAR_AI_GATEWAY_REAL_UPSTREAM_MODEL", &error) ||
        !CheckRequired(config.api_key_env, "SYLAR_AI_GATEWAY_REAL_API_KEY_ENV", &error))
    {
        return MakeResult(RealProviderSmokeStatus::CONFIG_ERROR, error);
    }
    if (!GetEnvValue(config.api_key_env.c_str()))
    {
        return MakeResult(RealProviderSmokeStatus::CONFIG_ERROR,
                          "provider API key env missing: " + config.api_key_env);
    }

    LogicalModelRouter router;
    if (!router.addCandidate(BuildSmokeCandidate(config), &error))
    {
        return MakeResult(RealProviderSmokeStatus::CONFIG_ERROR, error);
    }

    RequestExecutionBudget budget(config.request_deadline_ms, 1, "manual-real-provider-smoke");
    ProviderAttemptExecutor executor(
        router, CreateOpenAICompatibleAttemptHandler(post, BuildSmokeRequestOptions(config)));
    sylar::http::HttpResult::ptr result = executor.execute(BuildSmokeChatRequest(config), &budget);

    const uint32_t attempts = budget.consumedAttempts();
    const int http_status = ExtractHttpStatus(result);
    if (!result)
    {
        return MakeResult(RealProviderSmokeStatus::REQUEST_FAILED,
                          "real provider smoke failed: missing result", attempts, http_status);
    }
    if (result->result == (int)sylar::http::HttpResult::Error::OK && result->response &&
        result->response->getStatus() == sylar::http::HttpStatus::OK)
    {
        return MakeResult(RealProviderSmokeStatus::OK,
                          "real provider smoke succeeded", attempts, http_status);
    }

    std::ostringstream os;
    os << "real provider smoke failed";
    if (!result->error.empty())
    {
        os << ": " << result->error;
    }
    return MakeResult(RealProviderSmokeStatus::REQUEST_FAILED, os.str(), attempts, http_status);
}

RealProviderSmokeResult RunRealProviderSmoke(const RealProviderSmokeConfig &config)
{
    return RunRealProviderSmoke(config, DoProviderHttpPost);
}

} // namespace ai_gateway
} // namespace sylar
