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

void test_valid_request_maps_provider_response()
{
    sylar::ai_gateway::AiGatewayServlet servlet([](const std::string &) { return MakeSuccessResult(); });
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
    sylar::ai_gateway::AiGatewayServlet servlet([&called](const std::string &) {
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
    sylar::ai_gateway::AiGatewayServlet servlet([](const std::string &) {
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
    sylar::ai_gateway::AiGatewayServlet servlet([](const std::string &) {
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

} // namespace

int main()
{
    test_valid_request_maps_provider_response();
    test_invalid_request_does_not_call_upstream();
    test_upstream_failure_hides_internal_error();
    test_rate_limited_upstream_maps_to_429();
    return g_failures == 0 ? 0 : 1;
}
