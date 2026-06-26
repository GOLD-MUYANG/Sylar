#include "ai_gateway_demo_servlet.h"

#include "sylar/http/http.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace sylar
{
namespace ai_gateway
{

namespace
{

const char *kDefaultDemoHtmlPath = "modules/ai_gateway/ai_gateway_demo.html";

bool ReadTextFile(const std::string &path, std::string *content)
{
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input)
    {
        return false;
    }

    std::ostringstream ss;
    ss << input.rdbuf();
    *content = ss.str();
    return true;
}

std::string BuildFallbackHtml()
{
    return "<!doctype html><html><head><meta charset=\"utf-8\"><title>AI Gateway Demo</title>"
           "</head><body><h1>AI Gateway Demo</h1><p>demo html file not found.</p></body></html>";
}

} // namespace

AiGatewayDemoServlet::AiGatewayDemoServlet()
    : AiGatewayDemoServlet(kDefaultDemoHtmlPath)
{
}

AiGatewayDemoServlet::AiGatewayDemoServlet(const std::string &html_path)
    : Servlet("ai_gateway_demo"), m_htmlPath(html_path.empty() ? kDefaultDemoHtmlPath : html_path)
{
}

int32_t AiGatewayDemoServlet::handle(sylar::http::HttpRequest::ptr,
                                     sylar::http::HttpResponse::ptr response,
                                     sylar::http::HttpSession::ptr)
{
    std::string body;

    // 兼容从仓库根目录直接运行，以及 ctest 从 build/ 目录运行。
    std::vector<std::string> candidates;
    candidates.push_back(m_htmlPath);
    candidates.push_back("../" + m_htmlPath);

    for (auto &path : candidates)
    {
        if (ReadTextFile(path, &body))
        {
            break;
        }
    }

    if (body.empty())
    {
        body = BuildFallbackHtml();
    }

    response->setStatus(sylar::http::HttpStatus::OK);
    response->setHeader("Content-Type", "text/html; charset=utf-8");
    response->setHeader("Cache-Control", "no-store");
    response->setBody(body);
    return 0;
}

} // namespace ai_gateway
} // namespace sylar
