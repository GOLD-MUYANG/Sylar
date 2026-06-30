#include "modules/ai_gateway/ai_gateway_servlet.h"
#include "modules/ai_gateway/ai_gateway_protocol.h"
#include "sylar/http/http_connection.h"
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

sylar::http::HttpRequest::ptr MakeRequest(const std::string &body)
{
    sylar::http::HttpRequest::ptr request(new sylar::http::HttpRequest);
    request->setMethod(sylar::http::HttpMethod::POST);
    request->setPath("/v1/chat/completions");
    request->setBody(body);
    return request;
}

sylar::http::HttpResult::ptr MakeSuccessResult()
{
    sylar::http::HttpResponse::ptr response(new sylar::http::HttpResponse);
    response->setStatus(sylar::http::HttpStatus::OK);
    response->setBody(
        sylar::ai_gateway::BuildMockProviderResponse("mock-a", "mock completion"));
    return std::make_shared<sylar::http::HttpResult>(
        (int)sylar::http::HttpResult::Error::OK, response, "");
}

sylar::http::HttpResult::ptr MakeTracedSuccessResult(sylar::http::HttpLoadBalanceRequestTrace *trace)
{
    if (trace)
    {
        sylar::http::HttpLoadBalanceAttemptTrace first;
        first.endpoint_key = "127.0.0.1:19003";
        first.outcome = "failure";
        first.result = (int)sylar::http::HttpResult::Error::CONNECT_FAIL;
        first.reason = "connect failed";
        trace->attempts.push_back(first);

        sylar::http::HttpLoadBalanceAttemptTrace second;
        second.endpoint_key = "127.0.0.1:19001";
        second.outcome = "success";
        second.result = (int)sylar::http::HttpResult::Error::OK;
        second.http_status = 200;
        trace->attempts.push_back(second);
    }
    return MakeSuccessResult();
}

void test_valid_request_maps_provider_response()
{
    sylar::ai_gateway::AiGatewayServlet servlet(
        [](const std::string &, sylar::http::HttpLoadBalanceRequestTrace *) {
            return MakeSuccessResult();
        });
    auto request = MakeRequest(
        R"({"model":"demo-chat","messages":[{"role":"user","content":"hello"}]})");
    sylar::http::HttpResponse::ptr response(new sylar::http::HttpResponse);

    EXPECT_EQ(servlet.handle(request, response, nullptr), 0);
    EXPECT_EQ(response->getStatus(), sylar::http::HttpStatus::OK);

    Json::Value root;
    EXPECT_TRUE(ParseJson(response->getBody(), &root));
    EXPECT_EQ(root["object"].asString(), "chat.completion");
    EXPECT_EQ(root["model"].asString(), "demo-chat");
    EXPECT_EQ(root["choices"][0]["message"]["content"].asString(), "mock completion");
}

void test_invalid_request_does_not_call_upstream()
{
    bool called = false;
    sylar::ai_gateway::AiGatewayServlet servlet(
        [&called](const std::string &, sylar::http::HttpLoadBalanceRequestTrace *) {
            called = true;
            return MakeSuccessResult();
        });
    auto request = MakeRequest(R"({"model":"demo-chat","messages":[],"stream":true})");
    sylar::http::HttpResponse::ptr response(new sylar::http::HttpResponse);

    EXPECT_EQ(servlet.handle(request, response, nullptr), 0);
    EXPECT_TRUE(!called);
    EXPECT_EQ(response->getStatus(), sylar::http::HttpStatus::BAD_REQUEST);

    Json::Value root;
    EXPECT_TRUE(ParseJson(response->getBody(), &root));
    EXPECT_EQ(root["error"]["code"].asString(), "INVALID_REQUEST");
}

void test_upstream_failure_hides_internal_error()
{
    sylar::ai_gateway::AiGatewayServlet servlet(
        [](const std::string &, sylar::http::HttpLoadBalanceRequestTrace *) {
            return std::make_shared<sylar::http::HttpResult>(
                (int)sylar::http::HttpResult::Error::CONNECT_FAIL, nullptr,
                "connect http://127.0.0.1:19001 failed");
        });
    auto request = MakeRequest(
        R"({"model":"demo-chat","messages":[{"role":"user","content":"hello"}]})");
    sylar::http::HttpResponse::ptr response(new sylar::http::HttpResponse);

    EXPECT_EQ(servlet.handle(request, response, nullptr), 0);
    EXPECT_EQ(response->getStatus(), sylar::http::HttpStatus::BAD_GATEWAY);
    EXPECT_TRUE(response->getBody().find("127.0.0.1") == std::string::npos);

    Json::Value root;
    EXPECT_TRUE(ParseJson(response->getBody(), &root));
    EXPECT_EQ(root["error"]["code"].asString(), "UPSTREAM_UNAVAILABLE");
}

void test_rate_limited_upstream_maps_to_429()
{
    sylar::ai_gateway::AiGatewayServlet servlet(
        [](const std::string &, sylar::http::HttpLoadBalanceRequestTrace *) {
            return std::make_shared<sylar::http::HttpResult>(
                (int)sylar::http::HttpResult::Error::RATE_LIMITED, nullptr,
                "http client concurrency limited");
        });
    auto request = MakeRequest(
        R"({"model":"demo-chat","messages":[{"role":"user","content":"hello"}]})");
    sylar::http::HttpResponse::ptr response(new sylar::http::HttpResponse);

    EXPECT_EQ(servlet.handle(request, response, nullptr), 0);
    EXPECT_EQ(response->getStatus(), sylar::http::HttpStatus::TOO_MANY_REQUESTS);
    EXPECT_TRUE(response->getBody().find("concurrency") == std::string::npos);

    Json::Value root;
    EXPECT_TRUE(ParseJson(response->getBody(), &root));
    EXPECT_EQ(root["error"]["code"].asString(), "RATE_LIMITED");
}

void test_demo_trace_header_is_exposed_when_enabled_and_requested()
{
    sylar::ai_gateway::AiGatewayServlet servlet(
        [](const std::string &, sylar::http::HttpLoadBalanceRequestTrace *trace) {
            return MakeTracedSuccessResult(trace);
        },
        true);
    auto request = MakeRequest(
        R"({"model":"demo-chat","messages":[{"role":"user","content":"hello"}]})");
    request->setHeader("X-Ai-Gateway-Demo-Trace", "1");
    sylar::http::HttpResponse::ptr response(new sylar::http::HttpResponse);

    EXPECT_EQ(servlet.handle(request, response, nullptr), 0);
    EXPECT_EQ(response->getStatus(), sylar::http::HttpStatus::OK);

    Json::Value trace_root;
    EXPECT_TRUE(ParseJson(response->getHeader("X-Ai-Gateway-Trace"), &trace_root));
    EXPECT_EQ(trace_root["attempts"].size(), 2U);
    EXPECT_EQ(trace_root["attempts"][0]["endpoint"].asString(), "127.0.0.1:19003");
    EXPECT_EQ(trace_root["attempts"][0]["outcome"].asString(), "failure");
    EXPECT_EQ(trace_root["attempts"][1]["outcome"].asString(), "success");
}

void test_real_provider_upstream_returns_compatible_response_and_trace()
{
    sylar::ai_gateway::AiGatewayServlet servlet(
        [](const sylar::ai_gateway::ChatCompletionRequest &completion_request,
           Json::Value *trace) {
            if (trace)
            {
                (*trace)["object"] = "ai_gateway.real_provider.trace";
                (*trace)["events"] = Json::Value(Json::arrayValue);
                Json::Value event;
                event["stage"] = "request_received";
                event["model"] = completion_request.model;
                (*trace)["events"].append(event);
                Json::Value attempt;
                attempt["stage"] = "attempt_result";
                attempt["provider"] = "provider-a";
                attempt["try_next_candidate"] = false;
                (*trace)["events"].append(attempt);
            }

            sylar::http::HttpResponse::ptr upstream_response(new sylar::http::HttpResponse);
            upstream_response->setStatus(sylar::http::HttpStatus::OK);
            upstream_response->setHeader("Content-Type", "application/json");
            upstream_response->setBody(sylar::ai_gateway::BuildChatCompletionResponse(
                completion_request, "chatcmpl-real-test", 123, "real provider answer"));
            return std::make_shared<sylar::http::HttpResult>(
                (int)sylar::http::HttpResult::Error::OK, upstream_response, "ok");
        },
        true);
    auto request = MakeRequest(
        R"({"model":"general-chat","messages":[{"role":"user","content":"do not leak me"}]})");
    request->setHeader("X-Ai-Gateway-Demo-Trace", "1");
    sylar::http::HttpResponse::ptr response(new sylar::http::HttpResponse);

    EXPECT_EQ(servlet.handle(request, response, nullptr), 0);
    EXPECT_EQ(response->getStatus(), sylar::http::HttpStatus::OK);

    Json::Value body;
    EXPECT_TRUE(ParseJson(response->getBody(), &body));
    EXPECT_EQ(body["model"].asString(), "general-chat");
    EXPECT_EQ(body["choices"][0]["message"]["content"].asString(), "real provider answer");

    const std::string trace_header = response->getHeader("X-Ai-Gateway-Trace");
    Json::Value trace_root;
    EXPECT_TRUE(ParseJson(trace_header, &trace_root));
    EXPECT_EQ(trace_root["object"].asString(), "ai_gateway.real_provider.trace");
    EXPECT_EQ(trace_root["events"][0]["stage"].asString(), "request_received");
    EXPECT_TRUE(trace_header.find("do not leak me") == std::string::npos);
}

} // namespace

int main()
{
    test_valid_request_maps_provider_response();
    test_invalid_request_does_not_call_upstream();
    test_upstream_failure_hides_internal_error();
    test_rate_limited_upstream_maps_to_429();
    test_demo_trace_header_is_exposed_when_enabled_and_requested();
    test_real_provider_upstream_returns_compatible_response_and_trace();
    return g_failures == 0 ? 0 : 1;
}
