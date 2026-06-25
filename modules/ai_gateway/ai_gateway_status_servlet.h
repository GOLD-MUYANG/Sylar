#ifndef __SYLAR_AI_GATEWAY_STATUS_SERVLET_H__
#define __SYLAR_AI_GATEWAY_STATUS_SERVLET_H__

#include "ai_gateway_upstream.h"
#include "sylar/http/servlet.h"

namespace sylar
{
namespace ai_gateway
{

// 只读状态接口，用于本地演示和排查当前 Provider 治理状态。
class AiGatewayStatusServlet : public sylar::http::Servlet
{
public:
    typedef std::shared_ptr<AiGatewayStatusServlet> ptr;

    AiGatewayStatusServlet(const std::vector<AiGatewayProviderConfig> &providers,
                           sylar::http::HttpLoadBalanceClient::ptr client);

    int32_t handle(sylar::http::HttpRequest::ptr request,
                   sylar::http::HttpResponse::ptr response,
                   sylar::http::HttpSession::ptr session) override;

private:
    std::string buildStatusBody() const;

private:
    std::vector<AiGatewayProviderConfig> m_providers;
    sylar::http::HttpLoadBalanceClient::ptr m_client;
};

} // namespace ai_gateway
} // namespace sylar

#endif
