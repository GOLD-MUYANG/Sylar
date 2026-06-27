#include "ai_provider_adapter.h"

#include <json/json.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <memory>

namespace sylar
{
namespace ai_gateway
{
namespace
{

bool SetError(const std::string &message, std::string *error)
{
    if (error)
    {
        *error = message;
    }
    return false;
}

std::string WriteJson(const Json::Value &value)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

std::string JoinUrl(const std::string &base_url, const std::string &path)
{
    if (base_url.empty())
    {
        return path;
    }
    if (path.empty())
    {
        return base_url;
    }
    if (base_url[base_url.size() - 1] == '/' && path[0] == '/')
    {
        return base_url + path.substr(1);
    }
    if (base_url[base_url.size() - 1] != '/' && path[0] != '/')
    {
        return base_url + "/" + path;
    }
    return base_url + path;
}

sylar::http::HttpResult::ptr MakeAdapterError(sylar::http::HttpResult::Error error,
                                              const std::string &message)
{
    sylar::http::HttpResult::ptr result =
        std::make_shared<sylar::http::HttpResult>((int)error, nullptr, message);
    result->attempt.detail = message;
    return result;
}

bool ParseJsonObject(const std::string &body, Json::Value *root)
{
    if (!root)
    {
        return false;
    }

    Json::CharReaderBuilder builder;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    return reader->parse(body.data(), body.data() + body.size(), root, &errors) && root->isObject();
}

const char *GetEnvValue(const std::string &name)
{
    if (name.empty())
    {
        return nullptr;
    }
    const char *value = std::getenv(name.c_str());
    if (!value || !*value)
    {
        return nullptr;
    }
    return value;
}

uint64_t LimitTimeoutByBudget(uint64_t timeout_ms, uint64_t remaining_ms)
{
    if (remaining_ms == (uint64_t)-1)
    {
        return timeout_ms;
    }
    if (timeout_ms == (uint64_t)-1)
    {
        return remaining_ms;
    }
    return std::min(timeout_ms, remaining_ms);
}

sylar::http::HttpRequestOptions
ApplyBudgetToOptions(const sylar::http::HttpRequestOptions &options,
                     RequestExecutionBudget *budget)
{
    if (!budget)
    {
        return options;
    }

    uint64_t remaining_ms = budget->remainingMs();
    sylar::http::HttpRequestOptions limited = options;
    limited.connect_timeout_ms = LimitTimeoutByBudget(limited.connect_timeout_ms, remaining_ms);
    limited.send_timeout_ms = LimitTimeoutByBudget(limited.send_timeout_ms, remaining_ms);
    limited.recv_timeout_ms = LimitTimeoutByBudget(limited.recv_timeout_ms, remaining_ms);
    limited.total_timeout_ms = LimitTimeoutByBudget(limited.total_timeout_ms, remaining_ms);
    return limited;
}

} // namespace

bool OpenAICompatibleProviderAdapter::buildChatRequest(
    const ProviderCandidate &candidate,
    const GatewayChatRequest &request,
    const std::string &api_key,
    const sylar::http::HttpRequestOptions &options,
    ProviderHttpRequest *http_request,
    std::string *error) const
{
    if (!http_request)
    {
        return SetError("provider HTTP 请求接收对象不能为空", error);
    }
    if (api_key.empty())
    {
        return SetError("provider API Key 不能为空", error);
    }
    if (candidate.base_url.empty() || candidate.chat_path.empty() || candidate.upstream_model.empty())
    {
        return SetError("provider 请求配置不完整", error);
    }

    Json::Value root;
    root["model"] = candidate.upstream_model;
    root["stream"] = false;

    for (const auto &message : request.messages)
    {
        Json::Value item;
        item["role"] = message.role;
        item["content"] = message.content;
        root["messages"].append(item);
    }

    ProviderHttpRequest built;
    built.url = JoinUrl(candidate.base_url, candidate.chat_path);
    built.options = options;
    built.headers["Authorization"] = "Bearer " + api_key;
    built.headers["Content-Type"] = "application/json";
    built.headers["Accept"] = "application/json";
    built.body = WriteJson(root);

    *http_request = built;
    if (error)
    {
        error->clear();
    }
    return true;
}

bool OpenAICompatibleProviderAdapter::parseChatResponse(const ProviderCandidate &candidate,
                                                        const std::string &body,
                                                        GatewayChatResponse *response,
                                                        std::string *error) const
{
    if (!response)
    {
        return SetError("provider 响应接收对象不能为空", error);
    }

    Json::Value root;
    if (!ParseJsonObject(body, &root) || !root["choices"].isArray() || root["choices"].empty())
    {
        return SetError("provider 响应格式无效", error);
    }

    const Json::Value &message = root["choices"][0]["message"];
    if (!message.isObject() || !message["content"].isString())
    {
        return SetError("provider 响应格式无效", error);
    }

    GatewayChatResponse parsed;
    parsed.model = candidate.logical_model;
    parsed.content = message["content"].asString();
    *response = parsed;
    if (error)
    {
        error->clear();
    }
    return true;
}

ProviderAttemptExecutor::BudgetedAttemptHandler CreateOpenAICompatibleAttemptHandler(
    const ProviderHttpPost &post,
    const sylar::http::HttpRequestOptions &options)
{
    return [post, options](const ProviderCandidate &candidate,
                           const GatewayChatRequest &request,
                           RequestExecutionBudget *budget) -> sylar::http::HttpResult::ptr {
        if (!post)
        {
            return MakeAdapterError(sylar::http::HttpResult::Error::CONNECT_FAIL,
                                    "provider HTTP post handler missing");
        }
        if (candidate.adapter_type != ProviderAdapterType::OPENAI_COMPATIBLE)
        {
            return MakeAdapterError(sylar::http::HttpResult::Error::CONNECT_FAIL,
                                    "provider adapter type unsupported");
        }

        const char *api_key = GetEnvValue(candidate.api_key_env);
        if (!api_key)
        {
            return MakeAdapterError(sylar::http::HttpResult::Error::CONNECT_FAIL,
                                    "provider API key env missing: " + candidate.api_key_env);
        }

        OpenAICompatibleProviderAdapter adapter;
        ProviderHttpRequest http_request;
        std::string error;
        sylar::http::HttpRequestOptions request_options = ApplyBudgetToOptions(options, budget);
        if (!adapter.buildChatRequest(candidate, request, api_key, request_options, &http_request,
                                      &error))
        {
            return MakeAdapterError(sylar::http::HttpResult::Error::CONNECT_FAIL, error);
        }

        sylar::http::HttpResult::ptr provider_result = post(candidate, http_request);
        if (!provider_result)
        {
            return MakeAdapterError(sylar::http::HttpResult::Error::CONNECT_FAIL,
                                    "provider HTTP result missing");
        }
        if (provider_result->result != (int)sylar::http::HttpResult::Error::OK)
        {
            return provider_result;
        }
        if (!provider_result->response)
        {
            return MakeAdapterError(sylar::http::HttpResult::Error::RESPONSE_PARSE_FAIL,
                                    "provider HTTP response missing");
        }

        GatewayChatResponse chat_response;
        if (!adapter.parseChatResponse(candidate, provider_result->response->getBody(), &chat_response,
                                       &error))
        {
            sylar::http::HttpResult::ptr result = MakeAdapterError(
                sylar::http::HttpResult::Error::RESPONSE_PARSE_FAIL, error);
            result->attempt = provider_result->attempt;
            result->attempt.detail = error;
            return result;
        }

        sylar::http::HttpResponse::ptr response(new sylar::http::HttpResponse);
        response->setStatus(sylar::http::HttpStatus::OK);
        response->setHeader("Content-Type", "application/json");
        response->setBody(BuildChatCompletionResponse(
            request, "chatcmpl-real-provider", static_cast<uint64_t>(std::time(nullptr)),
            chat_response.content));

        sylar::http::HttpResult::ptr result = std::make_shared<sylar::http::HttpResult>(
            (int)sylar::http::HttpResult::Error::OK, response, "ok");
        result->attempt = provider_result->attempt;
        return result;
    };
}

sylar::http::HttpResult::ptr DoProviderHttpPost(const ProviderCandidate &,
                                                const ProviderHttpRequest &request)
{
    return sylar::http::HttpConnection::DoPost(request.url, request.options, request.headers,
                                              request.body);
}

} // namespace ai_gateway
} // namespace sylar
