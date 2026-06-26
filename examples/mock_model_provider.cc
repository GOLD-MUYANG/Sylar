#include "modules/ai_gateway/ai_gateway_protocol.h"
#include "sylar/address.h"
#include "sylar/http/http_server.h"
#include "sylar/iomanager.h"
#include "sylar/log.h"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <unistd.h>

namespace
{

// 使用 root logger 输出日志。
// 放在匿名命名空间里，表示只在当前 cpp 文件内可见。
static sylar::Logger::ptr g_logger = SYLAR_LOG_ROOT();

// Mock Provider 的启动参数。
// 每启动一个 mock_model_provider 进程，就通过命令行指定这些参数。
struct ProviderOptions
{
    uint16_t port = 0;     // 监听端口，例如 9001
    uint64_t delay_ms = 0; // 慢响应模式下的延迟时间，单位毫秒
    std::string name;      // provider 名称，例如 mock-a / mock-b
    std::string mode;      // 工作模式：normal / slow / error
};

// 从命令行参数中读取指定参数名后面的值。
// 例如：
//   --port 9001
// 调用：
//   ReadArgument(argc, argv, "--port", &value)
// 会把 value 设置为 "9001"。
bool ReadArgument(int argc, char **argv, const std::string &name, std::string *value)
{
    // 从 argv[1] 开始遍历，因为 argv[0] 是程序名。
    // i + 1 < argc 是为了保证 argv[i + 1] 不越界。
    for (int i = 1; i + 1 < argc; ++i)
    {
        // 找到指定参数名后，取它后面的一个参数作为值。
        if (argv[i] == name)
        {
            *value = argv[i + 1];
            return true;
        }
    }

    // 没找到指定参数。
    return false;
}

// 解析 mock provider 的命令行参数。
// 必须提供：
//   --port <1-65535>
//   --name <name>
//   --mode normal|slow|error
//
// 可选提供：
//   --delay-ms <ms>
bool ParseOptions(int argc, char **argv, ProviderOptions *options)
{
    std::string value;

    // port 是必填参数。
    if (!ReadArgument(argc, argv, "--port", &value))
    {
        return false;
    }

    // 把端口字符串转换成 unsigned long。
    // 这里没有严格检查非法字符，例如 "123abc" 会被 strtoul 解析成 123。
    unsigned long port = strtoul(value.c_str(), nullptr, 10);

    // 校验端口范围，并继续读取 name 和 mode。
    // 任意一个必填参数缺失，都认为解析失败。
    if (port == 0 || port > 65535 || !ReadArgument(argc, argv, "--name", &options->name) ||
        !ReadArgument(argc, argv, "--mode", &options->mode))
    {
        return false;
    }

    // 保存合法端口。
    options->port = static_cast<uint16_t>(port);

    // delay-ms 是可选参数。
    // 只有 slow 模式才会用到这个值。
    if (ReadArgument(argc, argv, "--delay-ms", &value))
    {
        options->delay_ms = strtoull(value.c_str(), nullptr, 10);
    }

    // 只允许三种模式：
    // normal：正常返回 mock completion
    // slow：先 sleep 一段时间，再正常返回
    // error：返回 503 错误
    return options->mode == "normal" || options->mode == "slow" || options->mode == "error";
}

// 启动一个本地 Mock Model Provider。
// 它本质上是一个 HTTP Server，只处理 /v1/chat/completions 这个接口。
void StartProvider(const ProviderOptions &options)
{
    // 创建 HTTP Server。
    // 参数 true 一般表示 keepalive，具体含义取决于 Sylar HttpServer 实现。
    sylar::http::HttpServer::ptr server(new sylar::http::HttpServer(true));

    // 构造监听地址，例如：
    //   127.0.0.1:9001
    sylar::Address::ptr address =
        sylar::Address::LookupAnyIPAddress("127.0.0.1:" + std::to_string(options.port));

    // 绑定监听地址。
    // 如果地址解析失败，或者端口被占用，bind 会失败。
    if (!address || !server->bind(address))
    {
        SYLAR_LOG_ERROR(g_logger) << "mock provider bind failed port=" << options.port;
        return;
    }

    // 设置服务名，主要用于日志、调试、排查时区分不同 provider。
    server->setName("mock-model-provider/" + options.name);

    // 健康检查路由：
    // 网关启动时用它区分 A/B 正常和 C 未启动，不参与 Chat Completions 业务语义。
    server->getServletDispatch()->addServlet(
        "/health",
        [options](sylar::http::HttpRequest::ptr, sylar::http::HttpResponse::ptr response,
                  sylar::http::HttpSession::ptr)
        {
            response->setHeader("Content-Type", "application/json");
            if (options.mode == "error")
            {
                response->setStatus(sylar::http::HttpStatus::SERVICE_UNAVAILABLE);
                response->setBody(sylar::ai_gateway::BuildErrorResponse(
                    "Mock Provider 健康检查失败", "server_error", "MOCK_PROVIDER_ERROR"));
                return 0;
            }

            response->setBody("{\"status\":\"ok\"}");
            return 0;
        });

    // 注册 HTTP 路由：
    // 当请求路径是 /v1/chat/completions 时，执行这个 lambda。
    //
    // 这个路径故意模拟 OpenAI Chat Completions 接口。
    server->getServletDispatch()->addServlet(
        "/v1/chat/completions",
        [options](sylar::http::HttpRequest::ptr request, sylar::http::HttpResponse::ptr response,
                  sylar::http::HttpSession::ptr)
        {
            // 仅用于本地演示：证明网关实际选择了哪个 Provider。
            std::cerr << "mock provider received request name=" << options.name
                      << " path=" << request->getPath() << std::endl;

            // 统一返回 JSON。
            response->setHeader("Content-Type", "application/json");

            // slow 模式：
            // 用 usleep 人为制造响应延迟，用来测试网关的超时、重试、熔断逻辑。
            if (options.mode == "slow" && options.delay_ms > 0)
            {
                // usleep 的单位是微秒，所以毫秒要乘以 1000。
                usleep(options.delay_ms * 1000);
            }

            // error 模式：
            // 直接返回 503，模拟模型服务不可用。
            if (options.mode == "error")
            {
                response->setStatus(sylar::http::HttpStatus::SERVICE_UNAVAILABLE);

                // 构造统一格式的错误响应。
                response->setBody(sylar::ai_gateway::BuildErrorResponse(
                    "Mock Provider 被配置为失败", "server_error", "MOCK_PROVIDER_ERROR"));

                return 0;
            }

            // normal 模式，或者 slow 模式 sleep 结束后：
            // 返回一个 mock 的模型响应。
            response->setBody(
                sylar::ai_gateway::BuildMockProviderResponse(options.name, "mock completion"));

            return 0;
        });

    // 启动 HTTP Server，开始接受请求。
    server->start();
}

} // namespace

int main(int argc, char **argv)
{
    ProviderOptions options;

    // 解析命令行参数。
    // 参数不合法时，打印用法并退出。
    if (!ParseOptions(argc, argv, &options))
    {
        SYLAR_LOG_ERROR(g_logger) << "usage: mock_model_provider --port <1-65535> --name <name> "
                                     "--mode normal|slow|error [--delay-ms <ms>]";
        return 1;
    }

    // 创建 IOManager。
    //
    // 参数含义大概率是：
    //   1：使用 1 个工作线程
    //   true：是否把当前线程也纳入调度
    //   "mock-provider"：调度器名称
    sylar::IOManager iom(1, true, "mock-provider");

    // 把 StartProvider 投递到 IOManager 中执行。
    // HTTP Server 的启动和事件监听都交给 Sylar 的调度器处理。
    iom.schedule(std::bind(StartProvider, options));

    // 等待 IOManager 中的任务执行完成。
    //
    // 注意：
    // 如果 server->start() 成功启动并注册了监听事件，
    // IOManager 通常不会立刻退出，而是继续处理 socket 事件。
    iom.stop();

    return 0;
}
