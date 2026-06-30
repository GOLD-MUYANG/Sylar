#include "ai_gateway_servlet.h"

#include "ai_gateway_protocol.h"
#include "sylar/http/http_connection.h"
#include "sylar/log.h"

#include <ctime>
#include <json/json.h>

namespace sylar
{
namespace ai_gateway
{

// 使用 system 日志器，记录网关运行过程中的异常信息。
// 例如：上游回调抛异常、上游返回内容解析失败等。
static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

std::string BuildTraceHeader(const sylar::http::HttpLoadBalanceRequestTrace &trace)
{
    Json::Value root(Json::objectValue);
    Json::Value attempts(Json::arrayValue);

    for (auto &attempt : trace.attempts)
    {
        Json::Value item(Json::objectValue);
        item["endpoint"] = attempt.endpoint_key;
        item["outcome"] = attempt.outcome;
        item["result"] = attempt.result;
        item["http_status"] = attempt.http_status;
        item["reason"] = attempt.reason;
        attempts.append(item);
    }

    root["object"] = "ai_gateway.trace";
    root["attempts"] = attempts;

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, root);
}

std::string DumpTraceJson(const Json::Value &trace)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, trace);
}

// 构造 AI 网关 Servlet。
// upstream_post：外部注入的上游调用函数，用来把请求转发给模型提供者。
// m_requestSequence：本地请求序号，用来生成 chatcmpl-local-xxx 形式的响应 id。
AiGatewayServlet::AiGatewayServlet(UpstreamPost upstream_post, bool demo_trace_enabled)
    : Servlet("ai_gateway"), m_upstreamPost(upstream_post), m_demoTraceEnabled(demo_trace_enabled),
      m_requestSequence(0)
{
}

AiGatewayServlet::AiGatewayServlet(CompatibleUpstreamPost upstream_post, bool demo_trace_enabled)
    : Servlet("ai_gateway"), m_compatibleUpstreamPost(upstream_post),
      m_demoTraceEnabled(demo_trace_enabled), m_requestSequence(0)
{
}

// 处理一次 HTTP 请求。
// 主要流程：
// 1. 解析 OpenAI Chat Completions 兼容请求
// 2. 调用上游模型服务
// 3. 校验上游响应
// 4. 把上游 mock 响应转换成 Chat Completions 格式返回
int32_t AiGatewayServlet::handle(sylar::http::HttpRequest::ptr request,
                                 sylar::http::HttpResponse::ptr response,
                                 sylar::http::HttpSession::ptr)
{
    ChatCompletionRequest completion_request;
    std::string parse_error;

    // 先校验 request 是否存在，并解析请求体。
    // ParseChatCompletionRequest 会检查 body 是否是合法 JSON，
    // 以及 model/messages/role/content 等字段是否合法。
    if (!request ||
        !ParseChatCompletionRequest(request->getBody(), &completion_request, &parse_error))
    {
        // 请求本身不合法，返回 400。
        // parse_error 为空时，使用兜底错误信息。
        writeError(response, sylar::http::HttpStatus::BAD_REQUEST,
                   parse_error.empty() ? "请求参数不合法" : parse_error, "invalid_request_error",
                   "INVALID_REQUEST");
        return 0;
    }

    sylar::http::HttpResult::ptr upstream_result;
    sylar::http::HttpLoadBalanceRequestTrace trace;
    Json::Value compatible_trace;
    const bool trace_requested =
        m_demoTraceEnabled && request->getHeader("X-Ai-Gateway-Demo-Trace") == "1";

    try
    {
        // 调用注入进来的上游请求函数。
        // 这里不直接写死上游地址，而是通过 m_upstreamPost 解耦：
        // 测试时可以注入 mock，上线时可以注入真实的 HTTP 负载均衡调用。
        if (m_compatibleUpstreamPost)
        {
            upstream_result = m_compatibleUpstreamPost(
                completion_request, trace_requested ? &compatible_trace : nullptr);
        }
        else
        {
            upstream_result = m_upstreamPost
                                  ? m_upstreamPost(request->getBody(),
                                                   trace_requested ? &trace : nullptr)
                                  : nullptr;
        }
    }
    catch (const std::exception &e)
    {
        // 防止上游调用逻辑抛异常导致整个 Servlet 崩溃。
        // 捕获标准异常并记录具体错误信息。
        SYLAR_LOG_ERROR(g_logger) << "ai gateway upstream callback threw error=" << e.what();
    }
    catch (...)
    {
        // 捕获非标准异常。
        SYLAR_LOG_ERROR(g_logger) << "ai gateway upstream callback threw";
    }

    if (trace_requested && m_compatibleUpstreamPost)
    {
        response->setHeader("X-Ai-Gateway-Trace", DumpTraceJson(compatible_trace));
    }
    else if (trace_requested)
    {
        response->setHeader("X-Ai-Gateway-Trace", BuildTraceHeader(trace));
    }

    if (m_compatibleUpstreamPost)
    {
        if (!upstream_result ||
            upstream_result->result != (int)sylar::http::HttpResult::Error::OK ||
            !upstream_result->response)
        {
            if (upstream_result && upstream_result->response)
            {
                response->setStatus(upstream_result->response->getStatus());
                response->setHeader("Content-Type",
                                    upstream_result->response->getHeader("Content-Type",
                                                                         "application/json"));
                response->setBody(upstream_result->response->getBody());
                return 0;
            }
            writeError(response, sylar::http::HttpStatus::BAD_GATEWAY,
                       "没有可用的上游模型服务", "server_error", "UPSTREAM_UNAVAILABLE");
            return 0;
        }

        response->setStatus(upstream_result->response->getStatus());
        response->setHeader("Content-Type",
                            upstream_result->response->getHeader("Content-Type",
                                                                 "application/json"));
        response->setBody(upstream_result->response->getBody());
        return 0;
    }

    // 校验上游调用结果。
    // 失败条件包括：
    // 1. 上游调用结果为空
    // 2. HttpResult 本身不是 OK
    // 3. 上游没有返回 HttpResponse
    // 4. 上游 HTTP 状态码不是 200 OK
    if (!upstream_result || upstream_result->result != (int)sylar::http::HttpResult::Error::OK ||
        !upstream_result->response ||
        upstream_result->response->getStatus() != sylar::http::HttpStatus::OK)
    {
        if (upstream_result &&
            upstream_result->result == (int)sylar::http::HttpResult::Error::RATE_LIMITED)
        {
            writeError(response, sylar::http::HttpStatus::TOO_MANY_REQUESTS,
                       "上游模型服务当前请求过多", "rate_limit_error", "RATE_LIMITED");
            return 0;
        }

        // 对调用方来说，这是网关无法成功访问模型服务，所以返回 502。
        writeError(response, sylar::http::HttpStatus::BAD_GATEWAY, "没有可用的上游模型服务",
                   "server_error", "UPSTREAM_UNAVAILABLE");
        return 0;
    }

    std::string provider;
    std::string content;
    std::string provider_error;

    // 解析 mock provider 的响应体。
    // 这里假设上游模型服务返回的是项目内部约定的 mock 响应格式，
    // 不是直接返回 OpenAI Chat Completions 格式。
    //
    // provider：上游提供者名称，例如 mock-a/mock-b。
    // content：模型生成的文本内容。
    // provider_error：解析失败时的错误说明。
    if (!ParseMockProviderResponse(upstream_result->response->getBody(), &provider, &content,
                                   &provider_error))
    {
        // 上游虽然返回了 200，但响应体格式不符合网关约定，
        // 对调用方仍然视为上游不可用。
        SYLAR_LOG_ERROR(g_logger) << "ai gateway invalid upstream response error="
                                  << provider_error;
        writeError(response, sylar::http::HttpStatus::BAD_GATEWAY, "没有可用的上游模型服务",
                   "server_error", "UPSTREAM_UNAVAILABLE");
        return 0;
    }

    // 生成本次请求的递增序号。
    // fetch_add 返回的是自增前的值，所以 +1 后从 1 开始编号。
    const uint64_t sequence = m_requestSequence.fetch_add(1) + 1;

    // 构造 OpenAI Chat Completions 兼容响应。
    response->setStatus(sylar::http::HttpStatus::OK);
    response->setHeader("Content-Type", "application/json");

    // BuildChatCompletionResponse 会把内部 content 包装成：
    // {
    //   "id": "chatcmpl-local-1",
    //   "object": "chat.completion",
    //   "created": ...,
    //   "model": ...,
    //   "choices": [...]
    // }
    response->setBody(BuildChatCompletionResponse(
        completion_request, "chatcmpl-local-" + std::to_string(sequence), time(nullptr), content));

    return 0;
}

// 写入统一的错误响应。
// status：HTTP 状态码，例如 400/502。
// message：返回给调用方看的错误信息。
// type/code：OpenAI 风格错误字段，方便调用方按错误类型处理。
void AiGatewayServlet::writeError(sylar::http::HttpResponse::ptr response,
                                  sylar::http::HttpStatus status,
                                  const std::string &message,
                                  const std::string &type,
                                  const std::string &code) const
{
    response->setStatus(status);
    response->setHeader("Content-Type", "application/json");

    // BuildErrorResponse 会生成统一 JSON 错误格式。
    response->setBody(BuildErrorResponse(message, type, code));
}

} // namespace ai_gateway
} // namespace sylar
