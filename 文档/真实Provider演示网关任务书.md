# 真实 Provider 演示网关任务书

> 状态：待实现。本文承接 `文档/真实提供商网关.md` 已完成的真实 Provider 基础能力，把它推进到“可配置、可真实聊天、可观察、可演示”的运行链路。默认测试和 CI 仍不依赖真实 API Key、外网或计费账户。

## 1. 背景与目标

当前真实 Provider 能力已经具备 adapter、provider-aware executor、错误分类、预算、limiter/breaker 回归、离线 shape validation 和受控手工 smoke 入口，但还没有作为网关运行时的正式演示链路接入。

本任务书的目标是：

1. 在 `ai_gateway.yml` 中配置多个真实 Provider，运行时读取配置。
2. API Key 只通过环境变量读取，配置文件只保存环境变量名。
3. 网关可以根据逻辑模型路由到多个真实 Provider 候选。
4. 提供一个页面演示真实聊天、Provider 状态和一次请求的 timeline。
5. 演示环境可以额外启动两个 mock provider，用于稳定展示失败、超时、限流和故障转移。

本任务书不改变原有 mock 网关的离线验证价值，也不把真实 Provider 访问放进默认测试或 CI。

## 2. 需求判断

用户提出的方向整体是合理的，且比单纯 mock 演示更有说服力。真实 Provider 可以证明网关真的完成了 HTTPS、认证、协议转换、响应解析和统一返回。

需要补充和收紧的点如下：

1. **配置文件只保存 Key 的环境变量名。**
   例如 `api_key_env: ARK_API_KEY`。代码运行时按这个字段读取 `getenv("ARK_API_KEY")`。不能把真实 Key 写进 yml、日志、状态接口、trace 或测试快照。

2. **多个 Provider 必须经过逻辑模型兼容性校验。**
   同一个 `logical_model` 下的 Provider 不等于语义完全一致，只能表示这些候选都能接受同一种业务请求。必须保留 `compatibility_key`，避免把能力、上下文、价格或输出格式差异过大的模型混进同一个自动切换池。

3. **页面切换 Provider 信息只做观察，不默认改变路由。**
   用户点击按钮切换不同 Provider 信息时，建议只是切换右侧状态详情。如果后续需要“强制请求某个 Provider”，应设计成 demo-only 调试参数，不能绕过正常的逻辑模型路由和故障转移语义。

4. **真实聊天和故障演练要分层。**
   左侧聊天应优先走真实 Provider，体现项目说服力；mock provider 用于制造可控异常，展示失败分类、候选切换、breaker 和 timeline。不要让 mock 伪装成真实 Provider。

5. **mock provider 不应塞进生产模块职责。**
   可以做一个演示启动器或脚本，同时启动网关和两个 mock provider；也可以做独立 demo binary。不要把 mock provider 逻辑写进 `AiGatewayServlet` 或真实 Provider executor。

6. **timeline 需要比现有 mock trace 更完整。**
   当前 `/demo` 主要展示 endpoint 尝试结果。真实 Provider 演示需要展示请求进入、解析 model、候选列表、每次尝试、错误分类、是否允许切换、最终返回等阶段。

## 3. 配置设计

建议在 `ai_gateway.yml` 中保留现有 `ai_gateway` mock 配置，同时新增独立的 `real_providers` 或 `ai_gateway.real_providers` 节点。为了边界清晰，推荐使用独立节点：

```yaml
real_providers:
  enabled: false
  server_name: ai-gateway
  route_path: /v1/chat/completions
  demo_enabled: false
  request_deadline_ms: 30000
  max_total_attempts: 3
  load_balance: ROUND_ROBIN
  limiter:
    max_global_concurrency: 8
    max_endpoint_concurrency: 2
    max_global_qps: 20
    max_endpoint_qps: 5
  circuit_breaker:
    enabled: true
    consecutive_failure_threshold: 3
    open_timeout_ms: 5000
    half_open_max_requests: 1
  providers:
    - name: volcengine-ark
      enabled: true
      type: openai_compatible
      logical_model: general-chat
      compatibility_key: chat-basic-v1
      base_url: https://ark.cn-beijing.volces.com/api/v3
      chat_path: /chat/completions
      upstream_model: doubao-seed-2-0-lite-260428
      api_key_env: ARK_API_KEY
      tls_server_name: ark.cn-beijing.volces.com
      weight: 1
    - name: provider-b
      enabled: false
      type: openai_compatible
      logical_model: general-chat
      compatibility_key: chat-basic-v1
      base_url: https://api.provider-b.example
      chat_path: /v1/chat/completions
      upstream_model: provider-b-model
      api_key_env: PROVIDER_B_API_KEY
      weight: 1
```

配置读取要求：

- `enabled=false` 时不读取真实 Key，不访问外网。
- Provider 自身 `enabled=false` 时不加入候选池。
- 启用的 Provider 缺少 `api_key_env` 或环境变量不存在时，应给出脱敏配置错误。
- `base_url`、`chat_path`、`logical_model`、`upstream_model`、`compatibility_key` 不能为空。
- TLS 校验默认开启，不能为了连通默认关闭 `verify_peer`。

## 4. 组件边界

### 4.1 配置层

新增真实 Provider 运行时配置结构，负责从 yml 转换成 `ProviderCandidate`、执行预算、limiter/breaker 选项和 demo 开关。

配置层只做解析、默认值和字段校验，不发起 HTTP 请求。

### 4.2 路由与执行层

复用 `LogicalModelRouter`、`ProviderAttemptExecutor` 和 `OpenAICompatibleProviderAdapter`：

```text
AiGatewayServlet
    -> 解析 OpenAI-compatible 请求
    -> 创建 RequestExecutionBudget
    -> LogicalModelRouter 查找候选 Provider
    -> ProviderAttemptExecutor 逐个尝试
    -> AiProviderAdapter 转换真实 Provider 请求/响应
    -> 返回统一 chat.completion 或 OpenAI 风格 error
```

真实 Provider 路径不应重新走 mock 专用的 `HttpLoadBalanceClient` 统一 body 接口，因为真实候选有各自的 `base_url`、`chat_path`、认证头、上游模型名和响应形状。

### 4.3 Servlet 层

`AiGatewayServlet` 可以继续作为 `/v1/chat/completions` 入口，但它不应直接拼接真实 Provider 请求。建议通过注入一个新的 upstream 回调或 executor wrapper，让 servlet 只负责：

- 读取请求体；
- 协议解析和错误返回；
- 创建/传递 trace 上下文；
- 调用上游执行函数；
- 设置 demo trace 响应头或内部状态。

### 4.4 演示启动层

新增演示启动器或脚本，统一启动：

- 网关服务；
- 至少一个真实 Provider 配置；
- 两个本地 mock provider；
- 演示页面。

mock provider 只用于故障演练，不作为生产真实 Provider 路径的一部分。

## 5. 页面设计

页面目标是“能真实聊天，也能看清网关如何选择和处理 Provider”。

### 5.1 左侧聊天区

左侧做成类似 ChatGPT 的聊天界面：

- 展示用户消息和 assistant 回复；
- 输入框发送消息；
- 可选择逻辑模型，例如 `general-chat`；
- 请求仍发送到网关自己的 `/v1/chat/completions`；
- 返回内容来自当前配置的真实 Provider 候选池；
- 失败时展示网关统一错误摘要，不展示 Key、Authorization、完整 provider 原始 body。

第一版只做非流式响应，不做 SSE 打字机效果。

### 5.2 右侧运行状态区

右侧显示 Provider 状态，支持点击按钮切换查看不同 Provider：

- provider name；
- logical model；
- enabled；
- adapter type；
- upstream model；
- endpoint 脱敏展示；
- health；
- circuit breaker 状态；
- in-flight；
- success / failure / rate limited 计数；
- last failure reason；
- last attempt time；
- last smoke result。

点击 Provider 时只切换状态详情，不默认影响路由选择。

### 5.3 请求过程 timeline

每次聊天请求后展示 timeline：

```text
1. 请求进入网关
2. 解析 model=general-chat
3. 找到候选 provider: A, B, C
4. 尝试 A -> 失败原因 / HTTP 状态 / 是否已发送
5. 判断是否允许切换 -> 是或否，并展示原因
6. 尝试 B -> 成功
7. 返回给调用方
```

timeline 字段建议包含：

- stage；
- provider name；
- adapter type；
- endpoint key；
- attempt phase；
- may_have_submitted；
- http status；
- provider error category；
- gateway decision；
- try_next_candidate；
- elapsed ms；
- consumed attempts；
- remaining deadline。

trace 只能在 demo 或显式请求头下返回，不能污染普通 OpenAI-compatible 响应 body。

## 6. Mock Provider 演练

演示环境需要两个 mock provider，用于稳定制造真实 Provider 不容易遇到的异常：

- `mock-provider-a`：可配置 normal / error / slow / off；
- `mock-provider-b`：可配置 normal / error / slow / off。

演练目标：

- A 正常时返回成功；
- A 连接失败或未启动时切换到 B；
- A 返回 5xx 时在预算内切换到 B；
- A 返回 429 时按限流语义处理；
- A slow 时展示 deadline 耗尽或结果未知；
- A 已发送后读超时时不盲目切换，避免重复提交语义风险。

mock provider 可以复用现有 `examples/mock_model_provider.cc`，也可以新增真实 Provider 契约兼容的 mock。若新增，文件边界应保持清晰：

- mock provider 只负责模拟上游；
- demo launcher 只负责启动和清理；
- gateway module 不内嵌 mock 行为。

## 7. 实施切片

| 切片 | 状态 | 目标 | 验证 |
| --- | --- | --- | --- |
| D0 | 待实现 | 任务书落地，明确真实演示链路边界 | 检查本文档与 `文档/真实提供商网关.md` 不冲突 |
| D1 | 待实现 | 新增真实 Provider yml 配置读取，支持多个 Provider 和 env-only API Key | 单元测试覆盖启用、禁用、缺字段、缺 Key、不兼容 `compatibility_key` |
| D2 | 待实现 | 把真实 Provider executor 接入 `/v1/chat/completions`，但保持显式开关 | fake transport 测试真实路径响应成功、错误脱敏、预算不重置 |
| D3 | 待实现 | 扩展真实 Provider trace，覆盖请求进入、解析、候选、尝试、决策、返回 | 单元测试检查 trace 字段完整且不含 Key/messages |
| D4 | 待实现 | 扩展 `/internal/status` 或新增内部状态接口展示真实 Provider 状态 | 测试状态 JSON 脱敏、provider 计数和 breaker/limiter 状态 |
| D5 | 待实现 | 新增演示页面：左侧聊天，右侧 Provider 状态和 timeline | 本地页面请求能看到真实返回和 timeline |
| D6 | 待实现 | 新增演示启动器或脚本，统一启动网关和两个 mock provider | mock 演练可稳定展示成功、失败和切换 |
| D7 | 待实现 | 真实 Provider live smoke 接入页面或脚本入口，仍保持手工 opt-in | 缺 Key 不触网；设置 Key 后手工请求成功并脱敏输出 |

## 8. 验证策略

默认验证仍以离线为主：

```bash
cmake --build build
ctest --test-dir build -R ai_gateway --output-on-failure
./bin/test_ai_gateway_real_provider
./bin/test_ai_gateway_real_provider_smoke
```

涉及页面和本地 mock 演示时：

```bash
./scripts/demo_ai_gateway.sh
```

真实 Provider live 验证必须手工 opt-in：

```bash
export ARK_API_KEY='本机真实 Key'
export SYLAR_AI_GATEWAY_REAL_SMOKE=1
./scripts/smoke_ai_gateway_real_provider.sh
```

若新增真实演示脚本，也必须满足：

- 默认不触网；
- 缺 Key 不触网；
- Key 不进入命令输出；
- 不进入默认 `ctest` 或 CI；
- 页面和状态接口不回显敏感信息。

## 9. 明确不做

- 不做流式 SSE。
- 不做 tools/function calling。
- 不做多模态输入。
- 不做用户级 token 账单和费用治理。
- 不保证不同 Provider 的回答、usage、计费完全一致。
- 不对可能已经发送到 Provider 的非幂等请求做盲目重试。
- 不把 mock provider 伪装成真实 Provider。

## 10. 完成标准

完成后应能做到：

1. 在 yml 中配置多个真实 Provider。
2. API Key 通过环境变量读取，仓库内不保存真实 Key。
3. 网关运行时可以通过 `/v1/chat/completions` 调用真实 Provider。
4. 页面左侧可以进行真实聊天。
5. 页面右侧可以查看 Provider 状态并切换详情。
6. 每次请求都能看到清晰 timeline。
7. mock provider 可以稳定演示失败、超时和故障转移。
8. 默认测试和 CI 仍不依赖外网、真实 Key 或计费账户。
