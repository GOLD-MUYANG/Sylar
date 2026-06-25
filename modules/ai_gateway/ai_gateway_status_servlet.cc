#include "ai_gateway_status_servlet.h"

#include "sylar/uri.h"

#include <json/json.h>
#include <map>

namespace sylar
{
namespace ai_gateway
{
namespace
{

std::string ProviderEndpointKey(const AiGatewayProviderConfig &provider)
{
    sylar::Uri::ptr uri = sylar::Uri::Create(provider.url);
    if (!uri)
    {
        return "";
    }
    return uri->getHost() + ":" + std::to_string(uri->getPort());
}

std::map<std::string, std::string>
BuildProviderNameByEndpoint(const std::vector<AiGatewayProviderConfig> &providers)
{
    std::map<std::string, std::string> names;
    for (const auto &provider : providers)
    {
        const std::string endpoint_key = ProviderEndpointKey(provider);
        if (!endpoint_key.empty())
        {
            names[endpoint_key] = provider.name;
        }
    }
    return names;
}

std::string DumpJson(const Json::Value &root)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, root);
}

} // namespace

AiGatewayStatusServlet::AiGatewayStatusServlet(
    const std::vector<AiGatewayProviderConfig> &providers,
    sylar::http::HttpLoadBalanceClient::ptr client)
    : Servlet("ai_gateway_status"), m_providers(providers), m_client(client)
{
}

int32_t AiGatewayStatusServlet::handle(sylar::http::HttpRequest::ptr,
                                       sylar::http::HttpResponse::ptr response,
                                       sylar::http::HttpSession::ptr)
{
    response->setStatus(sylar::http::HttpStatus::OK);
    response->setHeader("Content-Type", "application/json");
    response->setBody(buildStatusBody());
    return 0;
}

std::string AiGatewayStatusServlet::buildStatusBody() const
{
    Json::Value root;
    root["object"] = "ai_gateway.status";

    Json::Value providers(Json::arrayValue);
    if (!m_client)
    {
        root["providers"] = providers;
        return DumpJson(root);
    }

    const auto names = BuildProviderNameByEndpoint(m_providers);
    const auto snapshots = m_client->getStatusSnapshots();
    for (const auto &snapshot : snapshots)
    {
        Json::Value item;
        auto name_it = names.find(snapshot.endpoint_key);
        item["name"] = name_it == names.end() ? snapshot.endpoint_key : name_it->second;
        item["endpoint"] = snapshot.endpoint_key;
        item["scheme"] = snapshot.ssl ? "https" : "http";
        item["health"] = sylar::http::HttpEndpointStatusToString(snapshot.health_status);
        item["circuit_breaker"] =
            sylar::http::HttpCircuitBreakerStateToString(snapshot.circuit_state);
        item["in_flight"] = snapshot.active_requests;
        item["success_count"] = Json::UInt64(snapshot.success_count);
        item["failure_count"] = Json::UInt64(snapshot.failure_count);
        item["rate_limited_count"] = Json::UInt64(snapshot.rate_limited_count);
        item["last_failure_reason"] = snapshot.last_failure_reason;
        providers.append(item);
    }

    root["providers"] = providers;
    return DumpJson(root);
}

} // namespace ai_gateway
} // namespace sylar
