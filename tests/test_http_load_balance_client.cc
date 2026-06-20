#include "sylar/hook.h"
#include "sylar/http/http_circuit_breaker.h"
#include "sylar/http/http_load_balance_client.h"
#include "sylar/iomanager.h"
#include "sylar/log.h"

#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

// 使用 Sylar 根日志器输出测试信息和错误信息。
static sylar::Logger::ptr g_logger = SYLAR_LOG_ROOT();

// 记录测试失败次数。
// 使用 atomic 是因为部分测试会涉及多线程 / IOManager 调度。
static std::atomic<int> g_failures(0);

/**
 * @brief 简单的断言宏：判断表达式是否为 true。
 *
 * 如果 expr 为 false：
 * - 失败次数 +1
 * - 打印失败表达式和所在行号
 *
 * 这里没有直接 abort，是为了让后续测试继续执行，
 * 最后通过 g_failures 统一决定程序返回值。
 */
#define EXPECT_TRUE(expr)                                                                          \
    do                                                                                             \
    {                                                                                              \
        if (!(expr))                                                                               \
        {                                                                                          \
            ++g_failures;                                                                          \
            SYLAR_LOG_ERROR(g_logger) << "EXPECT_TRUE failed: " #expr << " line=" << __LINE__;     \
        }                                                                                          \
    } while (0)

/**
 * @brief 简单的相等断言宏。
 *
 * lhs 和 rhs 会先保存到临时变量，避免表达式被重复求值。
 * 如果二者不相等，则记录失败并打印实际值。
 */
#define EXPECT_EQ(lhs, rhs)                                                                        \
    do                                                                                             \
    {                                                                                              \
        auto _lhs = (lhs);                                                                         \
        auto _rhs = (rhs);                                                                         \
        if (!(_lhs == _rhs))                                                                       \
        {                                                                                          \
            ++g_failures;                                                                          \
            SYLAR_LOG_ERROR(g_logger) << "EXPECT_EQ failed: " #lhs "=" << _lhs << " " #rhs "="     \
                                      << _rhs << " line=" << __LINE__;                             \
        }                                                                                          \
    } while (0)

// 匿名 namespace：限制这些测试辅助类和函数只在当前 cpp 文件内可见。
namespace
{

/**
 * @brief 一个极简 HTTP 测试服务器。
 *
 * 作用：
 * - 监听 127.0.0.1 的随机端口；
 * - 收到任意 HTTP 请求后，返回固定 body；
 * - body 内容由构造函数传入的 name 决定；
 * - 可以设置响应延迟，用来模拟慢节点。
 *
 * 例如：
 * NamedHttpServer first("first");
 * 访问这个 server 时，响应 body 就是 "first"。
 */
class NamedHttpServer
{
public:
    /**
     * @param name 响应 body 的内容，用来区分请求打到了哪个 server。
     * @param response_delay_ms 响应前延迟多少毫秒，用来模拟慢服务。
     */
    explicit NamedHttpServer(const std::string &name, uint32_t response_delay_ms = 0)
        : m_name(name), m_responseDelayMs(response_delay_ms)
    {
        // 创建 TCP socket。
        // socket_f 是 Sylar hook 包装后的 socket 函数。
        m_fd = socket_f(AF_INET, SOCK_STREAM, 0);
        EXPECT_TRUE(m_fd >= 0);

        // 允许端口快速复用，避免测试频繁启动时 bind 失败。
        int reuse = 1;
        setsockopt_f(m_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));

        // IPv4 地址。
        addr.sin_family = AF_INET;

        // 监听本机回环地址 127.0.0.1。
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        // 端口设置为 0，表示让操作系统自动分配一个空闲端口。
        addr.sin_port = 0;

        // 绑定地址和端口。
        int rt = ::bind(m_fd, (const sockaddr *)&addr, sizeof(addr));
        EXPECT_EQ(rt, 0);

        // 开始监听连接，backlog 为 128。
        rt = ::listen(m_fd, 128);
        EXPECT_EQ(rt, 0);

        // 获取系统实际分配的端口。
        socklen_t len = sizeof(addr);
        rt = ::getsockname(m_fd, (sockaddr *)&addr, &len);
        EXPECT_EQ(rt, 0);

        // 网络字节序转主机字节序，保存端口。
        m_port = ntohs(addr.sin_port);

        // 启动 accept 线程，持续接收客户端连接。
        m_thread = std::thread([this]() { accept_loop(); });
    }

    /**
     * @brief 析构时关闭监听 socket，并等待 accept 线程退出。
     */
    ~NamedHttpServer()
    {
        // 通知 accept_loop 停止。
        m_stop = true;

        if (m_fd >= 0)
        {
            // shutdown 用来唤醒可能阻塞在 accept 上的线程。
            ::shutdown(m_fd, SHUT_RDWR);

            // 关闭监听 socket。
            close_f(m_fd);
            m_fd = -1;
        }

        // 等待 accept 线程结束。
        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }

    /**
     * @brief 返回当前测试服务器监听的端口。
     */
    uint16_t getPort() const
    {
        return m_port;
    }

private:
    /**
     * @brief 循环接收客户端连接。
     *
     * 每接收到一个连接，就启动一个 detached 线程处理请求。
     */
    void accept_loop()
    {
        while (!m_stop)
        {
            int client = accept_f(m_fd, nullptr, nullptr);
            if (client < 0)
            {
                // 如果不是主动停止，说明可能是临时 accept 失败。
                // 稍微睡眠一下，避免空转占 CPU。
                if (!m_stop)
                {
                    usleep(1000);
                }
                continue;
            }

            // 每个客户端连接交给独立线程处理。
            // 注意：这里 detach 了，析构时不会等待这些工作线程结束。
            std::thread(&NamedHttpServer::handle_client, this, client).detach();
        }
    }

    /**
     * @brief 处理单个 HTTP 客户端连接。
     *
     * 逻辑：
     * 1. 读取请求，直到读到 HTTP 头结束标志 "\r\n\r\n"；
     * 2. 拼接一个 HTTP/1.1 200 OK 响应；
     * 3. body 写入 m_name；
     * 4. 可选延迟；
     * 5. 发送响应并关闭连接。
     */
    void handle_client(int client)
    {
        std::string req;
        char buf[1024];

        // 读取 HTTP 请求头。
        // 这里只判断 "\r\n\r\n"，不解析具体 method/path/header。
        while (!m_stop && req.find("\r\n\r\n") == std::string::npos)
        {
            int rt = recv_f(client, buf, sizeof(buf), 0);
            if (rt <= 0)
            {
                close_f(client);
                return;
            }

            req.append(buf, rt);
        }

        // 构造最简单的 HTTP 响应。
        // body 是 m_name，比如 "first"、"second"、"slow"。
        std::stringstream ss;
        ss << "HTTP/1.1 200 OK\r\n"
           << "connection: close\r\n"
           << "content-length: " << m_name.size() << "\r\n\r\n"
           << m_name;

        std::string rsp = ss.str();

        // 模拟慢服务。
        if (m_responseDelayMs > 0)
        {
            usleep(m_responseDelayMs * 1000);
        }

        // 发送响应并关闭客户端连接。
        send_f(client, rsp.c_str(), rsp.size(), 0);
        close_f(client);
    }

private:
    // 响应 body，用来区分请求命中了哪个测试服务。
    std::string m_name;

    // 响应延迟，单位毫秒。
    uint32_t m_responseDelayMs = 0;

    // 监听 socket fd。
    int m_fd = -1;

    // 实际监听端口。
    uint16_t m_port = 0;

    // 停止标志。
    std::atomic<bool> m_stop{false};

    // accept 线程。
    std::thread m_thread;
};

/**
 * @brief 创建一个 HttpEndpoint。
 *
 * Endpoint 表示一个后端服务实例。
 *
 * @param port 后端服务端口。
 * @param weight 权重，供 WEIGHTED_ROUND_ROBIN 使用。
 */
sylar::http::HttpEndpoint::ptr create_endpoint(uint16_t port, uint32_t weight = 1)
{
    return sylar::http::HttpEndpoint::Create(
        "127.0.0.1",                         // host
        port,                                // port
        false,                               // ssl，false 表示 HTTP，不是 HTTPS
        weight,                              // endpoint 权重
        sylar::http::HttpEndpointStatus::UP, // 初始状态为 UP
        "",                                  // vhost，这里不用
        2,     // 连接池最大连接数，具体含义看 HttpEndpoint::Create 定义
        30000, // 最大存活时间 / 超时配置，取决于你的实现
        10     // 连接池相关参数，取决于你的实现
    );
}

/**
 * @brief 检查 HTTP 请求是否成功，并且响应 body 是否等于预期值。
 *
 * @param result client->get() 返回的结果。
 * @param body 期望的响应 body。
 */
void expect_ok_body(const sylar::http::HttpResult::ptr &result, const std::string &body)
{
    // 请求结果对象不能为空。
    EXPECT_TRUE(result != nullptr);
    if (!result)
    {
        return;
    }

    // 请求必须成功。
    EXPECT_EQ(result->result, (int)sylar::http::HttpResult::Error::OK);

    // HTTP response 不能为空。
    EXPECT_TRUE(result->response != nullptr);
    if (!result->response)
    {
        return;
    }

    // 检查响应 body 是否符合预期。
    EXPECT_EQ(result->response->getBody(), body);
}

/**
 * @brief 测试 ROUND_ROBIN 是否能在多个 endpoint 之间轮流切换。
 *
 * 预期：
 * 第一次请求 -> first
 * 第二次请求 -> second
 * 第三次请求 -> first
 */
void test_round_robin_rotates_between_endpoints()
{
    NamedHttpServer first("first");
    NamedHttpServer second("second");

    // 等待服务线程启动，避免刚创建 server 就立刻请求导致偶发失败。
    usleep(50 * 1000);

    std::vector<sylar::http::HttpEndpoint::ptr> endpoints;
    endpoints.push_back(create_endpoint(first.getPort()));
    endpoints.push_back(create_endpoint(second.getPort()));

    auto client = sylar::http::HttpLoadBalanceClient::Create(
        endpoints, sylar::http::HttpLoadBalanceStrategy::ROUND_ROBIN);

    EXPECT_TRUE(client != nullptr);
    if (!client)
    {
        return;
    }

    auto first_result = client->get("/ok", 1000);
    auto second_result = client->get("/ok", 1000);
    auto third_result = client->get("/ok", 1000);

    expect_ok_body(first_result, "first");
    expect_ok_body(second_result, "second");
    expect_ok_body(third_result, "first");
}

/**
 * @brief 测试状态为 DOWN 的 endpoint 会被跳过。
 *
 * 这里故意创建两个 endpoint：
 * - 第一个 endpoint 状态是 DOWN；
 * - 第二个 endpoint 状态是 UP；
 *
 * 即使第一个 endpoint 的端口也是可访问的，
 * 负载均衡选择时也应该跳过它。
 */
void test_down_endpoint_is_skipped()
{
    NamedHttpServer available("available");
    usleep(50 * 1000);

    auto down =
        sylar::http::HttpEndpoint::Create("127.0.0.1", available.getPort(), false, 1,
                                          sylar::http::HttpEndpointStatus::DOWN, "", 2, 30000, 10);

    std::vector<sylar::http::HttpEndpoint::ptr> endpoints;
    endpoints.push_back(down);
    endpoints.push_back(create_endpoint(available.getPort()));

    auto client = sylar::http::HttpLoadBalanceClient::Create(
        endpoints, sylar::http::HttpLoadBalanceStrategy::ROUND_ROBIN);

    EXPECT_TRUE(client != nullptr);
    if (!client)
    {
        return;
    }

    auto result = client->get("/ok", 1000);

    // 只应该请求到 UP 的 available endpoint。
    expect_ok_body(result, "available");
}

/**
 * @brief 测试 RANDOM 策略在有可用 endpoint 时可以成功请求。
 *
 * 这里只有一个 endpoint，所以随机选择也只能选中它。
 * 这个测试不验证随机分布，只验证 RANDOM 策略基本可用。
 */
void test_random_strategy_uses_available_endpoint()
{
    NamedHttpServer available("random-ok");
    usleep(50 * 1000);

    std::vector<sylar::http::HttpEndpoint::ptr> endpoints;
    endpoints.push_back(create_endpoint(available.getPort()));

    auto client = sylar::http::HttpLoadBalanceClient::Create(
        endpoints, sylar::http::HttpLoadBalanceStrategy::RANDOM);

    EXPECT_TRUE(client != nullptr);
    if (!client)
    {
        return;
    }

    auto result = client->get("/ok", 1000);
    expect_ok_body(result, "random-ok");
}

/**
 * @brief 测试 WEIGHTED_ROUND_ROBIN 是否按权重分配请求。
 *
 * first 权重 = 2
 * second 权重 = 1
 *
 * 如果实现方式是简单展开权重，
 * 那么选择序列应该类似：
 * first, first, second, first, first, second...
 *
 * 当前测试只检查前 4 次：
 * first -> first -> second -> first
 */
void test_weighted_round_robin_uses_endpoint_weight()
{
    NamedHttpServer first("first");
    NamedHttpServer second("second");
    usleep(50 * 1000);

    std::vector<sylar::http::HttpEndpoint::ptr> endpoints;
    endpoints.push_back(create_endpoint(first.getPort(), 2));
    endpoints.push_back(create_endpoint(second.getPort(), 1));

    auto client = sylar::http::HttpLoadBalanceClient::Create(
        endpoints, sylar::http::HttpLoadBalanceStrategy::WEIGHTED_ROUND_ROBIN);

    EXPECT_TRUE(client != nullptr);
    if (!client)
    {
        return;
    }

    expect_ok_body(client->get("/ok", 1000), "first");
    expect_ok_body(client->get("/ok", 1000), "first");
    expect_ok_body(client->get("/ok", 1000), "second");
    expect_ok_body(client->get("/ok", 1000), "first");
}

/**
 * @brief 测试 LEAST_CONNECTION 是否会选择当前连接数更少的 endpoint。
 *
 * slow server 响应延迟 200ms。
 * fast server 立即响应。
 *
 * 测试流程：
 * 1. 异步发起一个请求，让 slow endpoint 处于忙碌状态；
 * 2. 等 50ms，确保 slow 请求还没完成；
 * 3. 再发起第二个请求；
 * 4. LEAST_CONNECTION 应该选择 fast endpoint。
 */
void test_least_connection_uses_less_busy_endpoint()
{
    NamedHttpServer slow("slow", 200);
    NamedHttpServer fast("fast");
    usleep(50 * 1000);

    std::vector<sylar::http::HttpEndpoint::ptr> endpoints;
    endpoints.push_back(create_endpoint(slow.getPort()));
    endpoints.push_back(create_endpoint(fast.getPort()));

    auto client = sylar::http::HttpLoadBalanceClient::Create(
        endpoints, sylar::http::HttpLoadBalanceStrategy::LEAST_CONNECTION);

    EXPECT_TRUE(client != nullptr);
    if (!client)
    {
        return;
    }

    sylar::http::HttpResult::ptr slow_result;
    sylar::http::HttpResult::ptr fast_result;

    // 获取当前 IOManager。
    // main 里会创建 IOManager，并把 run_tests 调度进去，
    // 所以这里能拿到当前线程所属的 IOManager。
    sylar::IOManager *iom = sylar::IOManager::GetThis();

    // 异步发起第一个请求。
    // 由于 endpoints 中 slow 在前，第一次通常会选中 slow。
    iom->schedule([&]() { slow_result = client->get("/ok", 1000); });

    // 等待 slow 请求进入处理中。
    usleep(50 * 1000);

    // 第二个请求应该选择连接数更少的 fast endpoint。
    fast_result = client->get("/ok", 1000);

    // 等待 slow 请求完成。
    usleep(250 * 1000);

    expect_ok_body(fast_result, "fast");
    expect_ok_body(slow_result, "slow");
}

/**
 * @brief 测试健康检查会把失败 endpoint 标记为 DOWN。
 *
 * 测试流程：
 * 1. 创建 available server，保持可用；
 * 2. 创建 unavailable server，拿到端口后马上销毁；
 * 3. 对两个 endpoint 执行 checkHealth；
 * 4. 可用节点应该是 UP；
 * 5. 不可用节点应该是 DOWN；
 * 6. 后续请求应该只打到 available。
 */
void test_health_check_marks_failed_endpoint_down()
{
    NamedHttpServer available("available");

    // 用 shared_ptr 包一层，是为了后面可以主动 reset，
    // 模拟服务下线。
    std::shared_ptr<NamedHttpServer> unavailable(new NamedHttpServer("unavailable"));

    // 先记录 unavailable server 的端口。
    uint16_t unavailable_port = unavailable->getPort();

    usleep(50 * 1000);

    std::vector<sylar::http::HttpEndpoint::ptr> endpoints;

    auto up = create_endpoint(available.getPort());
    auto down = create_endpoint(unavailable_port);

    endpoints.push_back(up);
    endpoints.push_back(down);

    auto client = sylar::http::HttpLoadBalanceClient::Create(
        endpoints, sylar::http::HttpLoadBalanceStrategy::ROUND_ROBIN);

    EXPECT_TRUE(client != nullptr);
    if (!client)
    {
        return;
    }

    // 销毁 unavailable server，模拟这个端口已经不可用了。
    unavailable.reset();

    // 对所有 endpoint 发起健康检查。
    // 超时时间 200ms，避免不可用节点卡太久。
    size_t available_count =
        client->checkHealth("/ok", sylar::http::HttpRequestOptions::FromTimeout(200));

    // 只剩一个节点可用。
    EXPECT_EQ(available_count, (size_t)1);

    // available 应该仍然是 UP。
    EXPECT_EQ((int)up->getStatus(), (int)sylar::http::HttpEndpointStatus::UP);

    // unavailable 对应的 endpoint 应该被标记为 DOWN。
    EXPECT_EQ((int)down->getStatus(), (int)sylar::http::HttpEndpointStatus::DOWN);

    // 请求应该跳过 DOWN 节点，只访问 available。
    expect_ok_body(client->get("/ok", 1000), "available");
}

/**
 * @brief 测试 endpoint 级别的并发限制。
 *
 * 测试目标：
 * 如果某个 endpoint 当前已有 1 个请求正在处理，
 * 并且 max_endpoint_concurrency = 1，
 * 那么第二个请求应该被限流，返回 RATE_LIMITED。
 */
void test_endpoint_concurrency_limit_rejects_busy_endpoint()
{
    NamedHttpServer slow("slow", 200);
    usleep(50 * 1000);

    std::vector<sylar::http::HttpEndpoint::ptr> endpoints;
    endpoints.push_back(create_endpoint(slow.getPort()));

    // 配置 endpoint 最大并发数为 1。
    sylar::http::HttpConcurrencyLimitOptions limits;
    limits.max_endpoint_concurrency = 1;

    auto client = sylar::http::HttpLoadBalanceClient::Create(
        endpoints, sylar::http::HttpLoadBalanceStrategy::ROUND_ROBIN, limits);

    EXPECT_TRUE(client != nullptr);
    if (!client)
    {
        return;
    }

    sylar::http::HttpResult::ptr slow_result;

    sylar::IOManager *iom = sylar::IOManager::GetThis();

    // 第一个请求打到 slow endpoint。
    // slow server 会延迟 200ms，所以这段时间内 endpoint 会保持 busy。
    iom->schedule([&]() { slow_result = client->get("/ok", 1000); });

    // 等待第一个请求进入处理中。
    usleep(50 * 1000);

    // 第二个请求因为 endpoint 并发数已经达到 1，应该被限流。
    auto limited_result = client->get("/ok", 1000);

    EXPECT_TRUE(limited_result != nullptr);
    if (limited_result)
    {
        EXPECT_EQ(limited_result->result, (int)sylar::http::HttpResult::Error::RATE_LIMITED);
    }

    // 等待第一个慢请求完成。
    usleep(250 * 1000);

    // 第一个请求本身应该成功。
    expect_ok_body(slow_result, "slow");
}

/**
 * @brief 测试 endpoint 级别的 QPS 限制。
 *
 * 测试目标：
 * 如果 max_endpoint_qps = 1，那么同一个 endpoint 的第二个立即请求应该被限流。
 */
void test_endpoint_qps_limit_rejects_fast_second_request()
{
    NamedHttpServer server("qps-ok");
    usleep(50 * 1000);

    std::vector<sylar::http::HttpEndpoint::ptr> endpoints;
    endpoints.push_back(create_endpoint(server.getPort()));

    sylar::http::HttpConcurrencyLimitOptions limits;
    limits.max_endpoint_qps = 1;

    auto client = sylar::http::HttpLoadBalanceClient::Create(
        endpoints, sylar::http::HttpLoadBalanceStrategy::ROUND_ROBIN, limits);

    EXPECT_TRUE(client != nullptr);
    if (!client)
    {
        return;
    }

    expect_ok_body(client->get("/ok", 1000), "qps-ok");

    auto limited_result = client->get("/ok", 1000);
    EXPECT_TRUE(limited_result != nullptr);
    if (limited_result)
    {
        EXPECT_EQ(limited_result->result, (int)sylar::http::HttpResult::Error::RATE_LIMITED);
    }
}

/**
 * @brief 测试负载均衡客户端接入 endpoint 熔断。
 *
 * 测试目标：
 * - 第一次请求打到不可用 endpoint，触发连接失败；
 * - 熔断阈值设置为 1，所以该 endpoint 进入 OPEN；
 * - 第二次请求应该被熔断器快速拒绝，返回 CIRCUIT_OPEN。
 */
void test_circuit_breaker_rejects_open_endpoint()
{
    uint16_t unavailable_port = 0;
    {
        NamedHttpServer unavailable("unavailable");
        unavailable_port = unavailable.getPort();
    }

    std::vector<sylar::http::HttpEndpoint::ptr> endpoints;
    endpoints.push_back(create_endpoint(unavailable_port));

    sylar::http::HttpConcurrencyLimitOptions limits;

    sylar::http::HttpCircuitBreakerOptions circuit_options;
    circuit_options.enabled = true;
    circuit_options.consecutive_failure_threshold = 1;
    circuit_options.open_timeout_ms = 1000;

    auto client = sylar::http::HttpLoadBalanceClient::Create(
        endpoints,
        sylar::http::HttpLoadBalanceStrategy::ROUND_ROBIN,
        limits,
        circuit_options);

    EXPECT_TRUE(client != nullptr);
    if (!client)
    {
        return;
    }

    auto first_result = client->get("/ok", sylar::http::HttpRequestOptions::FromTimeout(100));
    EXPECT_TRUE(first_result != nullptr);
    if (first_result)
    {
        EXPECT_TRUE(first_result->result != (int)sylar::http::HttpResult::Error::CIRCUIT_OPEN);
    }

    auto second_result = client->get("/ok", sylar::http::HttpRequestOptions::FromTimeout(100));
    EXPECT_TRUE(second_result != nullptr);
    if (second_result)
    {
        EXPECT_EQ(second_result->result, (int)sylar::http::HttpResult::Error::CIRCUIT_OPEN);
    }
}

/**
 * @brief 依次运行所有测试用例。
 */
void run_tests()
{
    test_round_robin_rotates_between_endpoints();
    test_down_endpoint_is_skipped();
    test_random_strategy_uses_available_endpoint();
    test_weighted_round_robin_uses_endpoint_weight();
    test_least_connection_uses_less_busy_endpoint();
    test_health_check_marks_failed_endpoint_down();
    test_endpoint_concurrency_limit_rejects_busy_endpoint();
    test_endpoint_qps_limit_rejects_fast_second_request();
    test_circuit_breaker_rejects_open_endpoint();

    SYLAR_LOG_INFO(g_logger) << "run_tests over";
}

} // namespace

/**
 * @brief 测试程序入口。
 *
 * 创建一个 4 线程 IOManager，
 * 把 run_tests 调度到 IOManager 中执行。
 *
 * IOManager 析构时会等待调度任务结束。
 *
 * 返回值：
 * - 0：所有测试通过；
 * - 1：至少一个断言失败。
 */
int main(int argc, char **argv)
{
    {
        sylar::IOManager iom(4);
        iom.schedule(run_tests);
    }

    return g_failures.load() == 0 ? 0 : 1;
}
