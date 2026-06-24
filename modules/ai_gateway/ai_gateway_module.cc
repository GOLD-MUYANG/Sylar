#include "ai_gateway_servlet.h"
#include "ai_gateway_upstream.h"

#include "sylar/application.h"
#include "sylar/env.h"
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

    // 多个上游模型服务实例。
    std::vector<AiGatewayProviderConfig> providers;

    // 多实例选择策略。
    std::string load_balance = "ROUND_ROBIN";

    // 请求上游模型服务的超时时间，单位：毫秒
    uint64_t request_timeout_ms = 800;

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

        config.load_balance = node["load_balance"].as<std::string>(config.load_balance);

        // 读取多个上游模型服务实例。
        YAML::Node providers = node["providers"];
        if (providers && providers.IsSequence())
        {
            for (const auto &provider : providers)
            {
                AiGatewayProviderConfig item;
                item.name = provider["name"].as<std::string>(item.name);
                item.url = provider["url"].as<std::string>(item.url);
                item.weight = provider["weight"].as<uint32_t>(item.weight);
                config.providers.push_back(item);
            }
        }

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

        // 启用模块时，server_name 和至少一个 provider 都必须配置。
        if (config.server_name.empty() || config.providers.empty())
        {
            SYLAR_LOG_ERROR(g_logger) << "ai gateway requires server_name and providers";
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

        // 由独立组件校验 Provider URL 并创建多实例客户端。
        std::string upstream_error;
        sylar::http::HttpLoadBalanceClient::ptr client =
            CreateLoadBalanceClient(config.providers, config.load_balance, &upstream_error);
        if (!client)
        {
            SYLAR_LOG_ERROR(g_logger) << "ai gateway invalid providers error=" << upstream_error;
            return false;
        }

        // 构造一个上游 POST 调用函数
        // AiGatewayServlet 不直接依赖 HttpClient，而是通过这个函数回调调用上游
        //
        // 捕获 client：
        //   保证 servlet 调用时还能访问这个 HTTP 客户端
        //
        // 捕获 config：使用里面的 request_timeout_ms。
        AiGatewayServlet::UpstreamPost upstream = [client, config](const std::string &body)
        {
            // G3 只允许在不同 Provider 间故障转移，不会对同一实例重复提交。
            sylar::http::HttpRetryOptions retry_options;
            retry_options.retry_non_idempotent = true;
            return client->post(
                "/v1/chat/completions",
                sylar::http::HttpRequestOptions::FromTimeout(config.request_timeout_ms),
                retry_options, {{"Content-Type", "application/json"}}, body);
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
