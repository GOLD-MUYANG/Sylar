#ifndef __SYLAR_AI_GATEWAY_REAL_PROVIDER_RUNTIME_H__
#define __SYLAR_AI_GATEWAY_REAL_PROVIDER_RUNTIME_H__

#include "ai_provider_adapter.h"
#include "real_provider_gateway.h"
#include "sylar/http/http_load_balance_client.h"
#include "sylar/mutex.h"

#include <json/json.h>
#include <memory>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

namespace sylar
{
namespace ai_gateway
{

struct RealProviderRuntimeConfig
{
    bool enabled = false;
    std::string server_name;
    std::string route_path = "/v1/chat/completions";
    bool demo_enabled = false;
    std::string load_balance = "ROUND_ROBIN";
    uint64_t request_deadline_ms = 30000;
    uint32_t max_total_attempts = 3;
    sylar::http::HttpRequestOptions request_options;
    sylar::http::HttpConcurrencyLimitOptions limiter_options;
    sylar::http::HttpCircuitBreakerOptions circuit_breaker_options;
    std::vector<ProviderCandidate> providers;
};

bool LoadRealProviderRuntimeConfig(const YAML::Node &root,
                                   RealProviderRuntimeConfig *config,
                                   std::string *error);

std::string DumpRealProviderTrace(const Json::Value &trace);

class RealProviderRuntime
{
public:
    typedef std::shared_ptr<RealProviderRuntime> ptr;

    static RealProviderRuntime::ptr Create(const RealProviderRuntimeConfig &config,
                                           const ProviderHttpPost &post = DoProviderHttpPost);

    sylar::http::HttpResult::ptr execute(const GatewayChatRequest &request,
                                         Json::Value *trace = nullptr);

    Json::Value buildStatusJson() const;

private:
    struct ProviderStats
    {
        ProviderCandidate candidate;
        uint32_t in_flight = 0;
        uint64_t success_count = 0;
        uint64_t failure_count = 0;
        uint64_t rate_limited_count = 0;
        uint64_t last_attempt_ms = 0;
        std::string last_failure_reason;
        std::string last_smoke_result = "not_run";
    };

    explicit RealProviderRuntime(const RealProviderRuntimeConfig &config);

    bool init(const ProviderHttpPost &post, std::string *error);
    void recordAttemptBegin(const ProviderCandidate &candidate);
    void recordAttemptEnd(const ProviderCandidate &candidate,
                          const sylar::http::HttpResult::ptr &result);
    uint32_t getProviderInFlight(const std::string &provider_name) const;
    Json::Value buildCandidateStatus(const ProviderStats &stats) const;

private:
    typedef sylar::Mutex MutexType;

    RealProviderRuntimeConfig m_config;
    LogicalModelRouter m_router;
    ProviderExecutionControls m_controls;
    ProviderAttemptExecutor::BudgetedAttemptHandler m_handler;
    sylar::load_balance::CandidateSelector<ProviderCandidate>::ptr m_selector;
    mutable MutexType m_mutex;
    std::vector<ProviderStats> m_stats;
    uint64_t m_requestSequence = 0;
};

} // namespace ai_gateway
} // namespace sylar

#endif
