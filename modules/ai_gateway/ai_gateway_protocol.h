#ifndef __SYLAR_AI_GATEWAY_PROTOCOL_H__
#define __SYLAR_AI_GATEWAY_PROTOCOL_H__

#include <stdint.h>

#include <string>
#include <vector>

namespace sylar
{
namespace ai_gateway
{

struct ChatMessage
{
    std::string role;
    std::string content;
};

struct ChatCompletionRequest
{
    std::string model;
    std::vector<ChatMessage> messages;
};

/**
 * @brief 解析并校验第一版支持的 Chat Completions 非流式请求。
 */
bool ParseChatCompletionRequest(const std::string &body,
                                ChatCompletionRequest *request,
                                std::string *error);

/**
 * @brief 生成面向调用方的非流式 Chat Completions 响应。
 */
std::string BuildChatCompletionResponse(const ChatCompletionRequest &request,
                                        const std::string &request_id,
                                        uint64_t created,
                                        const std::string &content);

/**
 * @brief 生成不包含内部实现细节的 OpenAI 兼容错误对象。
 */
std::string BuildErrorResponse(const std::string &message,
                               const std::string &type,
                               const std::string &code);

/**
 * @brief 构造 Mock Provider 与网关之间使用的最小响应。
 */
std::string BuildMockProviderResponse(const std::string &provider, const std::string &content);

/**
 * @brief 校验并读取 Mock Provider 的最小响应。
 */
bool ParseMockProviderResponse(const std::string &body,
                               std::string *provider,
                               std::string *content,
                               std::string *error);

} // namespace ai_gateway
} // namespace sylar

#endif
