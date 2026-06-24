#include "ai_gateway_servlet.h"

#include "sylar/application.h"
#include "sylar/env.h"
#include "sylar/http/http_client.h"
#include "sylar/log.h"
#include "sylar/module.h"

#include <yaml-cpp/yaml.h>

namespace sylar
{
namespace ai_gateway
{

// 使用 system 日志器，AI Gateway 模块的日志都会打到这里
static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

// AI 网关模块配置
// 对应配置文件中的 ai_gateway 节点
struct AiGatewayConfig
{
    // 是否启用 AI Gateway 模块
    bool enabled = false;

    // 要挂载路由的 HTTP Server 名称
    // 需要和框架里启动的 server name 对应
    std::string server_name;

    // 上游模型服务地址，例如 http://127.0.0.1:9001
    std::string provider_url;

    // 请求上游模型服务的超时时间，单位：毫秒
    uint64_t request_timeout_ms = 800;

    // 配置比较函数
    // 目前这段代码里没有直接用到，但如果后面接入配置热更新，
    // 可以用它判断配置是否发生变化
    bool operator==(const AiGatewayConfig &other) const
    {
        return enabled == other.enabled && server_name == other.server_name &&
               provider_url == other.provider_url && request_timeout_ms == other.request_timeout_ms;
    }
};

// 从 config/ai_gateway.yml 中读取 AI Gateway 配置
AiGatewayConfig LoadConfig()
{
    AiGatewayConfig config;

    // 拼出配置文件路径
    // EnvMgr::getConfigPath() 一般对应程序启动时指定的配置目录
    const std::string file = sylar::EnvMgr::GetInstance()->getConfigPath() + "/ai_gateway.yml";

    try
    {
        // 加载 YAML 文件
        YAML::Node root = YAML::LoadFile(file);

        // 读取 ai_gateway 节点
        YAML::Node node = root["ai_gateway"];

        // 如果配置文件里没有 ai_gateway 节点，则返回默认配置
        if (!node)
        {
            return config;
        }

        // 读取是否启用模块
        // as<T>(default_value) 表示字段不存在时使用默认值
        config.enabled = node["enabled"].as<bool>(config.enabled);

        // 读取要挂载的 HTTP Server 名称
        config.server_name = node["server_name"].as<std::string>(config.server_name);

        // 读取上游模型服务地址
        config.provider_url = node["provider_url"].as<std::string>(config.provider_url);

        // 读取请求上游的超时时间
        config.request_timeout_ms =
            node["request_timeout_ms"].as<uint64_t>(config.request_timeout_ms);
    }
    catch (const std::exception &e)
    {
        // 配置文件不存在、YAML 格式错误、类型转换失败等都会进入这里
        // 当前策略：记录错误，然后返回默认配置
        SYLAR_LOG_ERROR(g_logger) << "load ai gateway config failed file=" << file
                                  << " error=" << e.what();
    }

    return config;
}

// AI Gateway 模块
// 这个类会被框架通过 CreateModule() 动态创建
class AiGatewayModule : public sylar::Module
{
public:
    // 模块名称 ai_gateway，版本 1.0
    AiGatewayModule() : Module("ai_gateway", "1.0", "")
    {
    }

    // 当服务器准备完成后调用
    // 这里适合做路由注册、服务发现、连接池初始化等工作
    bool onServerReady() override
    {
        // 读取 AI Gateway 配置
        const AiGatewayConfig config = LoadConfig();

        // 未启用时直接返回 true
        // 表示模块没有启动，但不是错误
        if (!config.enabled)
        {
            SYLAR_LOG_INFO(g_logger) << "ai gateway module disabled";
            return true;
        }

        // 启用模块时，server_name 和 provider_url 都必须配置
        if (config.server_name.empty() || config.provider_url.empty())
        {
            SYLAR_LOG_ERROR(g_logger) << "ai gateway requires server_name and provider_url";
            return false;
        }

        // 获取全局 Application 对象
        sylar::Application *application = sylar::Application::GetInstance();

        // 根据配置里的 server_name 查找已经创建好的 HTTP Server
        sylar::http::HttpServer::ptr server =
            application ? application->getHttpServer(config.server_name) : nullptr;

        // 如果找不到对应的 server，说明配置的 server_name 不对，
        // 或者 HTTP Server 还没有在 Application 中注册
        if (!server)
        {
            SYLAR_LOG_ERROR(g_logger) << "ai gateway server not found name=" << config.server_name;
            return false;
        }

        // 根据 provider_url 创建 HTTP 客户端
        // 这个 client 后面会负责把请求转发给上游模型服务
        sylar::http::HttpClient::ptr client = sylar::http::HttpClient::Create(config.provider_url);

        // provider_url 不合法时，Create 会失败
        if (!client)
        {
            SYLAR_LOG_ERROR(g_logger) << "ai gateway invalid provider url";
            return false;
        }

        // 构造一个上游 POST 调用函数
        // AiGatewayServlet 不直接依赖 HttpClient，而是通过这个函数回调调用上游
        //
        // 捕获 client：
        //   保证 servlet 调用时还能访问这个 HTTP 客户端
        //
        // 捕获 config：
        //   使用里面的 request_timeout_ms
        AiGatewayServlet::UpstreamPost upstream = [client, config](const std::string &body)
        {
            // 把客户端请求体原样转发到上游模型服务的 OpenAI 兼容接口
            // 完整请求地址实际是：
            //   provider_url + "/v1/chat/completions"
            return client->post("/v1/chat/completions", config.request_timeout_ms,
                                {{"Content-Type", "application/json"}}, body);
        };

        // 向目标 HTTP Server 注册 OpenAI Chat Completions 兼容路由
        //
        // 外部访问：
        //   POST /v1/chat/completions
        //
        // 实际处理：
        //   AiGatewayServlet::handle()
        //
        // handle() 内部会解析请求、调用 upstream、处理上游返回或错误
        server->getServletDispatch()->addServlet("/v1/chat/completions",
                                                 std::make_shared<AiGatewayServlet>(upstream));

        SYLAR_LOG_INFO(g_logger) << "ai gateway route registered server=" << config.server_name;
        return true;
    }
};

} // namespace ai_gateway
} // namespace sylar

extern "C"
{

    // 动态模块入口函数
    // 框架加载 .so 模块时会调用这个函数创建模块对象
    sylar::Module *CreateModule()
    {
        return new sylar::ai_gateway::AiGatewayModule;
    }

    // 动态模块销毁函数
    // 框架卸载模块时调用，用于释放 CreateModule() 创建的对象
    //
    // 注意：这里函数名是 DestoryModule，不是 DestroyModule。
    // 如果 Sylar 框架原本就是按 DestoryModule 查找符号，就不要改。
    // 这是框架接口拼写问题，不一定是你这里写错。
    void DestoryModule(sylar::Module *module)
    {
        delete module;
    }
}