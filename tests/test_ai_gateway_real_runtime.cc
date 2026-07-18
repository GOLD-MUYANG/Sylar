#include "modules/ai_gateway/ai_gateway_protocol.h"
#include "modules/ai_gateway/real_provider_runtime.h"
#include "sylar/http/http_connection.h"
#include "sylar/log.h"

#include <cstdlib>
#include <atomic>
#include <condition_variable>
#include <json/json.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <yaml-cpp/yaml.h>

static sylar::Logger::ptr g_logger = SYLAR_LOG_ROOT();
static int g_failures = 0;

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

Json::Value ParseJson(const std::string &body)
{
    Json::CharReaderBuilder builder;
    Json::Value root;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    EXPECT_TRUE(reader->parse(body.data(), body.data() + body.size(), &root, &errors));
    return root;
}

sylar::http::HttpResult::ptr ProviderOkBody(const std::string &content)
{
    sylar::http::HttpResponse::ptr response(new sylar::http::HttpResponse);
    response->setStatus(sylar::http::HttpStatus::OK);
    response->setBody(
        "{\"id\":\"chatcmpl-runtime\",\"object\":\"chat.completion\",\"created\":123,"
        "\"model\":\"upstream-a\",\"choices\":[{\"index\":0,\"message\":{"
        "\"role\":\"assistant\",\"content\":\"" +
        content +
        "\"},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":1,"
        "\"completion_tokens\":2,\"total_tokens\":3}}");

    sylar::http::HttpResult::ptr result = std::make_shared<sylar::http::HttpResult>(
        (int)sylar::http::HttpResult::Error::OK, response, "ok");
    result->attempt.phase = sylar::http::HttpAttemptPhase::RESPONSE_RECEIVED;
    result->attempt.response_started = true;
    result->attempt.response_completed = true;
    result->attempt.http_status = (int)sylar::http::HttpStatus::OK;
    return result;
}

YAML::Node RuntimeYaml()
{
    return YAML::Load(
        "real_providers:\n"
        "  enabled: true\n"
        "  server_name: ai-gateway\n"
        "  route_path: /v1/chat/completions\n"
        "  demo_enabled: true\n"
        "  load_balance: ROUND_ROBIN\n"
        "  request_deadline_ms: 30000\n"
        "  max_total_attempts: 2\n"
        "  limiter:\n"
        "    max_endpoint_concurrency: 2\n"
        "  circuit_breaker:\n"
        "    enabled: true\n"
        "    consecutive_failure_threshold: 3\n"
        "    open_timeout_ms: 5000\n"
        "    half_open_max_requests: 1\n"
        "  providers:\n"
        "    - name: provider-a\n"
        "      enabled: true\n"
        "      type: openai_compatible\n"
        "      logical_model: general-chat\n"
        "      compatibility_key: chat-basic-v1\n"
        "      base_url: https://api-a.example/v1\n"
        "      chat_path: /chat/completions\n"
        "      upstream_model: upstream-a\n"
        "      api_key_env: SYLAR_TEST_REAL_PROVIDER_KEY\n"
        "      tls_server_name: api-a.example\n"
        "      weight: 1\n");
}

YAML::Node RuntimeYamlWithTwoProviders()
{
    return YAML::Load(
        "real_providers:\n"
        "  enabled: true\n"
        "  server_name: ai-gateway\n"
        "  route_path: /v1/chat/completions\n"
        "  demo_enabled: true\n"
        "  load_balance: ROUND_ROBIN\n"
        "  request_deadline_ms: 30000\n"
        "  max_total_attempts: 2\n"
        "  providers:\n"
        "    - name: provider-a\n"
        "      enabled: true\n"
        "      type: openai_compatible\n"
        "      logical_model: general-chat\n"
        "      compatibility_key: chat-basic-v1\n"
        "      base_url: https://api-a.example/v1\n"
        "      chat_path: /chat/completions\n"
        "      upstream_model: upstream-a\n"
        "      api_key_env: SYLAR_TEST_REAL_PROVIDER_KEY\n"
        "      weight: 1\n"
        "    - name: provider-b\n"
        "      enabled: true\n"
        "      type: openai_compatible\n"
        "      logical_model: general-chat\n"
        "      compatibility_key: chat-basic-v1\n"
        "      base_url: https://api-b.example/v1\n"
        "      chat_path: /chat/completions\n"
        "      upstream_model: upstream-b\n"
        "      api_key_env: SYLAR_TEST_REAL_PROVIDER_KEY\n"
        "      weight: 1\n");
}

void test_runtime_config_reads_enabled_providers_and_env_only_key()
{
    setenv("SYLAR_TEST_REAL_PROVIDER_KEY", "sk-runtime-secret", 1);

    sylar::ai_gateway::RealProviderRuntimeConfig config;
    std::string error;
    EXPECT_TRUE(sylar::ai_gateway::LoadRealProviderRuntimeConfig(RuntimeYaml(), &config, &error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(config.enabled);
    EXPECT_TRUE(config.demo_enabled);
    EXPECT_EQ(config.load_balance, "ROUND_ROBIN");
    EXPECT_EQ(config.server_name, "ai-gateway");
    EXPECT_EQ(config.route_path, "/v1/chat/completions");
    EXPECT_EQ(config.request_deadline_ms, 30000U);
    EXPECT_EQ(config.max_total_attempts, 2U);
    EXPECT_EQ(config.providers.size(), (size_t)1);
    EXPECT_EQ(config.providers[0].name, "provider-a");
    EXPECT_EQ(config.providers[0].logical_model, "general-chat");
    EXPECT_EQ(config.providers[0].compatibility_key, "chat-basic-v1");
    EXPECT_EQ(config.providers[0].api_key_env, "SYLAR_TEST_REAL_PROVIDER_KEY");
    EXPECT_EQ(config.providers[0].tls_server_name, "api-a.example");

    unsetenv("SYLAR_TEST_REAL_PROVIDER_KEY");
}

void test_enabled_provider_requires_present_environment_key()
{
    unsetenv("SYLAR_TEST_REAL_PROVIDER_KEY");

    sylar::ai_gateway::RealProviderRuntimeConfig config;
    std::string error;
    EXPECT_TRUE(!sylar::ai_gateway::LoadRealProviderRuntimeConfig(RuntimeYaml(), &config, &error));
    EXPECT_TRUE(error.find("provider-a") != std::string::npos);
    EXPECT_TRUE(error.find("sk-runtime-secret") == std::string::npos);
}

void test_runtime_executes_real_provider_and_builds_sanitized_trace_and_status()
{
    setenv("SYLAR_TEST_REAL_PROVIDER_KEY", "sk-runtime-secret", 1);

    sylar::ai_gateway::RealProviderRuntimeConfig config;
    std::string error;
    EXPECT_TRUE(sylar::ai_gateway::LoadRealProviderRuntimeConfig(RuntimeYaml(), &config, &error));

    int call_count = 0;
    sylar::ai_gateway::RealProviderRuntime::ptr runtime =
        sylar::ai_gateway::RealProviderRuntime::Create(
            config,
            [&call_count](const sylar::ai_gateway::ProviderCandidate &candidate,
                          const sylar::ai_gateway::ProviderHttpRequest &request) {
                ++call_count;
                EXPECT_EQ(candidate.name, "provider-a");
                EXPECT_EQ(request.url, "https://api-a.example/v1/chat/completions");
                EXPECT_EQ(request.headers.at("Authorization"), "Bearer sk-runtime-secret");
                EXPECT_EQ(request.options.tls_server_name, "api-a.example");
                return ProviderOkBody("real runtime answer");
            });
    EXPECT_TRUE(runtime != nullptr);

    sylar::ai_gateway::GatewayChatRequest request;
    request.model = "general-chat";
    request.messages.push_back({"user", "hello real provider"});

    Json::Value trace;
    sylar::http::HttpResult::ptr result = runtime->execute(request, &trace);
    EXPECT_TRUE(result != nullptr);
    EXPECT_EQ(result->result, (int)sylar::http::HttpResult::Error::OK);
    EXPECT_EQ(call_count, 1);

    Json::Value response = ParseJson(result->response->getBody());
    EXPECT_EQ(response["model"].asString(), "general-chat");
    EXPECT_EQ(response["choices"][0]["message"]["content"].asString(), "real runtime answer");

    const std::string trace_text = sylar::ai_gateway::DumpRealProviderTrace(trace);
    EXPECT_TRUE(trace_text.find("request_received") != std::string::npos);
    EXPECT_TRUE(trace_text.find("candidates_selected") != std::string::npos);
    EXPECT_TRUE(trace_text.find("attempt_result") != std::string::npos);
    EXPECT_TRUE(trace_text.find("sk-runtime-secret") == std::string::npos);
    EXPECT_TRUE(trace_text.find("hello real provider") == std::string::npos);

    Json::Value status = runtime->buildStatusJson();
    EXPECT_EQ(status["object"].asString(), "ai_gateway.real_provider.status");
    EXPECT_EQ(status["providers"].size(), 1U);
    EXPECT_EQ(status["providers"][0]["name"].asString(), "provider-a");
    EXPECT_EQ(status["providers"][0]["logical_model"].asString(), "general-chat");
    EXPECT_EQ(status["providers"][0]["adapter_type"].asString(), "openai_compatible");
    EXPECT_EQ(status["providers"][0]["upstream_model"].asString(), "upstream-a");
    EXPECT_EQ(status["providers"][0]["endpoint"].asString(), "https://api-a.example");
    EXPECT_TRUE(status.toStyledString().find("sk-runtime-secret") == std::string::npos);
    EXPECT_TRUE(status.toStyledString().find("SYLAR_TEST_REAL_PROVIDER_KEY") == std::string::npos);

    unsetenv("SYLAR_TEST_REAL_PROVIDER_KEY");
}

void test_runtime_rotates_first_provider_for_successful_requests()
{
    setenv("SYLAR_TEST_REAL_PROVIDER_KEY", "sk-runtime-secret", 1);

    sylar::ai_gateway::RealProviderRuntimeConfig config;
    std::string error;
    EXPECT_TRUE(
        sylar::ai_gateway::LoadRealProviderRuntimeConfig(RuntimeYamlWithTwoProviders(), &config,
                                                         &error));

    std::vector<std::string> attempted_providers;
    sylar::ai_gateway::RealProviderRuntime::ptr runtime =
        sylar::ai_gateway::RealProviderRuntime::Create(
            config,
            [&attempted_providers](const sylar::ai_gateway::ProviderCandidate &candidate,
                                   const sylar::ai_gateway::ProviderHttpRequest &) {
                attempted_providers.push_back(candidate.name);
                return ProviderOkBody("answer from " + candidate.name);
            });
    EXPECT_TRUE(runtime != nullptr);

    sylar::ai_gateway::GatewayChatRequest request;
    request.model = "general-chat";
    request.messages.push_back({"user", "hello"});
    for (int i = 0; i < 4; ++i)
    {
        sylar::http::HttpResult::ptr result = runtime->execute(request);
        EXPECT_TRUE(result != nullptr);
        EXPECT_EQ(result->result, (int)sylar::http::HttpResult::Error::OK);
    }

    EXPECT_EQ(attempted_providers.size(), (size_t)4);
    EXPECT_EQ(attempted_providers[0], "provider-a");
    EXPECT_EQ(attempted_providers[1], "provider-b");
    EXPECT_EQ(attempted_providers[2], "provider-a");
    EXPECT_EQ(attempted_providers[3], "provider-b");

    unsetenv("SYLAR_TEST_REAL_PROVIDER_KEY");
}

void test_runtime_config_accepts_all_common_strategies_and_rejects_unknown()
{
    setenv("SYLAR_TEST_REAL_PROVIDER_KEY", "sk-runtime-secret", 1);
    const std::vector<std::string> strategies = {
        "ROUND_ROBIN", "RANDOM", "WEIGHTED_ROUND_ROBIN", "LEAST_CONNECTION"};
    for (const auto &strategy : strategies)
    {
        YAML::Node root = RuntimeYaml();
        root["real_providers"]["load_balance"] = strategy;
        sylar::ai_gateway::RealProviderRuntimeConfig config;
        std::string error;
        EXPECT_TRUE(sylar::ai_gateway::LoadRealProviderRuntimeConfig(root, &config, &error));
        EXPECT_EQ(config.load_balance, strategy);
    }

    YAML::Node invalid = RuntimeYaml();
    invalid["real_providers"]["load_balance"] = "UNKNOWN";
    sylar::ai_gateway::RealProviderRuntimeConfig config;
    std::string error;
    EXPECT_TRUE(!sylar::ai_gateway::LoadRealProviderRuntimeConfig(invalid, &config, &error));
    EXPECT_TRUE(error.find("UNKNOWN") != std::string::npos);
    unsetenv("SYLAR_TEST_REAL_PROVIDER_KEY");
}

void test_runtime_weighted_round_robin_uses_provider_weight()
{
    setenv("SYLAR_TEST_REAL_PROVIDER_KEY", "sk-runtime-secret", 1);
    YAML::Node root = RuntimeYamlWithTwoProviders();
    root["real_providers"]["load_balance"] = "WEIGHTED_ROUND_ROBIN";
    root["real_providers"]["providers"][0]["weight"] = 2;
    root["real_providers"]["providers"][1]["weight"] = 1;

    sylar::ai_gateway::RealProviderRuntimeConfig config;
    std::string error;
    EXPECT_TRUE(sylar::ai_gateway::LoadRealProviderRuntimeConfig(root, &config, &error));
    std::vector<std::string> attempted;
    auto runtime = sylar::ai_gateway::RealProviderRuntime::Create(
        config,
        [&attempted](const sylar::ai_gateway::ProviderCandidate &candidate,
                     const sylar::ai_gateway::ProviderHttpRequest &) {
            attempted.push_back(candidate.name);
            return ProviderOkBody("answer");
        });
    EXPECT_TRUE(runtime != nullptr);
    if (!runtime)
    {
        unsetenv("SYLAR_TEST_REAL_PROVIDER_KEY");
        return;
    }

    sylar::ai_gateway::GatewayChatRequest request;
    request.model = "general-chat";
    request.messages.push_back({"user", "hello"});
    for (size_t i = 0; i < 6; ++i)
    {
        EXPECT_EQ(runtime->execute(request)->result, (int)sylar::http::HttpResult::Error::OK);
    }
    const std::vector<std::string> expected = {
        "provider-a", "provider-a", "provider-b", "provider-a", "provider-a", "provider-b"};
    EXPECT_EQ(attempted.size(), expected.size());
    for (size_t i = 0; i < attempted.size() && i < expected.size(); ++i)
    {
        EXPECT_EQ(attempted[i], expected[i]);
    }
    unsetenv("SYLAR_TEST_REAL_PROVIDER_KEY");
}

void test_runtime_least_connection_reads_latest_in_flight()
{
    setenv("SYLAR_TEST_REAL_PROVIDER_KEY", "sk-runtime-secret", 1);
    YAML::Node root = RuntimeYamlWithTwoProviders();
    root["real_providers"]["load_balance"] = "LEAST_CONNECTION";
    sylar::ai_gateway::RealProviderRuntimeConfig config;
    std::string error;
    EXPECT_TRUE(sylar::ai_gateway::LoadRealProviderRuntimeConfig(root, &config, &error));

    std::mutex mutex;
    std::condition_variable condition;
    bool provider_a_entered = false;
    bool release_provider_a = false;
    std::atomic<uint32_t> provider_a_calls(0);
    std::atomic<uint32_t> provider_b_calls(0);
    auto runtime = sylar::ai_gateway::RealProviderRuntime::Create(
        config,
        [&](const sylar::ai_gateway::ProviderCandidate &candidate,
            const sylar::ai_gateway::ProviderHttpRequest &) {
            if (candidate.name == "provider-a")
            {
                ++provider_a_calls;
                std::unique_lock<std::mutex> lock(mutex);
                provider_a_entered = true;
                condition.notify_all();
                condition.wait(lock, [&release_provider_a]() { return release_provider_a; });
            }
            else
            {
                ++provider_b_calls;
            }
            return ProviderOkBody("answer");
        });
    EXPECT_TRUE(runtime != nullptr);
    if (!runtime)
    {
        unsetenv("SYLAR_TEST_REAL_PROVIDER_KEY");
        return;
    }

    sylar::ai_gateway::GatewayChatRequest request;
    request.model = "general-chat";
    request.messages.push_back({"user", "hello"});
    std::thread first([runtime, request]() { runtime->execute(request); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&provider_a_entered]() { return provider_a_entered; });
    }
    EXPECT_EQ(runtime->execute(request)->result, (int)sylar::http::HttpResult::Error::OK);
    EXPECT_EQ(runtime->execute(request)->result, (int)sylar::http::HttpResult::Error::OK);
    EXPECT_EQ(provider_a_calls.load(), (uint32_t)1);
    EXPECT_EQ(provider_b_calls.load(), (uint32_t)2);
    {
        std::lock_guard<std::mutex> lock(mutex);
        release_provider_a = true;
    }
    condition.notify_all();
    first.join();
    unsetenv("SYLAR_TEST_REAL_PROVIDER_KEY");
}

void test_runtime_releases_in_flight_when_handler_throws()
{
    setenv("SYLAR_TEST_REAL_PROVIDER_KEY", "sk-runtime-secret", 1);
    sylar::ai_gateway::RealProviderRuntimeConfig config;
    std::string error;
    EXPECT_TRUE(sylar::ai_gateway::LoadRealProviderRuntimeConfig(RuntimeYaml(), &config, &error));
    auto runtime = sylar::ai_gateway::RealProviderRuntime::Create(
        config,
        [](const sylar::ai_gateway::ProviderCandidate &,
           const sylar::ai_gateway::ProviderHttpRequest &) -> sylar::http::HttpResult::ptr {
            throw std::runtime_error("runtime handler failure");
        });
    EXPECT_TRUE(runtime != nullptr);
    if (!runtime)
    {
        unsetenv("SYLAR_TEST_REAL_PROVIDER_KEY");
        return;
    }

    sylar::ai_gateway::GatewayChatRequest request;
    request.model = "general-chat";
    request.messages.push_back({"user", "hello"});
    auto result = runtime->execute(request);
    EXPECT_TRUE(result != nullptr);
    EXPECT_EQ(result->result, (int)sylar::http::HttpResult::Error::CONNECT_FAIL);
    EXPECT_EQ(runtime->buildStatusJson()["providers"][0]["in_flight"].asUInt(), 0U);
    unsetenv("SYLAR_TEST_REAL_PROVIDER_KEY");
}

} // namespace

int main()
{
    test_runtime_config_reads_enabled_providers_and_env_only_key();
    test_enabled_provider_requires_present_environment_key();
    test_runtime_executes_real_provider_and_builds_sanitized_trace_and_status();
    test_runtime_rotates_first_provider_for_successful_requests();
    test_runtime_config_accepts_all_common_strategies_and_rejects_unknown();
    test_runtime_weighted_round_robin_uses_provider_weight();
    test_runtime_least_connection_reads_latest_in_flight();
    test_runtime_releases_in_flight_when_handler_throws();
    return g_failures == 0 ? 0 : 1;
}
