#include "modules/ai_gateway/ai_gateway_protocol.h"
#include "modules/ai_gateway/ai_provider_adapter.h"
#include "modules/ai_gateway/real_provider_gateway.h"
#include "sylar/http/http_connection.h"
#include "sylar/log.h"

#include <cstdlib>
#include <json/json.h>

#include <atomic>
#include <string>
#include <vector>

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

#define EXPECT_NE(lhs, rhs)                                                                     \
    do                                                                                          \
    {                                                                                           \
        auto _lhs = (lhs);                                                                      \
        auto _rhs = (rhs);                                                                      \
        if (!(_lhs != _rhs))                                                                    \
        {                                                                                       \
            ++g_failures;                                                                       \
            SYLAR_LOG_ERROR(g_logger) << "EXPECT_NE failed: " #lhs "=" << _lhs << " " #rhs "="  \
                                      << _rhs << " line=" << __LINE__;                          \
        }                                                                                       \
    } while (0)

namespace
{

sylar::ai_gateway::ProviderCandidate MakeCandidate(const std::string &name,
                                                   const std::string &logical_model,
                                                   const std::string &base_url,
                                                   const std::string &chat_path,
                                                   const std::string &upstream_model,
                                                   const std::string &api_key_env)
{
    sylar::ai_gateway::ProviderCandidate candidate;
    candidate.name = name;
    candidate.adapter_type = sylar::ai_gateway::ProviderAdapterType::OPENAI_COMPATIBLE;
    candidate.logical_model = logical_model;
    candidate.base_url = base_url;
    candidate.chat_path = chat_path;
    candidate.upstream_model = upstream_model;
    candidate.api_key_env = api_key_env;
    candidate.compatibility_key = "text-chat";
    candidate.enabled = true;
    return candidate;
}

sylar::http::HttpResult::ptr OkResult()
{
    sylar::http::HttpResponse::ptr response(new sylar::http::HttpResponse);
    response->setStatus(sylar::http::HttpStatus::OK);
    response->setBody(sylar::ai_gateway::BuildMockProviderResponse("real-a", "ok"));
    return std::make_shared<sylar::http::HttpResult>(
        (int)sylar::http::HttpResult::Error::OK, response, "ok");
}

Json::Value ParseJson(const std::string &body)
{
    Json::CharReaderBuilder builder;
    Json::Value root;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    EXPECT_TRUE(reader->parse(body.data(), body.data() + body.size(), &root, &errors));
    return root;
}

sylar::http::HttpResult::ptr OpenAICompatibleOkResult()
{
    sylar::http::HttpResponse::ptr response(new sylar::http::HttpResponse);
    response->setStatus(sylar::http::HttpStatus::OK);
    response->setBody(
        "{\"id\":\"chatcmpl-test\",\"object\":\"chat.completion\",\"model\":\"upstream-a\","
        "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\","
        "\"content\":\"adapter answer\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":2,\"total_tokens\":3}}");
    return std::make_shared<sylar::http::HttpResult>(
        (int)sylar::http::HttpResult::Error::OK, response, "ok");
}

sylar::http::HttpResult::ptr ProviderStatusError(sylar::http::HttpStatus status,
                                                 const std::string &body = "")
{
    sylar::http::HttpResponse::ptr response(new sylar::http::HttpResponse);
    response->setStatus(status);
    response->setBody(body.empty() ? "{\"error\":\"provider raw error\"}" : body);

    sylar::http::HttpResult::ptr result = std::make_shared<sylar::http::HttpResult>(
        (int)sylar::http::HttpResult::Error::HTTP_STATUS_ERROR, response, "provider status error");
    result->attempt.phase = sylar::http::HttpAttemptPhase::RESPONSE_RECEIVED;
    result->attempt.response_started = true;
    result->attempt.response_completed = true;
    result->attempt.http_status = (int)status;
    return result;
}

sylar::http::HttpResult::ptr ProviderUnknownSubmittedResult()
{
    sylar::http::HttpResult::ptr result = std::make_shared<sylar::http::HttpResult>(
        (int)sylar::http::HttpResult::Error::RECV_TIMEOUT, nullptr, "read timeout after send");
    result->attempt.phase = sylar::http::HttpAttemptPhase::RESPONSE_TIMEOUT_AFTER_SEND;
    result->attempt.request_bytes_started = true;
    result->attempt.request_bytes_completed = true;
    result->attempt.may_have_submitted = true;
    result->attempt.detail = "read timeout after request body sent";
    return result;
}

sylar::http::HttpResult::ptr ProviderSendFailedBeforeBodyResult()
{
    sylar::http::HttpResult::ptr result = std::make_shared<sylar::http::HttpResult>(
        (int)sylar::http::HttpResult::Error::SEND_FAIL, nullptr, "send failed before body");
    result->attempt.phase = sylar::http::HttpAttemptPhase::SEND_FAILED_BEFORE_BODY;
    result->attempt.may_have_submitted = false;
    result->attempt.detail = "send failed before request body";
    return result;
}

sylar::http::HttpResult::ptr ProviderPartialWriteResult()
{
    sylar::http::HttpResult::ptr result = std::make_shared<sylar::http::HttpResult>(
        (int)sylar::http::HttpResult::Error::SEND_FAIL, nullptr, "partial write");
    result->attempt.phase = sylar::http::HttpAttemptPhase::PARTIAL_WRITE_OR_UNKNOWN;
    result->attempt.request_bytes_started = true;
    result->attempt.request_bytes_sent = 16;
    result->attempt.may_have_submitted = true;
    result->attempt.detail = "partial request write";
    return result;
}

void test_gateway_protocol_names_are_available()
{
    sylar::ai_gateway::GatewayChatRequest request;
    request.model = "general-chat";
    request.messages.push_back({"user", "hello"});

    sylar::ai_gateway::GatewayChatResponse response;
    response.model = request.model;
    response.content = "world";

    EXPECT_EQ(request.messages.size(), (size_t)1);
    EXPECT_EQ(response.model, "general-chat");
}

void test_router_keeps_only_compatible_candidates_in_model_pool()
{
    sylar::ai_gateway::LogicalModelRouter router;
    std::string error;

    auto first = MakeCandidate("provider-a", "general-chat", "https://a.example",
                               "/v1/chat/completions", "a-model", "PROVIDER_A_KEY");
    EXPECT_TRUE(router.addCandidate(first, &error));

    auto incompatible = MakeCandidate("provider-b", "general-chat", "https://b.example",
                                      "/v1/chat/completions", "b-model", "PROVIDER_B_KEY");
    incompatible.compatibility_key = "vision-chat";
    EXPECT_TRUE(!router.addCandidate(incompatible, &error));
    EXPECT_TRUE(!error.empty());

    auto candidates = router.findCandidates("general-chat");
    EXPECT_EQ(candidates.size(), (size_t)1);
    EXPECT_EQ(candidates[0].name, "provider-a");
    EXPECT_EQ(candidates[0].upstream_model, "a-model");
}

void test_provider_aware_executor_preserves_candidate_specific_request_shape()
{
    sylar::ai_gateway::LogicalModelRouter router;
    std::string error;
    EXPECT_TRUE(router.addCandidate(
        MakeCandidate("provider-a", "general-chat", "https://api-a.example",
                      "/custom/chat", "upstream-a", "PROVIDER_A_KEY"),
        &error));

    std::vector<sylar::ai_gateway::ProviderCandidate> observed;
    sylar::ai_gateway::ProviderAttemptExecutor executor(
        router,
        [&observed](const sylar::ai_gateway::ProviderCandidate &candidate,
                    const sylar::ai_gateway::GatewayChatRequest &) {
            observed.push_back(candidate);
            return OkResult();
        });

    sylar::ai_gateway::GatewayChatRequest request;
    request.model = "general-chat";
    request.messages.push_back({"user", "hello"});

    auto result = executor.execute(request);
    EXPECT_TRUE(result != nullptr);
    EXPECT_EQ(result->result, (int)sylar::http::HttpResult::Error::OK);
    EXPECT_EQ(observed.size(), (size_t)1);
    EXPECT_EQ((int)observed[0].adapter_type,
              (int)sylar::ai_gateway::ProviderAdapterType::OPENAI_COMPATIBLE);
    EXPECT_EQ(observed[0].base_url, "https://api-a.example");
    EXPECT_EQ(observed[0].chat_path, "/custom/chat");
    EXPECT_EQ(observed[0].upstream_model, "upstream-a");
    EXPECT_EQ(observed[0].api_key_env, "PROVIDER_A_KEY");
}

void test_openai_adapter_builds_request_and_parses_response_via_executor()
{
    setenv("SYLAR_TEST_PROVIDER_KEY", "sk-r2-secret", 1);

    sylar::ai_gateway::LogicalModelRouter router;
    std::string error;
    EXPECT_TRUE(router.addCandidate(
        MakeCandidate("provider-a", "general-chat", "https://api-a.example",
                      "/v1/chat/completions", "upstream-a", "SYLAR_TEST_PROVIDER_KEY"),
        &error));

    int call_count = 0;
    auto handler = sylar::ai_gateway::CreateOpenAICompatibleAttemptHandler(
        [&call_count](const sylar::ai_gateway::ProviderCandidate &candidate,
                      const sylar::ai_gateway::ProviderHttpRequest &http_request) {
            ++call_count;
            EXPECT_EQ(candidate.name, "provider-a");
            EXPECT_EQ(http_request.url, "https://api-a.example/v1/chat/completions");
            EXPECT_EQ(http_request.headers.at("Authorization"), "Bearer sk-r2-secret");
            EXPECT_EQ(http_request.headers.at("Content-Type"), "application/json");

            Json::Value body = ParseJson(http_request.body);
            EXPECT_EQ(body["model"].asString(), "upstream-a");
            EXPECT_TRUE(body["stream"].isBool());
            EXPECT_TRUE(!body["stream"].asBool());
            EXPECT_EQ(body["messages"].size(), 2U);
            EXPECT_EQ(body["messages"][0]["role"].asString(), "system");
            EXPECT_EQ(body["messages"][1]["content"].asString(), "hello from user");
            return OpenAICompatibleOkResult();
        });

    sylar::ai_gateway::GatewayChatRequest request;
    request.model = "general-chat";
    request.messages.push_back({"system", "be concise"});
    request.messages.push_back({"user", "hello from user"});

    sylar::ai_gateway::ProviderAttemptExecutor executor(router, handler);
    auto result = executor.execute(request);
    EXPECT_TRUE(result != nullptr);
    EXPECT_EQ(call_count, 1);
    EXPECT_EQ(result->result, (int)sylar::http::HttpResult::Error::OK);

    Json::Value response = ParseJson(result->response->getBody());
    EXPECT_EQ(response["object"].asString(), "chat.completion");
    EXPECT_EQ(response["model"].asString(), "general-chat");
    EXPECT_EQ(response["choices"][0]["message"]["content"].asString(), "adapter answer");

    unsetenv("SYLAR_TEST_PROVIDER_KEY");
}

void test_openai_adapter_sanitizes_parse_error()
{
    setenv("SYLAR_TEST_PROVIDER_KEY", "sk-r2-secret", 1);

    sylar::ai_gateway::LogicalModelRouter router;
    std::string error;
    EXPECT_TRUE(router.addCandidate(
        MakeCandidate("provider-a", "general-chat", "https://api-a.example",
                      "/v1/chat/completions", "upstream-a", "SYLAR_TEST_PROVIDER_KEY"),
        &error));

    auto handler = sylar::ai_gateway::CreateOpenAICompatibleAttemptHandler(
        [](const sylar::ai_gateway::ProviderCandidate &,
           const sylar::ai_gateway::ProviderHttpRequest &) {
            sylar::http::HttpResponse::ptr response(new sylar::http::HttpResponse);
            response->setStatus(sylar::http::HttpStatus::OK);
            response->setBody("{\"raw\":\"hello from user sk-r2-secret\"}");
            return std::make_shared<sylar::http::HttpResult>(
                (int)sylar::http::HttpResult::Error::OK, response, "ok");
        });

    sylar::ai_gateway::GatewayChatRequest request;
    request.model = "general-chat";
    request.messages.push_back({"user", "hello from user"});

    sylar::ai_gateway::ProviderAttemptExecutor executor(router, handler);
    auto result = executor.execute(request);
    EXPECT_TRUE(result != nullptr);
    EXPECT_EQ(result->result, (int)sylar::http::HttpResult::Error::RESPONSE_PARSE_FAIL);
    EXPECT_EQ(result->error.find("sk-r2-secret"), std::string::npos);
    EXPECT_EQ(result->error.find("hello from user"), std::string::npos);

    unsetenv("SYLAR_TEST_PROVIDER_KEY");
}

sylar::http::HttpResult::ptr RetryableNotSentFailure()
{
    sylar::http::HttpResult::ptr result = std::make_shared<sylar::http::HttpResult>(
        (int)sylar::http::HttpResult::Error::CONNECT_FAIL, nullptr, "connect failed");
    result->attempt.phase = sylar::http::HttpAttemptPhase::NOT_SENT;
    result->attempt.may_have_submitted = false;
    return result;
}

void test_request_execution_budget_limits_provider_failover_attempts()
{
    sylar::ai_gateway::LogicalModelRouter router;
    std::string error;
    EXPECT_TRUE(router.addCandidate(
        MakeCandidate("provider-a", "general-chat", "https://api-a.example",
                      "/v1/chat/completions", "upstream-a", "PROVIDER_A_KEY"),
        &error));
    EXPECT_TRUE(router.addCandidate(
        MakeCandidate("provider-b", "general-chat", "https://api-b.example",
                      "/v1/chat/completions", "upstream-b", "PROVIDER_B_KEY"),
        &error));

    int call_count = 0;
    sylar::ai_gateway::ProviderAttemptExecutor executor(
        router,
        [&call_count](const sylar::ai_gateway::ProviderCandidate &,
                      const sylar::ai_gateway::GatewayChatRequest &,
                      sylar::ai_gateway::RequestExecutionBudget *) {
            ++call_count;
            return RetryableNotSentFailure();
        });

    sylar::ai_gateway::GatewayChatRequest request;
    request.model = "general-chat";
    request.messages.push_back({"user", "hello"});

    sylar::ai_gateway::RequestExecutionBudget budget(1000, 1, "req-r3");
    auto result = executor.execute(request, &budget);

    EXPECT_TRUE(result != nullptr);
    EXPECT_EQ(call_count, 1);
    EXPECT_EQ(budget.consumedAttempts(), 1U);
    EXPECT_EQ((int)budget.stopReason(),
              (int)sylar::ai_gateway::RequestExecutionBudget::StopReason::ATTEMPTS_EXHAUSTED);
    EXPECT_NE(result->error.find("attempts exhausted"), std::string::npos);
}

void test_openai_adapter_applies_remaining_budget_to_http_options()
{
    setenv("SYLAR_TEST_PROVIDER_KEY", "sk-r3-secret", 1);

    sylar::ai_gateway::LogicalModelRouter router;
    std::string error;
    EXPECT_TRUE(router.addCandidate(
        MakeCandidate("provider-a", "general-chat", "https://api-a.example",
                      "/v1/chat/completions", "upstream-a", "SYLAR_TEST_PROVIDER_KEY"),
        &error));

    sylar::http::HttpRequestOptions base_options;
    base_options.connect_timeout_ms = 5000;
    base_options.send_timeout_ms = 5000;
    base_options.recv_timeout_ms = 5000;
    base_options.total_timeout_ms = 5000;

    int call_count = 0;
    auto handler = sylar::ai_gateway::CreateOpenAICompatibleAttemptHandler(
        [&call_count](const sylar::ai_gateway::ProviderCandidate &,
                      const sylar::ai_gateway::ProviderHttpRequest &http_request) {
            ++call_count;
            EXPECT_TRUE(http_request.options.connect_timeout_ms > 0);
            EXPECT_TRUE(http_request.options.connect_timeout_ms <= 1000);
            EXPECT_TRUE(http_request.options.send_timeout_ms > 0);
            EXPECT_TRUE(http_request.options.send_timeout_ms <= 1000);
            EXPECT_TRUE(http_request.options.recv_timeout_ms > 0);
            EXPECT_TRUE(http_request.options.recv_timeout_ms <= 1000);
            EXPECT_TRUE(http_request.options.total_timeout_ms > 0);
            EXPECT_TRUE(http_request.options.total_timeout_ms <= 1000);
            return OpenAICompatibleOkResult();
        },
        base_options);

    sylar::ai_gateway::GatewayChatRequest request;
    request.model = "general-chat";
    request.messages.push_back({"user", "hello"});

    sylar::ai_gateway::RequestExecutionBudget budget(1000, 1, "req-r3");
    sylar::ai_gateway::ProviderAttemptExecutor executor(router, handler);
    auto result = executor.execute(request, &budget);

    EXPECT_TRUE(result != nullptr);
    EXPECT_EQ(result->result, (int)sylar::http::HttpResult::Error::OK);
    EXPECT_EQ(call_count, 1);

    unsetenv("SYLAR_TEST_PROVIDER_KEY");
}

void test_provider_status_classification_controls_failover()
{
    sylar::ai_gateway::LogicalModelRouter router;
    std::string error;
    EXPECT_TRUE(router.addCandidate(
        MakeCandidate("provider-a", "general-chat", "https://api-a.example",
                      "/v1/chat/completions", "upstream-a", "PROVIDER_A_KEY"),
        &error));
    EXPECT_TRUE(router.addCandidate(
        MakeCandidate("provider-b", "general-chat", "https://api-b.example",
                      "/v1/chat/completions", "upstream-b", "PROVIDER_B_KEY"),
        &error));

    sylar::ai_gateway::GatewayChatRequest request;
    request.model = "general-chat";
    request.messages.push_back({"user", "hello"});

    int bad_request_calls = 0;
    sylar::ai_gateway::ProviderAttemptExecutor bad_request_executor(
        router,
        [&bad_request_calls](const sylar::ai_gateway::ProviderCandidate &,
                             const sylar::ai_gateway::GatewayChatRequest &,
                             sylar::ai_gateway::RequestExecutionBudget *) {
            ++bad_request_calls;
            return ProviderStatusError(sylar::http::HttpStatus::BAD_REQUEST);
        });
    auto bad_request_result = bad_request_executor.execute(request);
    EXPECT_TRUE(bad_request_result != nullptr);
    EXPECT_EQ(bad_request_calls, 1);

    int rate_limited_calls = 0;
    sylar::ai_gateway::ProviderAttemptExecutor rate_limited_executor(
        router,
        [&rate_limited_calls](const sylar::ai_gateway::ProviderCandidate &,
                              const sylar::ai_gateway::GatewayChatRequest &,
                              sylar::ai_gateway::RequestExecutionBudget *) {
            ++rate_limited_calls;
            if (rate_limited_calls == 1)
            {
                return ProviderStatusError(sylar::http::HttpStatus::TOO_MANY_REQUESTS);
            }
            return OkResult();
        });
    auto rate_limited_result = rate_limited_executor.execute(request);
    EXPECT_TRUE(rate_limited_result != nullptr);
    EXPECT_EQ(rate_limited_result->result, (int)sylar::http::HttpResult::Error::OK);
    EXPECT_EQ(rate_limited_calls, 2);

    int server_error_calls = 0;
    sylar::ai_gateway::ProviderAttemptExecutor server_error_executor(
        router,
        [&server_error_calls](const sylar::ai_gateway::ProviderCandidate &,
                              const sylar::ai_gateway::GatewayChatRequest &,
                              sylar::ai_gateway::RequestExecutionBudget *) {
            ++server_error_calls;
            if (server_error_calls == 1)
            {
                return ProviderStatusError(sylar::http::HttpStatus::INTERNAL_SERVER_ERROR);
            }
            return OkResult();
        });
    auto server_error_result = server_error_executor.execute(request);
    EXPECT_TRUE(server_error_result != nullptr);
    EXPECT_EQ(server_error_result->result, (int)sylar::http::HttpResult::Error::OK);
    EXPECT_EQ(server_error_calls, 2);
}

void test_unknown_submitted_result_stops_provider_failover()
{
    sylar::ai_gateway::LogicalModelRouter router;
    std::string error;
    EXPECT_TRUE(router.addCandidate(
        MakeCandidate("provider-a", "general-chat", "https://api-a.example",
                      "/v1/chat/completions", "upstream-a", "PROVIDER_A_KEY"),
        &error));
    EXPECT_TRUE(router.addCandidate(
        MakeCandidate("provider-b", "general-chat", "https://api-b.example",
                      "/v1/chat/completions", "upstream-b", "PROVIDER_B_KEY"),
        &error));

    int call_count = 0;
    sylar::ai_gateway::ProviderAttemptExecutor executor(
        router,
        [&call_count](const sylar::ai_gateway::ProviderCandidate &,
                      const sylar::ai_gateway::GatewayChatRequest &,
                      sylar::ai_gateway::RequestExecutionBudget *) {
            ++call_count;
            return ProviderUnknownSubmittedResult();
        });

    sylar::ai_gateway::GatewayChatRequest request;
    request.model = "general-chat";
    request.messages.push_back({"user", "hello"});

    auto result = executor.execute(request);
    EXPECT_TRUE(result != nullptr);
    EXPECT_EQ(call_count, 1);
    EXPECT_EQ((int)result->attempt.phase,
              (int)sylar::http::HttpAttemptPhase::RESPONSE_TIMEOUT_AFTER_SEND);
    EXPECT_TRUE(result->attempt.may_have_submitted);
}

void test_provider_error_classification_covers_r4_matrix()
{
    auto bad_request =
        sylar::ai_gateway::ClassifyProviderAttemptResult(
            ProviderStatusError(sylar::http::HttpStatus::BAD_REQUEST));
    EXPECT_EQ((int)bad_request.category,
              (int)sylar::ai_gateway::ProviderErrorCategory::CALLER_OR_MODEL_REJECTED);
    EXPECT_TRUE(!bad_request.try_next_candidate);

    auto unauthorized =
        sylar::ai_gateway::ClassifyProviderAttemptResult(
            ProviderStatusError(sylar::http::HttpStatus::UNAUTHORIZED));
    EXPECT_EQ((int)unauthorized.category,
              (int)sylar::ai_gateway::ProviderErrorCategory::AUTHORIZATION_FAILED);
    EXPECT_TRUE(!unauthorized.try_next_candidate);

    auto forbidden =
        sylar::ai_gateway::ClassifyProviderAttemptResult(
            ProviderStatusError(sylar::http::HttpStatus::FORBIDDEN));
    EXPECT_EQ((int)forbidden.category,
              (int)sylar::ai_gateway::ProviderErrorCategory::AUTHORIZATION_FAILED);
    EXPECT_TRUE(!forbidden.try_next_candidate);

    auto not_found =
        sylar::ai_gateway::ClassifyProviderAttemptResult(
            ProviderStatusError(sylar::http::HttpStatus::NOT_FOUND));
    EXPECT_EQ((int)not_found.category,
              (int)sylar::ai_gateway::ProviderErrorCategory::CALLER_OR_MODEL_REJECTED);
    EXPECT_TRUE(!not_found.try_next_candidate);

    auto unprocessable =
        sylar::ai_gateway::ClassifyProviderAttemptResult(
            ProviderStatusError(sylar::http::HttpStatus::UNPROCESSABLE_ENTITY));
    EXPECT_EQ((int)unprocessable.category,
              (int)sylar::ai_gateway::ProviderErrorCategory::CALLER_OR_MODEL_REJECTED);
    EXPECT_TRUE(!unprocessable.try_next_candidate);

    auto rate_limited =
        sylar::ai_gateway::ClassifyProviderAttemptResult(
            ProviderStatusError(sylar::http::HttpStatus::TOO_MANY_REQUESTS));
    EXPECT_EQ((int)rate_limited.category,
              (int)sylar::ai_gateway::ProviderErrorCategory::RATE_LIMITED);
    EXPECT_TRUE(rate_limited.try_next_candidate);
    EXPECT_TRUE(!rate_limited.breaker_failure);

    auto server_error =
        sylar::ai_gateway::ClassifyProviderAttemptResult(
            ProviderStatusError(sylar::http::HttpStatus::SERVICE_UNAVAILABLE));
    EXPECT_EQ((int)server_error.category,
              (int)sylar::ai_gateway::ProviderErrorCategory::RETRYABLE_UPSTREAM);
    EXPECT_TRUE(server_error.try_next_candidate);
    EXPECT_TRUE(server_error.breaker_failure);

    auto pre_send =
        sylar::ai_gateway::ClassifyProviderAttemptResult(ProviderSendFailedBeforeBodyResult());
    EXPECT_EQ((int)pre_send.category,
              (int)sylar::ai_gateway::ProviderErrorCategory::RETRYABLE_TRANSPORT);
    EXPECT_TRUE(pre_send.try_next_candidate);

    auto partial = sylar::ai_gateway::ClassifyProviderAttemptResult(ProviderPartialWriteResult());
    EXPECT_EQ((int)partial.category,
              (int)sylar::ai_gateway::ProviderErrorCategory::RESULT_UNKNOWN);
    EXPECT_TRUE(!partial.try_next_candidate);

    auto read_timeout =
        sylar::ai_gateway::ClassifyProviderAttemptResult(ProviderUnknownSubmittedResult());
    EXPECT_EQ((int)read_timeout.category,
              (int)sylar::ai_gateway::ProviderErrorCategory::RESULT_UNKNOWN);
    EXPECT_TRUE(!read_timeout.try_next_candidate);
}

void test_openai_adapter_returns_sanitized_provider_errors()
{
    setenv("SYLAR_TEST_PROVIDER_KEY", "sk-r4-secret", 1);

    sylar::ai_gateway::LogicalModelRouter router;
    std::string error;
    EXPECT_TRUE(router.addCandidate(
        MakeCandidate("provider-a", "general-chat", "https://api-a.example",
                      "/v1/chat/completions", "upstream-a", "SYLAR_TEST_PROVIDER_KEY"),
        &error));

    auto handler = sylar::ai_gateway::CreateOpenAICompatibleAttemptHandler(
        [](const sylar::ai_gateway::ProviderCandidate &,
           const sylar::ai_gateway::ProviderHttpRequest &) {
            return ProviderStatusError(sylar::http::HttpStatus::UNAUTHORIZED,
                                       "{\"message\":\"bad key sk-r4-secret hello from user\"}");
        });

    sylar::ai_gateway::GatewayChatRequest request;
    request.model = "general-chat";
    request.messages.push_back({"user", "hello from user"});

    sylar::ai_gateway::ProviderAttemptExecutor executor(router, handler);
    auto result = executor.execute(request);
    EXPECT_TRUE(result != nullptr);
    EXPECT_EQ(result->result, (int)sylar::http::HttpResult::Error::HTTP_STATUS_ERROR);
    EXPECT_TRUE(result->response != nullptr);
    EXPECT_EQ((int)result->response->getStatus(), (int)sylar::http::HttpStatus::BAD_GATEWAY);
    EXPECT_EQ(result->attempt.http_status, (int)sylar::http::HttpStatus::UNAUTHORIZED);

    Json::Value body = ParseJson(result->response->getBody());
    EXPECT_EQ(body["error"]["code"].asString(), "PROVIDER_AUTH_FAILED");
    EXPECT_EQ(result->response->getBody().find("sk-r4-secret"), std::string::npos);
    EXPECT_EQ(result->response->getBody().find("hello from user"), std::string::npos);

    unsetenv("SYLAR_TEST_PROVIDER_KEY");
}

} // namespace

int main()
{
    test_gateway_protocol_names_are_available();
    test_router_keeps_only_compatible_candidates_in_model_pool();
    test_provider_aware_executor_preserves_candidate_specific_request_shape();
    test_openai_adapter_builds_request_and_parses_response_via_executor();
    test_openai_adapter_sanitizes_parse_error();
    test_request_execution_budget_limits_provider_failover_attempts();
    test_openai_adapter_applies_remaining_budget_to_http_options();
    test_provider_status_classification_controls_failover();
    test_unknown_submitted_result_stops_provider_failover();
    test_provider_error_classification_covers_r4_matrix();
    test_openai_adapter_returns_sanitized_provider_errors();
    return g_failures == 0 ? 0 : 1;
}
