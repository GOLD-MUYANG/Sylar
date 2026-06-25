#include "modules/ai_gateway/ai_gateway_status_servlet.h"
#include "modules/ai_gateway/ai_gateway_upstream.h"
#include "sylar/http/http_connection.h"
#include "sylar/log.h"

#include <json/json.h>

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
        if ((lhs) != (rhs))                                                                     \
        {                                                                                       \
            ++g_failures;                                                                       \
            SYLAR_LOG_ERROR(g_logger) << "EXPECT_EQ failed: " #lhs << " line=" << __LINE__;   \
        }                                                                                       \
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

sylar::http::HttpEndpoint::ptr MakeEndpoint(uint16_t port)
{
    return sylar::http::HttpEndpoint::Create("127.0.0.1", port);
}

void test_status_snapshot_reports_provider_fields()
{
    std::vector<sylar::http::HttpEndpoint::ptr> endpoints;
    endpoints.push_back(MakeEndpoint(19001));
    endpoints.push_back(MakeEndpoint(19002));

    sylar::http::HttpConcurrencyLimitOptions limits;
    sylar::http::HttpCircuitBreakerOptions breaker_options;
    breaker_options.enabled = true;

    auto client = sylar::http::HttpLoadBalanceClient::Create(
        endpoints,
        sylar::http::HttpLoadBalanceStrategy::ROUND_ROBIN,
        limits,
        breaker_options);
    EXPECT_TRUE(client != nullptr);
    if (!client)
    {
        return;
    }

    std::vector<sylar::ai_gateway::AiGatewayProviderConfig> providers;
    sylar::ai_gateway::AiGatewayProviderConfig first;
    first.name = "mock-a";
    first.url = "http://127.0.0.1:19001";
    providers.push_back(first);
    sylar::ai_gateway::AiGatewayProviderConfig second;
    second.name = "mock-b";
    second.url = "http://127.0.0.1:19002";
    providers.push_back(second);

    sylar::ai_gateway::AiGatewayStatusServlet servlet(providers, client);
    sylar::http::HttpRequest::ptr request(new sylar::http::HttpRequest);
    request->setMethod(sylar::http::HttpMethod::GET);
    request->setPath("/internal/status");
    sylar::http::HttpResponse::ptr response(new sylar::http::HttpResponse);

    EXPECT_EQ(servlet.handle(request, response, nullptr), 0);
    EXPECT_EQ(response->getStatus(), sylar::http::HttpStatus::OK);
    EXPECT_EQ(response->getHeader("Content-Type"), "application/json");

    Json::Value root;
    EXPECT_TRUE(ParseJson(response->getBody(), &root));
    EXPECT_EQ(root["object"].asString(), "ai_gateway.status");
    EXPECT_EQ(root["providers"].size(), 2U);

    const Json::Value first_provider = root["providers"][0];
    EXPECT_EQ(first_provider["name"].asString(), "mock-a");
    EXPECT_EQ(first_provider["endpoint"].asString(), "127.0.0.1:19001");
    EXPECT_EQ(first_provider["health"].asString(), "UP");
    EXPECT_EQ(first_provider["circuit_breaker"].asString(), "CLOSED");
    EXPECT_TRUE(first_provider["in_flight"].isUInt());
    EXPECT_TRUE(first_provider["success_count"].isUInt64());
    EXPECT_TRUE(first_provider["failure_count"].isUInt64());
    EXPECT_TRUE(first_provider["rate_limited_count"].isUInt64());
    EXPECT_TRUE(first_provider["last_failure_reason"].isString());
}

} // namespace

int main()
{
    test_status_snapshot_reports_provider_fields();
    return g_failures == 0 ? 0 : 1;
}
