#include "ai_gateway_upstream.h"

#include "sylar/uri.h"

namespace sylar
{
namespace ai_gateway
{
namespace
{

void SetError(std::string *error, const std::string &message)
{
    if (error)
    {
        *error = message;
    }
}

bool ParseStrategy(const std::string &value, sylar::http::HttpLoadBalanceStrategy *strategy)
{
    if (value == "ROUND_ROBIN")
    {
        *strategy = sylar::http::HttpLoadBalanceStrategy::ROUND_ROBIN;
        return true;
    }
    if (value == "RANDOM")
    {
        *strategy = sylar::http::HttpLoadBalanceStrategy::RANDOM;
        return true;
    }
    if (value == "WEIGHTED_ROUND_ROBIN")
    {
        *strategy = sylar::http::HttpLoadBalanceStrategy::WEIGHTED_ROUND_ROBIN;
        return true;
    }
    if (value == "LEAST_CONNECTION")
    {
        *strategy = sylar::http::HttpLoadBalanceStrategy::LEAST_CONNECTION;
        return true;
    }
    return false;
}

} // namespace

sylar::http::HttpLoadBalanceClient::ptr CreateLoadBalanceClient(
    const std::vector<AiGatewayProviderConfig> &providers,
    const std::string &strategy_name,
    std::string *error)
{
    return CreateLoadBalanceClient(providers, strategy_name, AiGatewayUpstreamOptions(), error);
}

sylar::http::HttpLoadBalanceClient::ptr CreateLoadBalanceClient(
    const std::vector<AiGatewayProviderConfig> &providers,
    const std::string &strategy_name,
    const AiGatewayUpstreamOptions &options,
    std::string *error)
{
    if (error)
    {
        error->clear();
    }

    if (providers.empty())
    {
        SetError(error, "provider 列表不能为空");
        return nullptr;
    }

    sylar::http::HttpLoadBalanceStrategy strategy;
    if (!ParseStrategy(strategy_name, &strategy))
    {
        SetError(error, "不支持的负载均衡策略: " + strategy_name);
        return nullptr;
    }

    std::vector<sylar::http::HttpEndpoint::ptr> endpoints;
    endpoints.reserve(providers.size());
    for (const auto &provider : providers)
    {
        if (provider.name.empty() || provider.url.empty())
        {
            SetError(error, "provider name 和 url 不能为空");
            return nullptr;
        }

        sylar::Uri::ptr uri = sylar::Uri::Create(provider.url);
        if (!uri || (uri->getScheme() != "http" && uri->getScheme() != "https") ||
            uri->getHost().empty() || uri->getPort() <= 0)
        {
            SetError(error, "provider url 不合法: " + provider.name);
            return nullptr;
        }

        sylar::http::HttpEndpoint::ptr endpoint = sylar::http::HttpEndpoint::Create(
            uri->getHost(), static_cast<uint32_t>(uri->getPort()), uri->getScheme() == "https",
            provider.weight);
        if (!endpoint)
        {
            SetError(error, "无法创建 provider endpoint: " + provider.name);
            return nullptr;
        }
        endpoints.push_back(endpoint);
    }

    sylar::http::HttpLoadBalanceClient::ptr client =
        sylar::http::HttpLoadBalanceClient::Create(
            endpoints, strategy, options.limiter, options.circuit_breaker);
    if (!client)
    {
        SetError(error, "无法创建负载均衡客户端");
    }
    return client;
}

} // namespace ai_gateway
} // namespace sylar
