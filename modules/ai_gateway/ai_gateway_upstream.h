#ifndef __SYLAR_AI_GATEWAY_UPSTREAM_H__
#define __SYLAR_AI_GATEWAY_UPSTREAM_H__

#include "sylar/http/http_load_balance_client.h"

#include <stdint.h>
#include <string>
#include <vector>

namespace sylar
{
namespace ai_gateway
{

// 单个上游模型 Provider 的静态配置。
struct AiGatewayProviderConfig
{
    std::string name;
    std::string url;
    uint32_t weight = 1;
};

// 将网关 Provider 配置转换为独立的多实例 HTTP 客户端。
sylar::http::HttpLoadBalanceClient::ptr CreateLoadBalanceClient(
    const std::vector<AiGatewayProviderConfig> &providers,
    const std::string &strategy,
    std::string *error);

} // namespace ai_gateway
} // namespace sylar

#endif
