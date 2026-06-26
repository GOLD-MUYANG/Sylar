#ifndef __SYLAR_AI_GATEWAY_DEMO_SERVLET_H__
#define __SYLAR_AI_GATEWAY_DEMO_SERVLET_H__

#include "sylar/http/servlet.h"

#include <string>

namespace sylar
{
namespace ai_gateway
{

/**
 * @brief AI Gateway 本地演示页面。
 *
 * 只负责返回静态 HTML，不处理网关请求转发，也不参与 provider 状态计算。
 */
class AiGatewayDemoServlet : public sylar::http::Servlet
{
public:
    AiGatewayDemoServlet();
    explicit AiGatewayDemoServlet(const std::string &html_path);

    int32_t handle(sylar::http::HttpRequest::ptr request,
                   sylar::http::HttpResponse::ptr response,
                   sylar::http::HttpSession::ptr session) override;

private:
    std::string m_htmlPath;
};

} // namespace ai_gateway
} // namespace sylar

#endif
