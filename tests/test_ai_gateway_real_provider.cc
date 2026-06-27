#include "modules/ai_gateway/ai_gateway_protocol.h"
#include "modules/ai_gateway/real_provider_gateway.h"
#include "sylar/http/http_connection.h"
#include "sylar/log.h"

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

} // namespace

int main()
{
    test_gateway_protocol_names_are_available();
    test_router_keeps_only_compatible_candidates_in_model_pool();
    test_provider_aware_executor_preserves_candidate_specific_request_shape();
    return g_failures == 0 ? 0 : 1;
}
