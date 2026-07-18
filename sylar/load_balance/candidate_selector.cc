#include "candidate_selector.h"

namespace sylar
{
namespace load_balance
{

bool ParseLoadBalanceStrategy(const std::string &value, LoadBalanceStrategy *strategy)
{
    if (!strategy)
    {
        return false;
    }
    if (value == "ROUND_ROBIN")
    {
        *strategy = LoadBalanceStrategy::ROUND_ROBIN;
        return true;
    }
    if (value == "RANDOM")
    {
        *strategy = LoadBalanceStrategy::RANDOM;
        return true;
    }
    if (value == "WEIGHTED_ROUND_ROBIN")
    {
        *strategy = LoadBalanceStrategy::WEIGHTED_ROUND_ROBIN;
        return true;
    }
    if (value == "LEAST_CONNECTION")
    {
        *strategy = LoadBalanceStrategy::LEAST_CONNECTION;
        return true;
    }
    return false;
}

const char *LoadBalanceStrategyToString(LoadBalanceStrategy strategy)
{
    switch (strategy)
    {
    case LoadBalanceStrategy::ROUND_ROBIN:
        return "ROUND_ROBIN";
    case LoadBalanceStrategy::RANDOM:
        return "RANDOM";
    case LoadBalanceStrategy::WEIGHTED_ROUND_ROBIN:
        return "WEIGHTED_ROUND_ROBIN";
    case LoadBalanceStrategy::LEAST_CONNECTION:
        return "LEAST_CONNECTION";
    default:
        return "UNKNOWN";
    }
}

} // namespace load_balance
} // namespace sylar
