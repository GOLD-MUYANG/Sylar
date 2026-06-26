#include "modules/ai_gateway/ai_gateway_protocol.h"
#include "modules/ai_gateway/ai_gateway_servlet.h"
#include "modules/ai_gateway/ai_gateway_upstream.h"
#include "sylar/hook.h"
#include "sylar/http/http_connection.h"
#include "sylar/iomanager.h"
#include "sylar/log.h"

#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <json/json.h>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
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
        if ((lhs) != (rhs))                                                                     \
        {                                                                                       \
            ++g_failures;                                                                       \
            SYLAR_LOG_ERROR(g_logger) << "EXPECT_EQ failed: " #lhs << " line=" << __LINE__;   \
        }                                                                                       \
    } while (0)

namespace
{

class MockProviderServer
{
public:
    MockProviderServer(const std::string &name,
                       const std::string &content,
                       bool fail = false,
                       uint64_t delay_ms = 0)
        : m_name(name), m_content(content), m_fail(fail), m_delayMs(delay_ms)
    {
        m_fd = socket_f(AF_INET, SOCK_STREAM, 0);
        EXPECT_TRUE(m_fd >= 0);

        int reuse = 1;
        setsockopt_f(m_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in address;
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        EXPECT_EQ(::bind(m_fd, (const sockaddr *)&address, sizeof(address)), 0);
        EXPECT_EQ(::listen(m_fd, 16), 0);

        socklen_t length = sizeof(address);
        EXPECT_EQ(::getsockname(m_fd, (sockaddr *)&address, &length), 0);
        m_port = ntohs(address.sin_port);
        m_thread = std::thread(&MockProviderServer::acceptLoop, this);
    }

    ~MockProviderServer()
    {
        m_stop = true;
        shutdown(m_fd, SHUT_RDWR);
        close_f(m_fd);
        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }

    uint16_t getPort() const
    {
        return m_port;
    }

    uint32_t getRequestCount() const
    {
        return m_requestCount.load();
    }

private:
    void acceptLoop()
    {
        while (!m_stop)
        {
            int client = accept_f(m_fd, nullptr, nullptr);
            if (client < 0)
            {
                continue;
            }
            std::thread(&MockProviderServer::handleClient, this, client).detach();
        }
    }

    void handleClient(int client)
    {
        char buffer[1024];
        std::string request;
        while (request.find("\r\n\r\n") == std::string::npos)
        {
            int rt = recv_f(client, buffer, sizeof(buffer), 0);
            if (rt <= 0)
            {
                close_f(client);
                return;
            }
            request.append(buffer, rt);
        }

        ++m_requestCount;
        if (m_delayMs > 0)
        {
            usleep(m_delayMs * 1000);
        }
        const std::string body = m_fail
                                     ? sylar::ai_gateway::BuildErrorResponse(
                                           "mock failure", "server_error", "MOCK_PROVIDER_ERROR")
                                     : sylar::ai_gateway::BuildMockProviderResponse(m_name, m_content);
        std::stringstream response;
        response << "HTTP/1.1 " << (m_fail ? "503 Service Unavailable" : "200 OK") << "\r\n"
                 << "Connection: close\r\nContent-Type: application/json\r\nContent-Length: "
                 << body.size() << "\r\n\r\n"
                 << body;
        const std::string wire = response.str();
        send_f(client, wire.data(), wire.size(), 0);
        close_f(client);
    }

private:
    std::string m_name;
    std::string m_content;
    bool m_fail = false;
    uint64_t m_delayMs = 0;
    int m_fd = -1;
    uint16_t m_port = 0;
    std::atomic<bool> m_stop{false};
    std::atomic<uint32_t> m_requestCount{0};
    std::thread m_thread;
};

sylar::http::HttpRequest::ptr MakeRequest()
{
    sylar::http::HttpRequest::ptr request(new sylar::http::HttpRequest);
    request->setMethod(sylar::http::HttpMethod::POST);
    request->setPath("/v1/chat/completions");
    request->setBody(R"({"model":"demo-chat","messages":[{"role":"user","content":"hello"}]})");
    return request;
}

Json::Value HandleCompletion(sylar::ai_gateway::AiGatewayServlet &servlet)
{
    sylar::http::HttpResponse::ptr response(new sylar::http::HttpResponse);
    EXPECT_EQ(servlet.handle(MakeRequest(), response, nullptr), 0);
    EXPECT_EQ(response->getStatus(), sylar::http::HttpStatus::OK);

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    EXPECT_TRUE(reader->parse(response->getBody().data(),
                              response->getBody().data() + response->getBody().size(), &root,
                              &errors));
    return root;
}

sylar::http::HttpLoadBalanceClient::ptr MakeClient(MockProviderServer &first,
                                                   MockProviderServer &second,
                                                   const std::string &strategy = "ROUND_ROBIN",
                                                   const sylar::ai_gateway::AiGatewayUpstreamOptions
                                                       &options = sylar::ai_gateway::
                                                           AiGatewayUpstreamOptions())
{
    std::vector<sylar::ai_gateway::AiGatewayProviderConfig> providers;
    sylar::ai_gateway::AiGatewayProviderConfig first_config;
    first_config.name = "mock-a";
    first_config.url = "http://127.0.0.1:" + std::to_string(first.getPort());
    providers.push_back(first_config);
    sylar::ai_gateway::AiGatewayProviderConfig second_config;
    second_config.name = "mock-b";
    second_config.url = "http://127.0.0.1:" + std::to_string(second.getPort());
    providers.push_back(second_config);

    std::string error;
    auto client = sylar::ai_gateway::CreateLoadBalanceClient(providers, strategy, options, &error);
    EXPECT_TRUE(client != nullptr);
    return client;
}

std::shared_ptr<sylar::ai_gateway::AiGatewayServlet>
MakeServlet(sylar::http::HttpLoadBalanceClient::ptr client)
{
    return std::make_shared<sylar::ai_gateway::AiGatewayServlet>(
        [client](const std::string &body, sylar::http::HttpLoadBalanceRequestTrace *trace) {
            sylar::http::HttpRetryOptions retry_options;
            retry_options.retry_non_idempotent = true;
            return client->post("/v1/chat/completions",
                                sylar::http::HttpRequestOptions::FromTimeout(500), retry_options,
                                {{"Content-Type", "application/json"}}, body, trace);
        });
}

void test_round_robin_maps_two_providers()
{
    MockProviderServer first("mock-a", "from-a");
    MockProviderServer second("mock-b", "from-b");
    usleep(50 * 1000);
    auto client = MakeClient(first, second);
    if (!client)
    {
        return;
    }
    auto servlet = MakeServlet(client);

    EXPECT_EQ(HandleCompletion(*servlet)["choices"][0]["message"]["content"].asString(), "from-a");
    EXPECT_EQ(HandleCompletion(*servlet)["choices"][0]["message"]["content"].asString(), "from-b");
    EXPECT_EQ(first.getRequestCount(), 1U);
    EXPECT_EQ(second.getRequestCount(), 1U);
}

void test_failed_provider_fails_over_without_leaking_provider()
{
    MockProviderServer failed("mock-a", "", true);
    MockProviderServer available("mock-b", "from-b");
    usleep(50 * 1000);
    auto client = MakeClient(failed, available);
    if (!client)
    {
        return;
    }
    auto servlet = MakeServlet(client);
    Json::Value root = HandleCompletion(*servlet);
    Json::StreamWriterBuilder builder;
    const std::string body = Json::writeString(builder, root);

    EXPECT_EQ(root["choices"][0]["message"]["content"].asString(), "from-b");
    EXPECT_TRUE(body.find("mock-a") == std::string::npos);
    EXPECT_TRUE(body.find("127.0.0.1") == std::string::npos);
    EXPECT_EQ(failed.getRequestCount(), 1U);
    EXPECT_EQ(available.getRequestCount(), 1U);
}

void test_invalid_provider_scheme_is_rejected()
{
    std::vector<sylar::ai_gateway::AiGatewayProviderConfig> providers;
    sylar::ai_gateway::AiGatewayProviderConfig provider;
    provider.name = "bad";
    provider.url = "ftp://127.0.0.1:1";
    providers.push_back(provider);
    std::string error;
    EXPECT_TRUE(!sylar::ai_gateway::CreateLoadBalanceClient(providers, "ROUND_ROBIN", &error));
    EXPECT_TRUE(!error.empty());
}

void test_endpoint_qps_limit_fails_over_to_available_provider()
{
    MockProviderServer first("mock-a", "from-a");
    MockProviderServer available("mock-b", "from-b");
    usleep(50 * 1000);

    sylar::ai_gateway::AiGatewayUpstreamOptions options;
    options.limiter.max_endpoint_qps = 1;
    auto client = MakeClient(first, available, "WEIGHTED_ROUND_ROBIN", options);
    if (!client)
    {
        return;
    }
    auto servlet = MakeServlet(client);

    EXPECT_EQ(HandleCompletion(*servlet)["choices"][0]["message"]["content"].asString(), "from-a");
    Json::Value root = HandleCompletion(*servlet);
    EXPECT_EQ(root["choices"][0]["message"]["content"].asString(), "from-b");

    EXPECT_EQ(first.getRequestCount(), 1U);
    EXPECT_EQ(available.getRequestCount(), 1U);
}

void test_all_endpoint_qps_limited_returns_rate_limited()
{
    MockProviderServer provider_server("mock-a", "from-a");
    usleep(50 * 1000);

    std::vector<sylar::ai_gateway::AiGatewayProviderConfig> providers;
    sylar::ai_gateway::AiGatewayProviderConfig provider;
    provider.name = "mock-a";
    provider.url = "http://127.0.0.1:" + std::to_string(provider_server.getPort());
    providers.push_back(provider);

    sylar::ai_gateway::AiGatewayUpstreamOptions options;
    options.limiter.max_endpoint_qps = 1;
    std::string error;
    auto client =
        sylar::ai_gateway::CreateLoadBalanceClient(providers, "ROUND_ROBIN", options, &error);
    EXPECT_TRUE(client != nullptr);
    if (!client)
    {
        return;
    }

    auto servlet = MakeServlet(client);
    EXPECT_EQ(HandleCompletion(*servlet)["choices"][0]["message"]["content"].asString(), "from-a");

    sylar::http::HttpResponse::ptr response(new sylar::http::HttpResponse);
    EXPECT_EQ(servlet->handle(MakeRequest(), response, nullptr), 0);
    EXPECT_EQ(response->getStatus(), sylar::http::HttpStatus::TOO_MANY_REQUESTS);
    EXPECT_TRUE(response->getBody().find("RATE_LIMITED") != std::string::npos);

    EXPECT_EQ(provider_server.getRequestCount(), 1U);
}

void test_circuit_breaker_skips_open_provider()
{
    MockProviderServer failed("mock-a", "", true);
    MockProviderServer available("mock-b", "from-b");
    usleep(50 * 1000);

    sylar::ai_gateway::AiGatewayUpstreamOptions options;
    options.circuit_breaker.enabled = true;
    options.circuit_breaker.consecutive_failure_threshold = 1;
    options.circuit_breaker.failure_rate_threshold = 0;
    auto client = MakeClient(failed, available, "WEIGHTED_ROUND_ROBIN", options);
    if (!client)
    {
        return;
    }
    auto servlet = MakeServlet(client);

    EXPECT_EQ(HandleCompletion(*servlet)["choices"][0]["message"]["content"].asString(), "from-b");
    EXPECT_EQ(HandleCompletion(*servlet)["choices"][0]["message"]["content"].asString(), "from-b");
    EXPECT_EQ(failed.getRequestCount(), 1U);
    EXPECT_EQ(available.getRequestCount(), 2U);
}

void test_max_total_attempts_stops_before_next_provider()
{
    MockProviderServer failed("mock-a", "", true);
    MockProviderServer available("mock-b", "from-b");
    usleep(50 * 1000);

    auto client = MakeClient(failed, available);
    if (!client)
    {
        return;
    }

    auto servlet = std::make_shared<sylar::ai_gateway::AiGatewayServlet>(
        [client](const std::string &body, sylar::http::HttpLoadBalanceRequestTrace *trace) {
            sylar::http::HttpRetryOptions retry_options;
            retry_options.retry_non_idempotent = true;
            retry_options.max_total_attempts = 1;
            return client->post("/v1/chat/completions",
                                sylar::http::HttpRequestOptions::FromTimeout(500), retry_options,
                                {{"Content-Type", "application/json"}}, body, trace);
        });

    sylar::http::HttpResponse::ptr response(new sylar::http::HttpResponse);
    EXPECT_EQ(servlet->handle(MakeRequest(), response, nullptr), 0);
    EXPECT_EQ(response->getStatus(), sylar::http::HttpStatus::BAD_GATEWAY);
    EXPECT_EQ(failed.getRequestCount(), 1U);
    EXPECT_EQ(available.getRequestCount(), 0U);
}

void test_health_check_marks_failed_provider_down()
{
    MockProviderServer failed("mock-a", "", true);
    MockProviderServer available("mock-b", "from-b");
    usleep(50 * 1000);

    auto client = MakeClient(failed, available);
    if (!client)
    {
        return;
    }

    EXPECT_EQ(client->checkHealth("/", sylar::http::HttpRequestOptions::FromTimeout(500)), 1U);
    auto servlet = MakeServlet(client);
    EXPECT_EQ(HandleCompletion(*servlet)["choices"][0]["message"]["content"].asString(), "from-b");
}

void RunTests()
{
    test_round_robin_maps_two_providers();
    test_failed_provider_fails_over_without_leaking_provider();
    test_invalid_provider_scheme_is_rejected();
    test_endpoint_qps_limit_fails_over_to_available_provider();
    test_all_endpoint_qps_limited_returns_rate_limited();
    test_circuit_breaker_skips_open_provider();
    test_max_total_attempts_stops_before_next_provider();
    test_health_check_marks_failed_provider_down();
}

} // namespace

int main()
{
    sylar::IOManager iom(1, false, "ai-gateway-load-balance-test");
    iom.schedule(RunTests);
    iom.stop();
    return g_failures == 0 ? 0 : 1;
}
