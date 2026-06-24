#include "ai_gateway_protocol.h"

#include <json/json.h>

#include <memory>

/**
主要是对 OpenAI Chat Completions 请求体响应体体进行解析和序列化。
*/
namespace sylar
{
namespace ai_gateway
{
namespace
{

// 统一设置错误信息，并返回 false。
// 这样调用方可以直接写：return SetError(...);
bool SetError(const std::string &message, std::string *error)
{
    if (error)
    {
        *error = message;
    }
    return false;
}

// 当前第一版只支持 OpenAI Chat Completions 中最基础的三种文本角色。
// 暂不支持 tool、function、developer 等扩展角色。
bool IsSupportedRole(const std::string &role)
{
    return role == "system" || role == "user" || role == "assistant";
}

// 将 Json::Value 序列化为紧凑 JSON 字符串。
// indentation 设为空字符串，表示不输出格式化缩进，适合 HTTP 响应体。
std::string WriteJson(const Json::Value &value)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

} // namespace

// 解析调用方传入的 Chat Completions 请求体。
// 当前支持的最小格式类似：
// {
//   "model": "xxx",
//   "messages": [
//     {"role": "user", "content": "hello"}
//   ],
//   "stream": false,
//   "temperature": 0.7,
//   "max_tokens": 1024
// }
//
// 返回 true 表示解析成功，并把结果写入 request。
// 返回 false 表示请求非法，错误原因写入 error。
bool ParseChatCompletionRequest(const std::string &body,
                                ChatCompletionRequest *request,
                                std::string *error)
{
    // request 是输出参数，必须有效。
    if (!request)
    {
        return SetError("请求接收对象不能为空", error);
    }

    // 使用 jsoncpp 的 CharReader 解析原始请求体。
    Json::CharReaderBuilder builder;
    Json::Value root;
    std::string parse_errors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());

    // body 不是合法 JSON，直接拒绝。
    if (!reader->parse(body.data(), body.data() + body.size(), &root, &parse_errors))
    {
        return SetError("请求体不是合法 JSON", error);
    }

    // 顶层必须是 JSON 对象，不能是数组、字符串、数字等。
    if (!root.isObject())
    {
        return SetError("请求体必须是 JSON 对象", error);
    }

    // model 是必填字段，用于指定模型名称。
    if (!root["model"].isString() || root["model"].asString().empty())
    {
        return SetError("model 必须是非空字符串", error);
    }

    // messages 是必填字段，并且至少包含一条消息。
    if (!root["messages"].isArray() || root["messages"].empty())
    {
        return SetError("messages 必须是非空数组", error);
    }

    // 第一版不支持流式响应。
    // 如果传了 stream，只允许是 false。
    if (root.isMember("stream") && (!root["stream"].isBool() || root["stream"].asBool()))
    {
        return SetError("第一版不支持 stream=true", error);
    }

    // temperature 可选；如果传入，必须是数值。
    // 当前这里只做协议校验，不在这里决定采样行为。
    if (root.isMember("temperature") && !root["temperature"].isNumeric())
    {
        return SetError("temperature 必须是数值", error);
    }

    // max_tokens 可选；如果传入，必须是正整数。
    // isUInt 要求是无符号整数，因此负数、小数、字符串都会被拒绝。
    if (root.isMember("max_tokens") &&
        (!root["max_tokens"].isUInt() || root["max_tokens"].asUInt() == 0))
    {
        return SetError("max_tokens 必须是正整数", error);
    }

    // 先解析到临时对象，全部字段都校验成功后再覆盖输出参数。
    // 这样可以避免解析失败时污染调用方原来的 request 内容。
    ChatCompletionRequest parsed;
    parsed.model = root["model"].asString();

    // 校验并提取 messages。
    for (const auto &message : root["messages"])
    {
        // 每条 message 必须是对象，并且必须包含：
        // role: system / user / assistant
        // content: 字符串
        //
        // 当前第一版只支持纯文本消息，不支持数组 content、多模态、tool_calls 等格式。
        if (!message.isObject() || !message["role"].isString() ||
            !IsSupportedRole(message["role"].asString()) || !message["content"].isString())
        {
            return SetError("messages 只能包含 system、user、assistant 文本消息", error);
        }

        parsed.messages.push_back({message["role"].asString(), message["content"].asString()});
    }

    // 全部校验成功后，再写入输出参数。
    *request = parsed;

    // 成功时清空旧错误信息，避免调用方误读上一次错误。
    if (error)
    {
        error->clear();
    }

    return true;
}

// 构造兼容 OpenAI Chat Completions 风格的响应。
// 当前 usage 里 token 数统一填 0，因为 mock 网关没有真正 tokenizer。
std::string BuildChatCompletionResponse(const ChatCompletionRequest &request,
                                        const std::string &request_id,
                                        uint64_t created,
                                        const std::string &content)
{
    Json::Value root;

    // 响应唯一 ID，由调用方传入。
    root["id"] = request_id;

    // OpenAI Chat Completions 的对象类型。
    root["object"] = "chat.completion";

    // 创建时间戳，通常是 Unix timestamp。
    root["created"] = static_cast<Json::UInt64>(created);

    // 回显请求中的模型名称。
    root["model"] = request.model;

    // choices 是数组。非流式响应里通常至少有一个 choice。
    Json::Value choice;
    choice["index"] = 0;
    choice["message"]["role"] = "assistant";
    choice["message"]["content"] = content;

    // 第一版固定认为正常结束。
    choice["finish_reason"] = "stop";

    root["choices"].append(choice);

    // mock 实现不统计 token，先统一置 0。
    // 如果以后接真实模型，可以从上游返回值或 tokenizer 里填充。
    root["usage"]["prompt_tokens"] = 0;
    root["usage"]["completion_tokens"] = 0;
    root["usage"]["total_tokens"] = 0;

    return WriteJson(root);
}

// 构造错误响应。
// 结构模仿 OpenAI API 的 error 格式：
// {
//   "error": {
//     "message": "...",
//     "type": "...",
//     "code": "..."
//   }
// }
std::string
BuildErrorResponse(const std::string &message, const std::string &type, const std::string &code)
{
    Json::Value root;
    root["error"]["message"] = message;
    root["error"]["type"] = type;
    root["error"]["code"] = code;
    return WriteJson(root);
}

// 构造 mock 上游模型服务的响应。
// 这个格式不是 OpenAI 标准格式，是网关内部 mock provider 使用的简化格式。
std::string BuildMockProviderResponse(const std::string &provider, const std::string &content)
{
    Json::Value root;

    // 标记这是 mock provider 返回的聊天结果。
    root["object"] = "mock.chat.completion";

    // provider 用于标识是哪一个上游实例返回的结果。
    root["provider"] = provider;

    // content 是 mock provider 生成的文本内容。
    root["content"] = content;

    return WriteJson(root);
}

// 解析 mock 上游模型服务的响应。
// 成功时把 provider 和 content 写入输出参数。
// 失败时返回 false，并通过 error 返回错误原因。
bool ParseMockProviderResponse(const std::string &body,
                               std::string *provider,
                               std::string *content,
                               std::string *error)
{
    // provider 和 content 是输出参数，必须有效。
    if (!provider || !content)
    {
        return SetError("上游响应接收对象不能为空", error);
    }

    Json::CharReaderBuilder builder;
    Json::Value root;
    std::string parse_errors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());

    // mock 上游响应必须是合法 JSON 对象，并且至少包含：
    // provider: 字符串
    // content: 字符串
    //
    // 这里把 JSON 解析失败、顶层类型错误、字段缺失、字段类型错误统一视为无效上游响应。
    if (!reader->parse(body.data(), body.data() + body.size(), &root, &parse_errors) ||
        !root.isObject() || !root["provider"].isString() || !root["content"].isString())
    {
        return SetError("上游模型服务返回了无效响应", error);
    }

    // 提取上游服务名和生成内容。
    *provider = root["provider"].asString();
    *content = root["content"].asString();

    // 成功时清空旧错误信息。
    if (error)
    {
        error->clear();
    }

    return true;
}

} // namespace ai_gateway
} // namespace sylar