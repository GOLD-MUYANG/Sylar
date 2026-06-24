# AI 模型调用网关 G3 设计

## 目标

将 AI 网关从单个 Mock Provider 的直连调用升级为多实例调用：正常情况下按轮询在
Provider A、B 之间分流；当前选中的 Provider 不可用时，同一业务请求继续尝试另一个
Provider。调用方仍只看到 Chat Completions 响应，不会得到内部 Provider 名称、地址或错误。

## 范围

G3 只接入已有的 `HttpLoadBalanceClient`，覆盖静态多 Provider 配置、轮询和单请求故障
转移。G4 负责的 limiter、熔断、健康检查、总 deadline 和总尝试预算不在本次改动中。

## 架构与数据流

`AiGatewayModule` 读取 `ai_gateway.yml`，把每条 `providers` 配置解析为一个
`HttpEndpoint`，再创建一个共享的 `HttpLoadBalanceClient`。模块向
`AiGatewayServlet` 注入一个上游 POST 回调；Servlet 保持只处理协议校验、上游结果映射和
错误脱敏的职责。

```text
POST /v1/chat/completions
        |
        v
AiGatewayServlet
        |
        v
HttpLoadBalanceClient (ROUND_ROBIN)
   |                          |
   v                          v
Mock Provider A           Mock Provider B
```

`HttpLoadBalanceClient` 在一次请求的 endpoint 尝试中维护已尝试集合。G3 的网关回调将
`retry_non_idempotent` 显式设为 `true`，但保持 `max_retry = 0`：A 返回连接失败或非成功结果
时，客户端可以继续选择 B，却不会对同一 endpoint 进行第二次提交；Servlet 不再叠加第二层
retry。该限定仅服务本地 Mock 闭环，真实 Provider 的幂等与重复计费策略留给 G6。

## 配置

G3 配置采用静态 Provider 列表。每个条目都有仅用于配置可读性和演示日志的 `name`，以及
用于创建 endpoint 的 `url` 与 `weight`。本次演示使用 `ROUND_ROBIN`；保留
`load_balance` 字段，让模块显式校验已支持的策略值。

```yaml
ai_gateway:
  enabled: true
  server_name: ai-gateway
  request_timeout_ms: 800
  load_balance: ROUND_ROBIN
  providers:
    - name: mock-a
      url: http://127.0.0.1:19001
      weight: 1
    - name: mock-b
      url: http://127.0.0.1:19002
      weight: 1
```

模块启动时拒绝空 Provider 列表、空名称、非法 URL、非 HTTP/HTTPS scheme、缺失 host 或
解析后端口无效的配置。URL 未显式指定端口时，沿用 HTTP/HTTPS 的标准默认端口。配置错误只
记录到服务器日志并使模块启动失败，不能注册一条半可用路由。

## 错误语义

- Provider A 失败、B 成功：向调用方返回 B 映射出的正常 Chat Completions 响应。
- 所有 Provider 均不可用：保留 G2 的 `502` 和 `UPSTREAM_UNAVAILABLE` 错误对象。
- 任何成功响应：不暴露 Provider 名、URL、端口或底层错误文本。

## 测试与演示

新增专用 G3 测试目标，而不是扩张现有 Servlet 基础测试。测试启动两个本地 HTTP 上游，
通过实际 `HttpLoadBalanceClient` 和 `AiGatewayServlet` 验证：

1. 连续两次请求分别到达 A、B；
2. A 不可用时，单次请求自动尝试 B 并成功；
3. 对外 JSON 不包含 `mock-a`、`mock-b` 或本地地址。

演示脚本同时启动两个 `mock_model_provider`。脚本发送两次正常请求，然后停止 A 后发送第三次
请求；Provider 的请求日志与脚本输出共同证明 A、B 轮询及 A 到 B 的故障转移。脚本退出时
清理全部子进程。

## 文件边界

- `modules/ai_gateway/ai_gateway_upstream.*`：Provider URL 校验、endpoint 创建和负载均衡客户端装配。
- `modules/ai_gateway/ai_gateway_module.cc`：配置解析和路由注册，并调用上游装配组件。
- `modules/ai_gateway/ai_gateway_servlet.*`：维持既有协议/错误映射职责，不加入选择逻辑。
- `examples/mock_model_provider.cc`：仅补充本地演示所需的请求日志。
- `tests/test_ai_gateway_load_balance.cc`：G3 的真实负载均衡与故障转移闭环测试。
- `scripts/demo_ai_gateway.sh` 与 `examples/ai_gateway_conf/ai_gateway.yml`：双 Provider 演示入口和配置。
- `文档/AI模型调用网关方案.md`：同步 G3 状态、配置、验证命令与演示说明。
