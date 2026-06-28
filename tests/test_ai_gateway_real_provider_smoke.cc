#include "modules/ai_gateway/real_provider_smoke.h"

#include "sylar/log.h"

#include <atomic>
#include <cstdlib>
#include <string>
#include <unistd.h>

static sylar::Logger::ptr g_logger = SYLAR_LOG_ROOT();
static std::atomic<int> g_failures(0);

#define EXPECT_TRUE(expr)                                                                       \
    do                                                                                          \
    {                                                                                           \
        if (!(expr))                                                                            \
        {                                                                                       \
            ++g_failures;                                                                       \
            SYLAR_LOG_ERROR(g_logger) << "EXPECT_TRUE failed: " #expr << " line=" << __LINE__; \
        }                                                                                       \
    } while (0)

#define EXPECT_EQ(lhs, rhs)                                                                     \
    do                                                                                          \
    {                                                                                           \
        auto _lhs = (lhs);                                                                      \
        auto _rhs = (rhs);                                                                      \
        if (!(_lhs == _rhs))                                                                    \
        {                                                                                       \
            ++g_failures;                                                                       \
            SYLAR_LOG_ERROR(g_logger) << "EXPECT_EQ failed: " #lhs "=" << _lhs << " " #rhs "="  \
                                      << _rhs << " line=" << __LINE__;                          \
        }                                                                                       \
    } while (0)

namespace
{

void ClearSmokeEnv()
{
    unsetenv("SYLAR_AI_GATEWAY_REAL_SMOKE");
    unsetenv("SYLAR_AI_GATEWAY_REAL_PROVIDER_NAME");
    unsetenv("SYLAR_AI_GATEWAY_REAL_BASE_URL");
    unsetenv("SYLAR_AI_GATEWAY_REAL_CHAT_PATH");
    unsetenv("SYLAR_AI_GATEWAY_REAL_LOGICAL_MODEL");
    unsetenv("SYLAR_AI_GATEWAY_REAL_UPSTREAM_MODEL");
    unsetenv("SYLAR_AI_GATEWAY_REAL_API_KEY_ENV");
    unsetenv("SYLAR_AI_GATEWAY_REAL_REQUEST_DEADLINE_MS");
    unsetenv("SYLAR_AI_GATEWAY_REAL_PROMPT");
    unsetenv("SYLAR_AI_GATEWAY_REAL_TLS_SERVER_NAME");
    unsetenv("SYLAR_AI_GATEWAY_REAL_TLS_CA_FILE");
    unsetenv("SYLAR_AI_GATEWAY_REAL_TLS_CA_PATH");
    unsetenv("REAL_PROVIDER_SMOKE_KEY");
    unsetenv("MISSING_REAL_PROVIDER_SMOKE_KEY");
}

sylar::http::HttpResult::ptr OpenAICompatibleOkResult()
{
    sylar::http::HttpResponse::ptr response(new sylar::http::HttpResponse);
    response->setStatus(sylar::http::HttpStatus::OK);
    response->setBody(
        "{\"id\":\"chatcmpl-smoke\",\"object\":\"chat.completion\",\"created\":123,"
        "\"model\":\"upstream-smoke\","
        "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\","
        "\"content\":\"pong\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":1,\"total_tokens\":2}}");

    sylar::http::HttpResult::ptr result = std::make_shared<sylar::http::HttpResult>(
        (int)sylar::http::HttpResult::Error::OK, response, "ok");
    result->attempt.phase = sylar::http::HttpAttemptPhase::RESPONSE_RECEIVED;
    result->attempt.response_started = true;
    result->attempt.response_completed = true;
    result->attempt.http_status = (int)sylar::http::HttpStatus::OK;
    return result;
}

void test_default_guard_skips_without_network()
{
    ClearSmokeEnv();

    sylar::ai_gateway::RealProviderSmokeConfig config =
        sylar::ai_gateway::LoadRealProviderSmokeConfigFromEnv();
    int calls = 0;
    sylar::ai_gateway::RealProviderSmokeResult result =
        sylar::ai_gateway::RunRealProviderSmoke(
            config,
            [&calls](const sylar::ai_gateway::ProviderCandidate &,
                     const sylar::ai_gateway::ProviderHttpRequest &)
            {
                ++calls;
                return OpenAICompatibleOkResult();
            });

    EXPECT_EQ(result.status, sylar::ai_gateway::RealProviderSmokeStatus::SKIPPED);
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(result.attempts, (uint32_t)0);
}

void test_missing_api_key_env_does_not_call_provider()
{
    ClearSmokeEnv();
    setenv("SYLAR_AI_GATEWAY_REAL_SMOKE", "1", 1);
    setenv("SYLAR_AI_GATEWAY_REAL_BASE_URL", "https://api.provider.example", 1);
    setenv("SYLAR_AI_GATEWAY_REAL_UPSTREAM_MODEL", "provider-model", 1);
    setenv("SYLAR_AI_GATEWAY_REAL_API_KEY_ENV", "MISSING_REAL_PROVIDER_SMOKE_KEY", 1);

    sylar::ai_gateway::RealProviderSmokeConfig config =
        sylar::ai_gateway::LoadRealProviderSmokeConfigFromEnv();
    int calls = 0;
    sylar::ai_gateway::RealProviderSmokeResult result =
        sylar::ai_gateway::RunRealProviderSmoke(
            config,
            [&calls](const sylar::ai_gateway::ProviderCandidate &,
                     const sylar::ai_gateway::ProviderHttpRequest &)
            {
                ++calls;
                return OpenAICompatibleOkResult();
            });

    EXPECT_EQ(result.status, sylar::ai_gateway::RealProviderSmokeStatus::CONFIG_ERROR);
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(result.attempts, (uint32_t)0);
    EXPECT_TRUE(result.message.find("MISSING_REAL_PROVIDER_SMOKE_KEY") != std::string::npos);
}

void test_enabled_smoke_uses_single_attempt_with_fake_transport()
{
    ClearSmokeEnv();
    setenv("SYLAR_AI_GATEWAY_REAL_SMOKE", "1", 1);
    setenv("SYLAR_AI_GATEWAY_REAL_PROVIDER_NAME", "manual-provider", 1);
    setenv("SYLAR_AI_GATEWAY_REAL_BASE_URL", "https://api.provider.example", 1);
    setenv("SYLAR_AI_GATEWAY_REAL_CHAT_PATH", "/v1/chat/completions", 1);
    setenv("SYLAR_AI_GATEWAY_REAL_LOGICAL_MODEL", "general-chat", 1);
    setenv("SYLAR_AI_GATEWAY_REAL_UPSTREAM_MODEL", "provider-model", 1);
    setenv("SYLAR_AI_GATEWAY_REAL_API_KEY_ENV", "REAL_PROVIDER_SMOKE_KEY", 1);
    setenv("SYLAR_AI_GATEWAY_REAL_REQUEST_DEADLINE_MS", "15000", 1);
    setenv("SYLAR_AI_GATEWAY_REAL_PROMPT", "reply pong", 1);
    setenv("REAL_PROVIDER_SMOKE_KEY", "secret-value", 1);

    sylar::ai_gateway::RealProviderSmokeConfig config =
        sylar::ai_gateway::LoadRealProviderSmokeConfigFromEnv();

    int calls = 0;
    std::string observed_url;
    std::string observed_auth;
    std::string observed_body;
    sylar::ai_gateway::RealProviderSmokeResult result =
        sylar::ai_gateway::RunRealProviderSmoke(
            config,
            [&calls, &observed_url, &observed_auth, &observed_body](
                const sylar::ai_gateway::ProviderCandidate &,
                const sylar::ai_gateway::ProviderHttpRequest &request)
            {
                ++calls;
                observed_url = request.url;
                observed_auth = request.headers.find("Authorization")->second;
                observed_body = request.body;
                return OpenAICompatibleOkResult();
            });

    EXPECT_EQ(result.status, sylar::ai_gateway::RealProviderSmokeStatus::OK);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(result.attempts, (uint32_t)1);
    EXPECT_EQ(result.http_status, 200);
    EXPECT_EQ(observed_url, std::string("https://api.provider.example/v1/chat/completions"));
    EXPECT_EQ(observed_auth, std::string("Bearer secret-value"));
    EXPECT_TRUE(observed_body.find("\"model\":\"provider-model\"") != std::string::npos);
    EXPECT_TRUE(observed_body.find("reply pong") != std::string::npos);
    EXPECT_TRUE(result.message.find("secret-value") == std::string::npos);
    EXPECT_TRUE(result.message.find("reply pong") == std::string::npos);
}

} // namespace

int main()
{
    test_default_guard_skips_without_network();
    test_missing_api_key_env_does_not_call_provider();
    test_enabled_smoke_uses_single_attempt_with_fake_transport();

    if (g_failures.load() != 0)
    {
        SYLAR_LOG_ERROR(g_logger) << "test_ai_gateway_real_provider_smoke failures="
                                  << g_failures.load();
        return 1;
    }
    SYLAR_LOG_INFO(g_logger) << "test_ai_gateway_real_provider_smoke passed";
    return 0;
}
