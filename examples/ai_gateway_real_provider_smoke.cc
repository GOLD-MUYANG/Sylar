#include "modules/ai_gateway/real_provider_smoke.h"

#include <iostream>

int main()
{
    sylar::ai_gateway::RealProviderSmokeConfig config =
        sylar::ai_gateway::LoadRealProviderSmokeConfigFromEnv();
    sylar::ai_gateway::RealProviderSmokeResult result =
        sylar::ai_gateway::RunRealProviderSmoke(config);

    std::cout << "status=" << sylar::ai_gateway::RealProviderSmokeStatusToString(result.status)
              << " attempts=" << result.attempts << " http_status=" << result.http_status
              << " message=\"" << result.message << "\"" << std::endl;

    if (result.status == sylar::ai_gateway::RealProviderSmokeStatus::OK ||
        result.status == sylar::ai_gateway::RealProviderSmokeStatus::SKIPPED)
    {
        return 0;
    }
    return 1;
}
