#ifndef __SYLAR_AI_GATEWAY_SERVLET_H__
#define __SYLAR_AI_GATEWAY_SERVLET_H__

#include "ai_gateway_protocol.h"
#include "sylar/http/http_connection.h"
#include "sylar/http/http_load_balance_client.h"
#include "sylar/http/servlet.h"

#include <atomic>
#include <functional>
#include <json/json.h>

namespace sylar
{
namespace ai_gateway
{

class AiGatewayServlet : public sylar::http::Servlet
{
public:
    typedef std::function<sylar::http::HttpResult::ptr(
        const std::string &body, sylar::http::HttpLoadBalanceRequestTrace *trace)>
        UpstreamPost;
    typedef std::function<sylar::http::HttpResult::ptr(const ChatCompletionRequest &request,
                                                       Json::Value *trace)>
        CompatibleUpstreamPost;

    explicit AiGatewayServlet(UpstreamPost upstream_post, bool demo_trace_enabled = false);
    explicit AiGatewayServlet(CompatibleUpstreamPost upstream_post,
                              bool demo_trace_enabled = false);

    int32_t handle(sylar::http::HttpRequest::ptr request,
                   sylar::http::HttpResponse::ptr response,
                   sylar::http::HttpSession::ptr session) override;

private:
    void writeError(sylar::http::HttpResponse::ptr response,
                    sylar::http::HttpStatus status,
                    const std::string &message,
                    const std::string &type,
                    const std::string &code) const;

private:
    UpstreamPost m_upstreamPost;
    CompatibleUpstreamPost m_compatibleUpstreamPost;
    bool m_demoTraceEnabled = false;
    std::atomic<uint64_t> m_requestSequence;
};

} // namespace ai_gateway
} // namespace sylar

#endif
