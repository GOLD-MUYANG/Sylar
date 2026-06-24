# AI 模型调用网关 G0-G2 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在不依赖外网或 API Key 的条件下，完成可注册路由的插件入口、可控 Mock Provider，以及单提供者的 Chat Completions 非流式网关闭环。

**Architecture:** `Application` 仅提供按 HTTP 服务名查询 `HttpServer` 的只读入口，模块在既有 `onServerReady()` 回调中注册路由。`modules/ai_gateway` 保存业务协议、Servlet 和插件装配；`mock_model_provider` 是独立可执行程序。两端只在业务协议层使用 JsonCpp，HTTP 客户端和框架治理层保持不变。

**Tech Stack:** C++11、Sylar HTTP Server/HttpClient/Module、JsonCpp 1.9.5、CMake、CTest。

---

## 文件边界

- `sylar/application.h/.cc`：在模块装载后强制重载配置，并暴露按名称查询已创建 HTTP 服务的受控入口。
- `modules/ai_gateway/ai_gateway_protocol.h/.cc`：只做 Chat Completions 最小 JSON 请求校验与统一 JSON 响应构造。
- `modules/ai_gateway/ai_gateway_servlet.h/.cc`：只处理网关 HTTP 请求、调用单一 `HttpClient`、映射上游结果。
- `modules/ai_gateway/ai_gateway_module.cc`：只读取网关 YAML 配置并在 `onServerReady()` 装配 Servlet。
- `examples/mock_model_provider.cc`：独立 Mock Provider，按启动参数返回 normal、slow 或 error。
- `tests/test_ai_gateway_protocol.cc`：协议层单元测试，不依赖监听端口。
- `tests/test_ai_gateway_route_registry.cc`：验证 Application 的服务查找契约。
- `tests/test_module_config_timing.cc`：验证动态模块注册配置项后可由强制重载读取既有 YAML。
- `CMakeLists.txt`：查找并链接 JsonCpp，接入测试、Mock Provider 和模块目标。
- `bin/conf/ai_gateway.yml`、`文档/AI模型调用网关方案.md`：演示配置及实现状态。

### Task 1：补齐并验证 G0 路由注册入口

**Files:**
- Modify: `sylar/application.h`
- Modify: `sylar/application.cc`
- Create: `tests/test_ai_gateway_route_registry.cc`
- Create: `tests/test_module_config_timing.cc`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 写配置时序失败测试**

在 `tests/test_module_config_timing.cc` 中创建临时目录及 `gateway.yml`（含 `ai_gateway.enabled: true`），首次强制加载配置目录后再注册同名 `ConfigVar<bool>`，断言其仍为默认 false；第二次使用 `force=true` 重载同一目录，断言值变为 true。

- [ ] **Step 2: 运行配置时序失败测试**

运行：`cmake --build build --target test_module_config_timing && ./bin/test_module_config_timing`

预期：编译失败，因为测试目标尚未接入；接入目标但未修改 Application 后，该测试展示现有默认加载不会重新解析未变更 YAML 的行为。

- [ ] **Step 3: 写路由入口失败测试**

在 `tests/test_ai_gateway_route_registry.cc` 中构造 `Application`，断言未知服务名返回空。该测试只验证查询接口不会为不存在的服务制造实例；非空路径由 Task 4 的真实插件注册演示验证，不向生产 `Application` 增加测试专用写接口。

- [ ] **Step 4: 运行路由入口失败测试**

运行：`cmake --build build --target test_ai_gateway_route_registry && ./bin/test_ai_gateway_route_registry`

预期：编译失败，因为 `Application::getHttpServer` 尚不存在。

- [ ] **Step 5: 最小实现**

在 `Application::init()` 的 `ModuleMgr::init()` 后调用 `Config::LoadFromConfDir(conf_path, true)`；失败则拒绝启动。这样基础配置先提供 `module.path`，动态模块注册配置项后再读取同一份 YAML。另添加 `getHttpServer(const std::string &name) const`，遍历 `m_httpservers`，按 `HttpServer::getName()` 返回匹配实例。生产代码仍只在 `run_fiber()` 的 bind 成功后向 `m_httpservers` 加入实例，不新增服务写入接口。

- [ ] **Step 6: 运行通过测试**

运行：`cmake --build build --target test_module_config_timing test_ai_gateway_route_registry && ./bin/test_module_config_timing && ./bin/test_ai_gateway_route_registry`

预期：退出码为 0。

### Task 2：实现并验证协议层

**Files:**
- Create: `modules/ai_gateway/ai_gateway_protocol.h`
- Create: `modules/ai_gateway/ai_gateway_protocol.cc`
- Create: `tests/test_ai_gateway_protocol.cc`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 写失败测试**

测试 `ParseChatCompletionRequest`：合法 `model` 和三类文本消息可解析；缺少 model、非数组 messages、未知 role、非字符串 content 和 `stream:true` 分别返回明确错误。测试 `BuildChatCompletionResponse`：生成 `object=chat.completion`、原 model、assistant content 与零 usage。

- [ ] **Step 2: 运行失败测试**

运行：`cmake --build build --target test_ai_gateway_protocol && ./bin/test_ai_gateway_protocol`

预期：编译失败，因为协议类型与函数尚不存在。

- [ ] **Step 3: 最小实现**

使用 `Json::CharReaderBuilder` 解析，定义 `ChatMessage` 与 `ChatCompletionRequest`；使用 `Json::StreamWriterBuilder` 生成错误对象、Mock Provider 对象及对外 Chat Completions 响应。所有协议错误只返回公开信息，不携带地址、栈或凭据。

- [ ] **Step 4: 运行通过测试**

运行：`cmake --build build --target test_ai_gateway_protocol && ./bin/test_ai_gateway_protocol`

预期：退出码为 0。

### Task 3：实现 G1 Mock Provider

**Files:**
- Create: `examples/mock_model_provider.cc`
- Modify: `CMakeLists.txt`
- Create: `scripts/demo_ai_gateway.sh`

- [ ] **Step 1: 写失败测试**

在协议测试中补充 Mock Provider 成功对象与 503 错误对象的精确 JSON 断言；它们是 Mock Provider 返回体的稳定契约。

- [ ] **Step 2: 运行失败测试**

运行：`cmake --build build --target test_ai_gateway_protocol && ./bin/test_ai_gateway_protocol`

预期：失败，因为 Mock Provider JSON 构造函数尚不存在。

- [ ] **Step 3: 最小实现**

实现 `mock_model_provider --port <port> --name <name> --mode normal|slow|error [--delay-ms <ms>]`。只注册 `POST /v1/chat/completions`：normal 返回 provider 与固定 completion 文本，slow 在延迟后返回相同结果，error 返回 HTTP 503 与统一错误 JSON；其他方法或路径返回 404。`scripts/demo_ai_gateway.sh` 启动 A/B 两个 normal 实例并在退出时清理它们。

- [ ] **Step 4: 构建并进行本地演示验证**

运行：`cmake --build build --target mock_model_provider`

运行：`./bin/mock_model_provider --port 19001 --name mock-a --mode normal`，另一个终端用 `curl -sS -X POST http://127.0.0.1:19001/v1/chat/completions -H 'Content-Type: application/json' -d '{"model":"demo-chat","messages":[{"role":"user","content":"hi"}]}'`。

预期：返回包含 `provider=mock-a` 的 JSON；若执行环境禁止 loopback，记录该限制并保留可在本机运行的命令。

### Task 4：实现 G2 网关插件与最小直连链路

**Files:**
- Create: `modules/ai_gateway/ai_gateway_servlet.h`
- Create: `modules/ai_gateway/ai_gateway_servlet.cc`
- Create: `modules/ai_gateway/ai_gateway_module.cc`
- Create: `bin/conf/ai_gateway.yml`
- Modify: `CMakeLists.txt`
- Modify: `文档/AI模型调用网关方案.md`

- [ ] **Step 1: 写失败测试**

在 `tests/test_ai_gateway_protocol.cc` 增加错误响应契约：非法请求映射 `INVALID_REQUEST`，上游不可用映射 `UPSTREAM_UNAVAILABLE`；所有错误是 OpenAI 兼容 `error` 对象。

- [ ] **Step 2: 运行失败测试**

运行：`cmake --build build --target test_ai_gateway_protocol && ./bin/test_ai_gateway_protocol`

预期：失败，因为错误响应构造尚未实现。

- [ ] **Step 3: 最小实现**

`AiGatewayModule` 在 `onServerReady()` 读取 `ai_gateway.enabled`、`server_name`、`provider_url` 和 `request_timeout_ms`，通过 `Application::getHttpServer` 取得指定服务并注册 `POST /v1/chat/completions`。`AiGatewayServlet` 校验入站 JSON，使用一份 `HttpClient` 向 `/v1/chat/completions` 转发；成功时把 Mock Provider 的 content 包装成 Chat Completions 响应，超时、网络错误与非 2xx 均映射成不泄露内部信息的上游错误。

- [ ] **Step 4: 构建和端到端验证**

运行：`cmake --build build --target ai_gateway_module mock_model_provider bin_sylar`

运行：启动 Mock Provider，复制 `bin/conf/` 到 `/tmp/sylar-ai-gateway-conf`，保留其中 `server.yml`、`worker.yml`、`log.yml` 并加入 `ai_gateway.yml`，然后执行 `./bin/sylar -s -c /tmp/sylar-ai-gateway-conf`，再用 curl POST `/v1/chat/completions`。

预期：返回 non-streaming Chat Completions JSON；`stream:true` 返回 HTTP 400 的 `INVALID_REQUEST`；停止 Mock Provider 后返回不含内部地址的 `UPSTREAM_UNAVAILABLE`。

### Task 5：回写文档和完整验证

**Files:**
- Modify: `文档/AI模型调用网关方案.md`
- Modify: `README.md`

- [ ] **Step 1: 回写状态**

把 G0 的生命周期部分标为已实现，把路由注册入口标为已实现；G1、G2 标为已实现，并保留 G3-G5 为待实现。补入启动命令、配置文件、测试文件与当前边界。

- [ ] **Step 2: 最终验证**

运行：`cmake --build build && ctest --test-dir build -L unit --output-on-failure && ./bin/test_ai_gateway_route_registry && ./bin/test_ai_gateway_protocol && git diff --check`

预期：构建、unit 标签测试、两个新增测试与 diff 空白检查均成功。
