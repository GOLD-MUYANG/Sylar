# AI 模型调用网关方案

> 状态：G0、G1、G2、G3、G4、G5 已实现并完成本地回归验证。本文件同时记录已落地的文件、验证边界和后续切片，避免把规划能力误写成现有能力。
>
> 目标：在现有 Sylar HTTP 客户端能力之上，做一个**本地可运行、可注入故障、兼容 Chat Completions 非流式基础请求/响应形状的 AI 模型调用网关**。它不试图实现大模型推理；它要展示的是网关如何稳定地调用多个模型提供者。

## 1. 项目要解决什么问题

用户向网关发送一次“生成文本”请求。网关背后配置多个模型提供者实例：正常时分摊请求，某个实例超时、报错或被限流时自动切换；持续失败的实例被熔断，恢复后再被谨慎放回流量中。

```text
调用方（curl / 演示页面）
        |
        v
AI Gateway（Sylar Application + ai_gateway 插件）
        |
        | G4：HttpLoadBalanceClient（负载均衡 + 故障转移 + limiter/breaker）
        +----> Mock Model A :19001
        +----> Mock Model B :19002
        +----> Mock Model C（可选）
```

这里的 `Mock Model` 是本地的“模型提供者模拟服务”，按配置返回固定文本、延迟或错误。第一版不用真实 OpenAI、DeepSeek 等 API，也不需要 API Key、外网或 GPU。

这样做的原因是：项目要诚实地展示**服务调用治理**，而不是把模型厂商的推理能力误写成自己的能力。

## 2. 为什么它能用到现有能力

| 现有模块 | 在网关中的位置 | 具体作用 |
| --- | --- | --- |
| `HttpConnectionPool` | 到每个模型提供者的连接 | 复用连接，避免每次请求重新建连 |
| `HttpClient` | 单个模型实例请求 | 超时、重试和错误归一化 |
| `HttpLoadBalanceClient` | 多个模型实例间 | 选择实例，并在一次调用内故障转移 |
| `HttpConcurrencyLimiter` | 出站调用准入边界 | 在建立下游请求前限制全部/某个模型服务/某个实例的并发和 QPS，避免网关和供应商被突发流量拖垮 |
| `HttpCircuitBreaker` | 每个模型实例 | 连续失败时暂时隔离实例；冷却后半开探测 |
| `HttpServer` / `ServletDispatch` | 网关入口 | 接收调用方请求，返回统一响应 |
| Worker / IOManager / 协程 | 运行基础 | 承载连接、定时健康检查与并发请求 |

注意：当前 `HttpConcurrencyLimiter` 是**出站客户端限流**，是 G4 必须接入的核心保护；G2 单实例直连链路尚未启用它。它不是“阻止大量用户进入网关”的入口限流。模型供应商自己的 429/排队只能保护供应商，而在收到该结果前，网关已经消耗了自身的协程、连接、内存和等待时间。

G4 的目标策略是**有界准入，不做无界排队**：有名额就立即转发；某个实例达到上限时由负载均衡器尝试其他实例；所有候选实例都无法准入时快速返回 `RATE_LIMITED`。这比无限等待更能保护延迟和吞吐。按用户身份的入口配额不是第一版范围，但以后如需加入，也必须作为独立能力，不能混进现有 limiter 的职责里。

## 3. 当前已经具备什么，还缺什么

### 已具备

- 单实例 HTTP 请求、连接池、超时、重试和错误归一化。
- 多实例选择：轮询、随机、加权轮询、最少连接、健康检查。
- 同一次请求内的故障转移。
- 出站并发/QPS 限制，以及 endpoint 级熔断器。
- HTTP Server 和 Servlet 路由分发能力。
- 动态模块的 load/unload、`onServerReady()` 生命周期和失败日志。
- 网关协议层的 JsonCpp 编解码、最小请求校验、统一成功/错误响应。

### G0 前置能力（已实现）

1. **模块生命周期可靠化**：`onLoad()` / `onUnload()` 和 `onServerReady()` 已有实现，`tests/test_module_lifecycle.cc` 覆盖加载、就绪、连接与卸载回调。
2. **业务路由注册扩展点**：`Application::getHttpServer(name)` 只读返回已 bind 的服务；`onServerReady()` 在 `HttpServer::start()` 前执行，因此 `ai_gateway_module` 可向目标 `ServletDispatch` 注册路由，而不接管 Application。
3. **动态插件配置边界**：插件私有配置类型不能注册到核心全局 `Config` 表，否则模块 `dlclose` 后会留下失效对象。网关模块在 `onServerReady()` 直接读取配置目录中的 `ai_gateway.yml`；这保持了模块私有类型与动态卸载边界。通用 `ConfigVar<bool>` 已补充 YAML `true` / `false` 转换测试。

### 新增但保持独立的业务组件

| 组件 | 责任 | 不负责什么 |
| --- | --- | --- |
| `ai_gateway_module` | 装配网关路由、读取网关配置 | 不实现负载均衡或熔断状态机 |
| `AiGatewayServlet` | 校验请求、调用客户端、组装响应 | 不管理 endpoint 选择细节 |
| 协议/响应类型 | 定义最小请求、响应和错误 JSON | 不保存业务数据或用户系统 |
| `mock_model_provider` | 模拟一个模型服务的正常、慢、错误行为 | 不伪装成真实大模型 |
| 网关观测组件 | 统计请求、选中的实例、失败转移、拒绝原因 | 不在第一版接 Prometheus、ELK 等外部系统 |

## 4. 对外接口：Chat Completions 非流式基础形状兼容

第一版只声明兼容 Chat Completions 的**非流式基础请求/响应形状**，不声明完整兼容 OpenAI API，也不另定义 `prompt` 式最小协议：

```http
POST /v1/chat/completions
Content-Type: application/json

{
  "model": "demo-chat",
  "messages": [
    {
      "role": "user",
      "content": "用一句话解释熔断"
    }
  ],
  "temperature": 0.2,
  "max_tokens": 128,
  "stream": false
}
```

正常响应：

```json
{
  "id": "chatcmpl-...",
  "object": "chat.completion",
  "created": 1710000000,
  "model": "demo-chat",
  "choices": [
    {
      "index": 0,
      "message": {
        "role": "assistant",
        "content": "mock completion"
      },
      "finish_reason": "stop"
    }
  ],
  "usage": {
    "prompt_tokens": 0,
    "completion_tokens": 0,
    "total_tokens": 0
  }
}
```

第一版的必测字段只有 `model` 和 `messages`。`messages` 只要求支持 `user`、`system`、`assistant` 三种角色，且每条消息的 `content` 必须是字符串文本；多模态 content、tools/function calling、vision 等不在 G0–G5 范围。

`temperature`、`max_tokens` 等可选字段已做基础类型/合法性校验，Mock Provider 不参与其生成语义。省略 `stream` 或传入 `false` 时返回上述非流式形状；`stream: true` 返回 `INVALID_REQUEST`，不会静默降级。响应中的 `created` 由网关写入 Unix 秒级时间戳；`usage` 固定为 `0` 是 Mock 阶段的占位值，不表示已实现 token 统计。provider 名只存在于网关与 Mock Provider 的内部协议，不作为对外字段返回。

当所有提供者均不可用时，返回 OpenAI 兼容的错误对象，例如：

```json
{
  "error": {
    "message": "没有可用的上游模型服务",
    "type": "server_error",
    "code": "UPSTREAM_UNAVAILABLE"
  }
}
```

响应不得暴露内部地址、堆栈或凭据。

### JSON 的选择

网关协议层确定使用 **jsoncpp**。它只服务于网关请求/响应、Mock Provider 协议和后续真实 Provider 适配器的 JSON 编解码；`HttpRequest`、`HttpResponse`、负载均衡、limiter 与 breaker 继续只处理 HTTP/治理语义，不能散落字符串拼接 JSON 或依赖 jsoncpp。

## 5. 运行时行为：如何演示已有能力

下表是 G0–G5 完成后的演示矩阵；当前已验证正常轮询、A 到 B 的同请求故障转移、对外响应的 Provider 信息脱敏、endpoint QPS 限流切换、全部候选限流时返回 429、熔断后跳过失败 provider、启动时健康检查能力、`max_total_attempts` 总尝试预算，以及只读 `/internal/status` 状态接口。

| 演示场景 | 提供者状态 | 期望行为 | 证明的能力 |
| --- | --- | --- | --- |
| 正常分流 | A、B 正常 | 请求在 A、B 间按策略分配 | 负载均衡 |
| 慢实例 | A 延迟超过超时 | 当前请求改尝试 B | 超时、重试/故障转移 |
| 连续失败 | A 连续返回 5xx | A 被熔断，后续请求绕过 A | 熔断 |
| 恢复探测 | A 恢复正常 | 冷却后只放行少量半开探测请求；探测成功才恢复流量，失败则重新打开 | 半开状态与防止恢复瞬间再次打爆 |
| 下游配额保护 | B 达到并发/QPS 上限 | 网关不再向 B 建立请求，优先尝试其他实例；成功、失败、超时、异常和故障转移后 permit 均释放 | 出站限流与 token 归还 |
| 网关突发保护 | 短时间请求超过全局并发上限 | 不建立无界等待队列，快速拒绝或切换 | 协程、连接、内存保护 |
| 全部不可用 | A、B 都失败 | 返回结构化失败，不崩溃、不无限重试 | 错误边界 |

为了让这些场景可重复，模拟服务应有**仅本地演示使用**的控制方式，例如启动参数或 `/__control` 路由，用于切换 `normal`、`slow`、`error` 三种状态。该控制接口不能作为正式网关 API 对外宣传。

## 6. 推荐实施顺序

每一步都是可独立验证、可单独审阅的小修复/小功能；前一步不通过，不进入后一步。

### G0：补齐插件装配入口（已实现）

- `Application` 提供按服务名查询的只读入口；模块在 `onServerReady()` 获取已 bind 的服务并注册路由。
- `tests/test_ai_gateway_route_registry.cc` 覆盖未知服务不会被隐式创建；真实插件装配由 G2 本地回环验证覆盖。
- 动态插件不注册私有 `ConfigVar`，避免 `dlclose` 后核心配置表保留失效对象。

### G1：实现可控的模拟模型提供者（已实现）

- `examples/mock_model_provider.cc` 构建为独立的 `bin/mock_model_provider`，不进入 `Application`。
- 启动参数：`--port`、`--name`、`--mode normal|slow|error`，slow 可附加 `--delay-ms`；正常响应内部携带 provider 名，error 返回固定 503。
- `tests/test_ai_gateway_protocol.cc` 覆盖 Mock Provider 内部协议；已用 curl 验证本地 provider 返回 JSON。

### G2：实现网关最小直连链路（已实现）

- `modules/ai_gateway/ai_gateway_module.cc` 构建为 `bin/module/libai_gateway_module.so`，从 `ai_gateway.yml` 装配 `POST /v1/chat/completions`。
- `AiGatewayServlet` 只校验协议、调用一份 `HttpClient` 并映射响应；实际下游 URL、超时和目标服务名来自配置。协议 JSON 集中在 `ai_gateway_protocol.*`。
- `tests/test_ai_gateway_protocol.cc` 与 `tests/test_ai_gateway_servlet.cc` 覆盖协议、非法请求、成功映射和上游错误脱敏；已验证成功请求、`stream:true` 的 400 `INVALID_REQUEST`，以及 Provider 停止后的 502 `UPSTREAM_UNAVAILABLE`。

### G3：接入多实例负载均衡和故障转移（已实现）

- `ai_gateway_upstream.*` 负责校验 Provider URL、构造 endpoint 和 `HttpLoadBalanceClient`；模块只读取配置并装配路由，Servlet 不参与选择逻辑。
- `providers` 配置 A、B 两个 endpoint，`load_balance: ROUND_ROBIN` 使正常请求在两者间轮询。
- 网关对本地 Mock 请求显式允许 POST 跨 endpoint 故障转移，但保持 `max_retry = 0`，不会对同一 endpoint 重复提交。
- `tests/test_ai_gateway_load_balance.cc` 断言轮询、A 返回 503 时同请求落到 B，以及响应不暴露 provider 名或地址。
- `scripts/demo_ai_gateway.sh` 启动 A、B，发送两次正常请求后停止 A 并发送第三次请求；Provider 日志证明流量路径。

### G4：接入 limiter、熔断与健康检查（已实现）

- `AiGatewayUpstreamOptions` 承接已有 `HttpConcurrencyLimiter` 与 `HttpCircuitBreaker` 参数，`ai_gateway_module` 从 `ai_gateway.yml` 读取 `limiter`、`circuit_breaker` 和 `health_check`，再交给 `HttpLoadBalanceClient`，网关层不重新实现限流或熔断状态机。
- 准入失败时不排入无界队列：当前候选实例满额则继续尝试其他实例；全部候选都满额时返回 `RATE_LIMITED`，`AiGatewayServlet` 对外映射为 429 和 OpenAI 风格 `RATE_LIMITED` 错误对象。
- endpoint 熔断器可通过配置开启；失败阈值、冷却时间和 `half_open_max_requests` 仍由 `HttpCircuitBreaker` 负责。`HALF_OPEN` 同时探测数默认保持为 `1`。
- `request_deadline_ms` 作为一次业务请求的统一 timeout 输入；`max_total_attempts` 已进入 `HttpRetryOptions`，`HttpLoadBalanceClient` 在跨 endpoint 尝试前统一扣减预算，避免 retry 和 failover 叠加成请求风暴。
- `health_check.enabled` 打开后，模块启动阶段调用 `HttpLoadBalanceClient::checkHealth()`，先标记不可用 endpoint；持续状态展示和人工可读原因由 G5 的 `/internal/status` 提供。
- `tests/test_ai_gateway_load_balance.cc` 覆盖 endpoint QPS 限流后切换、所有候选限流后的 429、熔断打开后跳过失败 provider、健康检查标记失败 provider，以及 `max_total_attempts=1` 时不继续尝试下一 provider。`tests/test_ai_gateway_servlet.cc` 覆盖 `RATE_LIMITED` 不泄露内部错误文案。

### G5：补最小可观测性与演示材料（已实现）

- 新增 `AiGatewayStatusServlet`，通过只读 `/internal/status` 显示每个 provider 的健康状态、熔断状态、in-flight 数、成功/失败/限流计数和最近一次失败原因。
- `HttpLoadBalanceClient` 增加只读状态快照和 endpoint 计数，不把观测逻辑反向塞进 Servlet 转发链路。
- 第一版不实现 Prometheus metrics，也不接 ELK/Grafana。
- `scripts/demo_ai_gateway.sh` 会启动双 Mock Provider 与网关，访问 `/internal/status`，再演示轮询和停止 `mock-a` 后故障转移到 `mock-b`；请求样例位于 `examples/ai_gateway_request.json`。
- 可选：增加极简静态 HTML 页面；它只是演示入口，不应成为项目的重心。

真实 Provider 的扩展已移至同路径的 [真实提供商网关.md](真实提供商网关.md)。它是 G0–G5 本地 Mock 闭环完成后的独立工作项，不能阻塞离线演示或让 CI 依赖外网、真实 API Key。

## 7. 当前 G4 配置

当前已实现的配置位于 `bin/conf/ai_gateway.yml`；独立演示配置位于 `examples/ai_gateway_conf/`：

```yaml
ai_gateway:
  enabled: true
  server_name: ai-gateway
  request_timeout_ms: 800
  request_deadline_ms: 800
  max_total_attempts: 3
  load_balance: ROUND_ROBIN
  limiter:
    max_global_concurrency: 32
    max_service_concurrency: 0
    max_endpoint_concurrency: 8
    max_global_qps: 100
    max_service_qps: 0
    max_endpoint_qps: 20
  circuit_breaker:
    enabled: true
    failure_threshold: 3
    failure_rate_threshold: 0
    open_timeout_ms: 5000
    half_open_max_requests: 1
  health_check:
    enabled: false
    path: /
    timeout_ms: 500
  providers:
    - name: mock-a
      url: http://127.0.0.1:19001
      weight: 1
    - name: mock-b
      url: http://127.0.0.1:19002
      weight: 1
```

`server_name` 必须匹配 `server.yml` 中的 HTTP 服务名。配置职责保持分开：endpoint/权重属于负载均衡；单次下游 I/O 超时受业务请求 deadline 约束；总尝试预算由 `HttpRetryOptions::max_total_attempts` 统一维护；并发/QPS 准入属于 limiter；失败阈值、冷却时间与半开探测并发属于 breaker。真实模型供应商的 token/分钟限额、计费和语义风险见 [真实提供商网关.md](真实提供商网关.md)，不混入本地 Mock 闭环。

## 8. 明确不做的内容

- 不训练、不部署、不宣称实现了大语言模型。
- 第一版不做账号、计费、数据库、会话历史、向量数据库、RAG、流式 token 输出。
- 第一版不接 Kubernetes、服务注册中心、Redis、Prometheus 或真实云厂商。
- 不为了演示而修改 `HttpLoadBalanceClient`、limiter、breaker 的职责边界。
- 不实现无界请求队列，也不增加无限重试；满额或所有候选不可用时必须快速结束请求。
- 不把所有组件放进一个巨大的 `application.cc` 或单个“网关万能类”。

这些可以在 README 的“已知限制与后续方向”中诚实说明；它们不是第一版演示闭环的阻塞项。

## 9. 完成标准

完成 G0–G5 后，应能在不依赖外网、API Key、GPU 的条件下演示：

1. 启动两个本地模拟提供者和一个 Sylar 网关。
2. 向网关发起同一种请求，观察请求被分给不同 provider。
3. 让一个 provider 变慢或失败，观察当前请求故障转移。
4. 连续制造失败，观察熔断打开、冷却、半开和恢复。
5. 将请求压力提高到某个实例或网关全局上限，观察不再建立无界等待，并优先切换其他可用 provider。
6. 通过状态接口和日志解释每次路由、限流拒绝、熔断跳过或故障切换的原因。
7. 每个核心行为都有对应的专用测试，不需要真实模型服务。

当前已满足第 1 项的双 Provider 版本、第 2 项的轮询、第 3 项的 A 到 B 故障转移、第 4 项的熔断/半开底层行为与网关熔断跳过、第 5 项的 limiter 快速拒绝/切换、第 6 项中 `/internal/status` 和日志解释当前路由/限流/熔断/故障切换原因的最小能力，以及第 7 项中 G0–G5 的专用测试要求。

## 10. 运行与验证（G0–G5）

```bash
cmake -S . -B build
cmake --build build --target mock_model_provider ai_gateway_module bin_sylar
./scripts/demo_ai_gateway.sh
```

脚本会启动 `mock-a`、`mock-b` 和网关，输出 Chat Completions 响应，并在退出时清理本地进程。单元验证入口为：

```bash
./bin/test_module_config_timing
./bin/test_ai_gateway_route_registry
./bin/test_ai_gateway_protocol
./bin/test_ai_gateway_servlet
./bin/test_ai_gateway_load_balance
./bin/test_ai_gateway_status
```

`./scripts/demo_ai_gateway.sh` 会启动 `mock-a`、`mock-b` 与网关：前两次请求按轮询分配，脚本停止
`mock-a` 后第三次请求会故障转移到 `mock-b`。脚本会在请求前后打印 `/internal/status`，状态中包含
provider 名、endpoint、健康状态、熔断状态、in-flight 数、成功/失败/限流计数和最近失败原因。Chat Completions
响应不显示 Provider 名；脚本最后打印的本地 Provider 日志用于证明实际路径。
