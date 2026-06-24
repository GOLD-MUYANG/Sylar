#include "modules/ai_gateway/ai_gateway_protocol.h"
#include "sylar/log.h"

#include <json/json.h>

static sylar::Logger::ptr g_logger = SYLAR_LOG_ROOT();
static int g_failures = 0;

#define EXPECT_TRUE(expr)                                                                          \
    do                                                                                             \
    {                                                                                              \
        if (!(expr))                                                                               \
        {                                                                                          \
            ++g_failures;                                                                          \
            SYLAR_LOG_ERROR(g_logger) << "EXPECT_TRUE failed: " #expr << " line=" << __LINE__;  \
        }                                                                                          \
    } while (0)

#define EXPECT_EQ(lhs, rhs)                                                                        \
    do                                                                                             \
    {                                                                                              \
        if ((lhs) != (rhs))                                                                        \
        {                                                                                          \
            ++g_failures;                                                                          \
            SYLAR_LOG_ERROR(g_logger) << "EXPECT_EQ failed: " #lhs << " line=" << __LINE__;    \
        }                                                                                          \
    } while (0)

namespace
{

bool ParseJson(const std::string &body, Json::Value *value)
{
    Json::CharReaderBuilder builder;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    return reader->parse(body.data(), body.data() + body.size(), value, &errors);
}

void test_parse_valid_request()
{
    const std::string body = R"({
        "model": "demo-chat",
        "messages": [
            {"role": "system", "content": "be concise"},
            {"role": "assistant", "content": "hello"},
            {"role": "user", "content": "explain breaker"}
        ],
        "temperature": 0.2,
        "max_tokens": 128,
        "stream": false
    })";

    sylar::ai_gateway::ChatCompletionRequest request;
    std::string error;
    EXPECT_TRUE(sylar::ai_gateway::ParseChatCompletionRequest(body, &request, &error));
    EXPECT_EQ(request.model, "demo-chat");
    EXPECT_EQ(request.messages.size(), (size_t)3);
    EXPECT_EQ(request.messages[2].content, "explain breaker");
}

void test_reject_invalid_request()
{
    const char *invalid_bodies[] = {
        R"({"messages": []})",
        R"({"model": "demo-chat", "messages": {}})",
        R"({"model": "demo-chat", "messages": [{"role":"tool","content":"x"}]})",
        R"({"model": "demo-chat", "messages": [{"role":"user","content":1}]})",
        R"({"model": "demo-chat", "messages": [], "stream":true})",
    };

    for (auto body : invalid_bodies)
    {
        sylar::ai_gateway::ChatCompletionRequest request;
        std::string error;
        EXPECT_TRUE(!sylar::ai_gateway::ParseChatCompletionRequest(body, &request, &error));
        EXPECT_TRUE(!error.empty());
    }
}

void test_build_completion_and_error_response()
{
    sylar::ai_gateway::ChatCompletionRequest request;
    request.model = "demo-chat";
    request.messages.push_back({"user", "hello"});

    Json::Value completion;
    EXPECT_TRUE(ParseJson(sylar::ai_gateway::BuildChatCompletionResponse(
                              request, "chatcmpl-local-7", 1710000000, "mock-a: hello"),
                          &completion));
    EXPECT_EQ(completion["id"].asString(), "chatcmpl-local-7");
    EXPECT_EQ(completion["object"].asString(), "chat.completion");
    EXPECT_EQ(completion["created"].asInt64(), (Json::Int64)1710000000);
    EXPECT_EQ(completion["model"].asString(), "demo-chat");
    EXPECT_EQ(completion["choices"][0]["message"]["role"].asString(), "assistant");
    EXPECT_EQ(completion["choices"][0]["message"]["content"].asString(), "mock-a: hello");
    EXPECT_EQ(completion["usage"]["total_tokens"].asInt(), 0);

    Json::Value error;
    EXPECT_TRUE(ParseJson(sylar::ai_gateway::BuildErrorResponse(
                              "请求参数不合法", "invalid_request_error", "INVALID_REQUEST"),
                          &error));
    EXPECT_EQ(error["error"]["type"].asString(), "invalid_request_error");
    EXPECT_EQ(error["error"]["code"].asString(), "INVALID_REQUEST");
}

void test_mock_provider_protocol()
{
    Json::Value provider_response;
    EXPECT_TRUE(ParseJson(
        sylar::ai_gateway::BuildMockProviderResponse("mock-a", "mock-a: hello"),
        &provider_response));
    EXPECT_EQ(provider_response["provider"].asString(), "mock-a");
    EXPECT_EQ(provider_response["content"].asString(), "mock-a: hello");

    std::string provider;
    std::string content;
    std::string error;
    EXPECT_TRUE(sylar::ai_gateway::ParseMockProviderResponse(
        provider_response.toStyledString(), &provider, &content, &error));
    EXPECT_EQ(provider, "mock-a");
    EXPECT_EQ(content, "mock-a: hello");
}

} // namespace

int main()
{
    test_parse_valid_request();
    test_reject_invalid_request();
    test_build_completion_and_error_response();
    test_mock_provider_protocol();
    return g_failures == 0 ? 0 : 1;
}
