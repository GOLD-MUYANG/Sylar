# Candidate Selector Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 HTTP 与真实 Provider 的四种候选选择策略统一到通用 `CandidateSelector<T>`，同时保留两条请求执行链各自的生命周期与安全边界。

**Architecture:** `sylar/load_balance` 只提供策略解析、候选元数据访问器和四种模板选择算法。HTTP client 负责 endpoint 的预占与释放，真实 Provider runtime 负责统计，`ProviderAttemptExecutor` 负责每次故障转移重新选择并排除已尝试候选。

**Tech Stack:** C++11、CMake、yaml-cpp、JsonCpp、现有 sylar mutex/HTTP/AI gateway 组件。

## Global Constraints

- 不修改 `文档/手动理解项目.md` 和 `文档/A_代码相关问题集`。
- 保留 `modules/ai_gateway/real_provider_runtime.cc` 中已有用户格式调整。
- 模板实现放在 `candidate_selector.h`，`.cc` 只放策略字符串转换。
- selector 只读取候选状态，不接管 endpoint/provider 请求生命周期。
- Provider 共享预算、错误分类和 `may_have_submitted` 重试安全规则保持不变。
- 不提交构建产物；本次不创建 git commit。

---

### Task 1: 通用 CandidateSelector

**Files:**
- Create: `sylar/load_balance/candidate_selector.h`
- Create: `sylar/load_balance/candidate_selector.cc`
- Create: `tests/test_candidate_selector.cc`
- Modify: `cmake/sylar_core.cmake`
- Modify: `cmake/sylar_tests.cmake`

**Interfaces:**
- Produces: `LoadBalanceStrategy`、`ParseLoadBalanceStrategy()`、`LoadBalanceStrategyToString()`、`CandidateAccessors<T>`、`CandidateSelector<T>`、四种 selector 与 `CreateCandidateSelector<T>()`。

- [x] **Step 1: Write the failing test**

  使用独立 `TestCandidate` 和完整 accessors 覆盖轮询 pool 隔离、过滤、固定 seed 随机、`2:1` 加权序列、权重零归一、最新 active、边界、factory 校验和并发状态完整性。

- [x] **Step 2: Run test to verify it fails**

  Run: `cmake -S . -B build && cmake --build build --target test_candidate_selector`

  Expected: FAIL，因为 `candidate_selector.h` 或目标实现尚不存在。

- [x] **Step 3: Write minimal implementation**

  定义统一枚举和访问器；每个 selector 在自己的 mutex 下过滤 `available && key 非空 && key 不在 tried_keys`，按设计维护 per-pool 状态；factory 在 accessors 不完整时返回空指针。

- [x] **Step 4: Run test to verify it passes**

  Run: `cmake --build build --target test_candidate_selector && ./bin/test_candidate_selector`

  Expected: PASS，进程退出码为 0。

### Task 2: HttpLoadBalanceClient 接入

**Files:**
- Modify: `sylar/http/http_load_balance_client.h`
- Modify: `sylar/http/http_load_balance_client.cc`
- Modify: `modules/ai_gateway/ai_gateway_upstream.cc`
- Modify: `tests/test_http_load_balance_client.cc`

**Interfaces:**
- Consumes: `CandidateSelector<HttpEndpoint::ptr>` 和 `LoadBalanceStrategy`。
- Produces: `using HttpLoadBalanceStrategy = sylar::load_balance::LoadBalanceStrategy`；保留现有 `HttpLoadBalanceClient::Create()` 调用形式。

- [x] **Step 1: Write the failing integration assertion**

  在现有 loopback 测试中断言同一请求发生 endpoint 故障转移时不会重复选择已经进入 `tried_endpoint_keys` 的 endpoint。

- [x] **Step 2: Run test to verify it fails or captures the old implementation boundary**

  Run: `cmake --build build --target test_http_load_balance_client && ./bin/test_http_load_balance_client`

  Expected: 新断言在未接入 selector 时失败；若旧实现行为已满足，则以删除旧算法后编译失败作为 RED 边界。

- [x] **Step 3: Replace embedded algorithms with the common selector**

  `Create()` 构造 HTTP accessors 与 selector；`selectEndpoint()` 在 client mutex 内调用 selector，选中后调用 `beginRequest()`；删除四个具体算法方法和状态成员。AI gateway 的策略解析改用公共解析函数。

- [x] **Step 4: Run HTTP load-balance regression test**

  Run: `cmake --build build --target test_http_load_balance_client && ./bin/test_http_load_balance_client`

  Expected: PASS；若 sandbox 禁止 loopback socket，记录明确的 socket 权限错误并继续离线验证。

### Task 3: 真实 Provider 动态选择

**Files:**
- Modify: `modules/ai_gateway/real_provider_gateway.h`
- Modify: `modules/ai_gateway/real_provider_gateway.cc`
- Modify: `modules/ai_gateway/real_provider_runtime.h`
- Modify: `modules/ai_gateway/real_provider_runtime.cc`
- Modify: `tests/test_ai_gateway_real_provider.cc`
- Modify: `tests/test_ai_gateway_real_runtime.cc`
- Modify: `cmake/ai_gateway.cmake`

**Interfaces:**
- Consumes: `CandidateSelector<ProviderCandidate>`。
- Produces: `ProviderAttemptExecutor` 的 selector 构造入口和 `RealProviderRuntime::getProviderInFlight()`。

- [x] **Step 1: Write failing Provider tests**

  覆盖四种配置名、未知策略、Provider 加权序列、最少连接读取 runtime 最新 `in_flight`、故障转移重新选择、同一请求不重复候选及 `may_have_submitted` 停止切换。

- [x] **Step 2: Run tests to verify RED**

  Run: `cmake --build build --target test_ai_gateway_real_provider test_ai_gateway_real_runtime`

  Expected: FAIL，因为 executor/runtime 尚无 selector 接口，且配置仅接受 ROUND_ROBIN。

- [x] **Step 3: Implement executor and runtime integration**

  executor 默认创建 round-robin selector；执行循环每轮调用 `select(request.model, candidates, tried_keys, &candidate)`，选中即记录 key，再保持既有 budget → limiter → breaker → handler → result classification 顺序。runtime 解析统一策略、创建带 runtime accessors 的 selector、删除 `OrderCandidates()`，保留 request sequence 仅生成 request id/trace。

- [x] **Step 4: Run Provider tests to verify GREEN**

  Run: `cmake --build build --target test_ai_gateway_real_provider test_ai_gateway_real_runtime && ./bin/test_ai_gateway_real_provider && ./bin/test_ai_gateway_real_runtime`

  Expected: 两个测试进程退出码均为 0。

### Task 4: 文档与完整验证

**Files:**
- Modify: `文档/候选选择器重构方案.md`
- Modify: `文档/真实提供商网关.md`

**Interfaces:**
- Consumes: 最终实现和验证结果。
- Produces: 与源码一致的策略支持状态与职责边界说明。

- [x] **Step 1: Update documentation status**

  将重构方案状态改为已实现，并在真实 Provider 文档中说明四种策略、每次故障转移重新选择、selector 不接管请求生命周期。

- [x] **Step 2: Run targeted and full verification**

  Run: `cmake -S . -B build`

  Run: `cmake --build build --target test_candidate_selector test_http_load_balance_client test_ai_gateway_real_runtime test_ai_gateway_real_provider`

  Run: `./bin/test_candidate_selector && ./bin/test_http_load_balance_client && ./bin/test_ai_gateway_real_runtime && ./bin/test_ai_gateway_real_provider`

  Run: `cmake --build build && ctest --test-dir build -L unit --output-on-failure`

  Expected: 所有可运行目标通过；任何环境限制单独记录，不能掩盖 selector 离线测试失败。

- [x] **Step 3: Check scope and diff hygiene**

  Run: `git diff --check && git status --short`

  Expected: 无 whitespace error；两份明确排除文档只保留用户原有修改，构建产物不进入最终源码差异。
