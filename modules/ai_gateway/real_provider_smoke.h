#ifndef __SYLAR_AI_GATEWAY_REAL_PROVIDER_SMOKE_H__
#define __SYLAR_AI_GATEWAY_REAL_PROVIDER_SMOKE_H__

#include "ai_provider_adapter.h"
#include "sylar/http/http_connection.h"

#include <stdint.h>

#include <iosfwd>
#include <string>

namespace sylar
{
namespace ai_gateway
{

enum class RealProviderSmokeStatus
{
    SKIPPED = 0,
    CONFIG_ERROR = 1,
    REQUEST_FAILED = 2,
    OK = 3,
};

const char *RealProviderSmokeStatusToString(RealProviderSmokeStatus status);
std::ostream &operator<<(std::ostream &os, RealProviderSmokeStatus status);

struct RealProviderSmokeConfig
{
    bool enabled = false;
    std::string provider_name = "manual-real-provider";
    std::string logical_model = "general-chat";
    std::string base_url;
    std::string chat_path = "/v1/chat/completions";
    std::string upstream_model;
    std::string api_key_env;
    std::string compatibility_key = "openai-compatible-chat";
    std::string prompt = "Reply with pong in one short sentence.";
    uint64_t request_deadline_ms = 15000;
    std::string tls_server_name;
    std::string tls_ca_file;
    std::string tls_ca_path;
};

struct RealProviderSmokeResult
{
    RealProviderSmokeStatus status = RealProviderSmokeStatus::SKIPPED;
    std::string message;
    uint32_t attempts = 0;
    int http_status = 0;
};

RealProviderSmokeConfig LoadRealProviderSmokeConfigFromEnv();

RealProviderSmokeResult RunRealProviderSmoke(const RealProviderSmokeConfig &config,
                                             const ProviderHttpPost &post);

RealProviderSmokeResult RunRealProviderSmoke(const RealProviderSmokeConfig &config);

} // namespace ai_gateway
} // namespace sylar

#endif
