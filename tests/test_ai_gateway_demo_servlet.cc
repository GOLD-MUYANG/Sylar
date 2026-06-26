#include "modules/ai_gateway/ai_gateway_demo_servlet.h"
#include "sylar/http/http_connection.h"
#include "sylar/log.h"

static sylar::Logger::ptr g_logger = SYLAR_LOG_ROOT();
static int g_failures = 0;

#define EXPECT_TRUE(expr)                                                                          \
    do                                                                                             \
    {                                                                                              \
        if (!(expr))                                                                               \
        {                                                                                          \
            ++g_failures;                                                                          \
            SYLAR_LOG_ERROR(g_logger) << "EXPECT_TRUE failed: " #expr << " line=" << __LINE__;   \
        }                                                                                          \
    } while (0)

#define EXPECT_EQ(lhs, rhs)                                                                        \
    do                                                                                             \
    {                                                                                              \
        if ((lhs) != (rhs))                                                                        \
        {                                                                                          \
            ++g_failures;                                                                          \
            SYLAR_LOG_ERROR(g_logger) << "EXPECT_EQ failed: " #lhs << " line=" << __LINE__;     \
        }                                                                                          \
    } while (0)

int main()
{
    sylar::ai_gateway::AiGatewayDemoServlet servlet;
    sylar::http::HttpRequest::ptr request(new sylar::http::HttpRequest);
    sylar::http::HttpResponse::ptr response(new sylar::http::HttpResponse);

    request->setPath("/demo");
    EXPECT_EQ(servlet.handle(request, response, nullptr), 0);
    EXPECT_EQ(response->getStatus(), sylar::http::HttpStatus::OK);
    EXPECT_EQ(response->getHeader("Content-Type"), "text/html; charset=utf-8");
    EXPECT_TRUE(response->getBody().find("data-source=\"static-file\"") != std::string::npos);
    EXPECT_TRUE(response->getBody().find("id=\"sendRequest\"") != std::string::npos);
    EXPECT_TRUE(response->getBody().find("X-Ai-Gateway-Demo-Trace") != std::string::npos);
    EXPECT_TRUE(response->getBody().find("/internal/status") != std::string::npos);

    return g_failures == 0 ? 0 : 1;
}
