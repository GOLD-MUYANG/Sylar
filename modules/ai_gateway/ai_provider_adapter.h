#ifndef __SYLAR_AI_GATEWAY_AI_PROVIDER_ADAPTER_H__
#define __SYLAR_AI_GATEWAY_AI_PROVIDER_ADAPTER_H__

#include "ai_gateway_protocol.h"
#include "real_provider_gateway.h"
#include "sylar/http/http_connection.h"
#include "sylar/http/http_request_options.h"

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace sylar
{
namespace ai_gateway
{

// Adapter 构造出的单次 Provider HTTP 请求。
// 它只描述出站请求，不负责限流、熔断、候选选择或真实 I/O。
struct ProviderHttpRequest
{
    std::string url;
    sylar::http::HttpRequestOptions options;
    std::map<std::string, std::string> headers;
    std::string body;
};

// 真实 Provider 适配器契约。
//
// 职责边界：
// 1. 把网关内部请求转换为某类 Provider 的 HTTP 请求；
// 2. 把 Provider 响应解析回内部响应；
// 3. 不做 endpoint 选择、重试、限流或熔断。
class AiProviderAdapter
{
public:
    typedef std::shared_ptr<AiProviderAdapter> ptr;
    virtual ~AiProviderAdapter() {}

    virtual bool buildChatRequest(const ProviderCandidate &candidate,
                                  const GatewayChatRequest &request,
                                  const std::string &api_key,
                                  const sylar::http::HttpRequestOptions &options,
                                  ProviderHttpRequest *http_request,
                                  std::string *error) const = 0;

    virtual bool parseChatResponse(const ProviderCandidate &candidate,
                                   const std::string &body,
                                   GatewayChatResponse *response,
                                   std::string *error) const = 0;
};

class OpenAICompatibleProviderAdapter : public AiProviderAdapter
{
public:
    bool buildChatRequest(const ProviderCandidate &candidate,
                          const GatewayChatRequest &request,
                          const std::string &api_key,
                          const sylar::http::HttpRequestOptions &options,
                          ProviderHttpRequest *http_request,
                          std::string *error) const override;

    bool parseChatResponse(const ProviderCandidate &candidate,
                           const std::string &body,
                           GatewayChatResponse *response,
                           std::string *error) const override;
};

typedef std::function<sylar::http::HttpResult::ptr(const ProviderCandidate &,
                                                   const ProviderHttpRequest &)>
    ProviderHttpPost;

ProviderAttemptExecutor::BudgetedAttemptHandler
CreateOpenAICompatibleAttemptHandler(
    const ProviderHttpPost &post,
    const sylar::http::HttpRequestOptions &options = sylar::http::HttpRequestOptions());

sylar::http::HttpResult::ptr DoProviderHttpPost(const ProviderCandidate &candidate,
                                                const ProviderHttpRequest &request);

} // namespace ai_gateway
} // namespace sylar

#endif
