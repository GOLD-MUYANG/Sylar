# AI Gateway Demo Trace Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a local AI Gateway demo page that shows A/B normal providers, C down, and the per-request upstream attempt trace.

**Architecture:** `HttpLoadBalanceClient` records an optional per-request trace without changing routing behavior. `AiGatewayServlet` exposes that trace only when demo trace is enabled and the request asks for it. A focused demo servlet serves the HTML page; mock providers remain independent upstream simulators.

**Tech Stack:** C++11, Sylar HTTP server/servlet, JsonCpp, static HTML/CSS/JavaScript served by the AI gateway module.

---

### Task 1: Load Balance Trace

**Files:**
- Modify: `sylar/http/http_load_balance_client.h`
- Modify: `sylar/http/http_load_balance_client.cc`
- Test: `tests/test_http_load_balance_client.cc`

- [ ] Add a failing test that expects `HttpLoadBalanceRequestTrace` to contain two attempts when the first endpoint is unreachable and the second succeeds.
- [ ] Add `HttpLoadBalanceAttemptTrace` and `HttpLoadBalanceRequestTrace` structs.
- [ ] Add optional trace parameters to the full `request()` / `post()` path used by the gateway.
- [ ] Record endpoint key, selected phase, result code, HTTP status, and safe reason text for each attempt.
- [ ] Run `cmake --build build --target test_http_load_balance_client` and `./bin/test_http_load_balance_client`.

### Task 2: Gateway Trace Exposure

**Files:**
- Modify: `modules/ai_gateway/ai_gateway_servlet.h`
- Modify: `modules/ai_gateway/ai_gateway_servlet.cc`
- Modify: `modules/ai_gateway/ai_gateway_module.cc`
- Test: `tests/test_ai_gateway_servlet.cc`

- [ ] Add a failing servlet test that enables demo trace and expects `X-Ai-Gateway-Trace` on valid traced requests.
- [ ] Change `UpstreamPost` to accept an optional `HttpLoadBalanceRequestTrace *`.
- [ ] Add a constructor flag that controls whether demo trace may be exposed.
- [ ] Expose trace only when enabled and request header `X-Ai-Gateway-Demo-Trace: 1` is present.
- [ ] Read `demo_trace_enabled` from `ai_gateway.yml`, defaulting to false.

### Task 3: Demo Page

**Files:**
- Create: `modules/ai_gateway/ai_gateway_demo_servlet.h`
- Create: `modules/ai_gateway/ai_gateway_demo_servlet.cc`
- Modify: `modules/ai_gateway/ai_gateway_module.cc`
- Modify: `CMakeLists.txt`

- [ ] Add a servlet returning a static HTML demo page at `/demo`.
- [ ] Page sends POST `/v1/chat/completions` with `X-Ai-Gateway-Demo-Trace: 1`.
- [ ] Page refreshes `/internal/status` before and after requests.
- [ ] Page shows provider cards, request timeline, request JSON, response JSON, HTTP status, and latency.

### Task 4: Demo Config And Docs

**Files:**
- Modify: `examples/ai_gateway_conf/ai_gateway.yml`
- Modify: `scripts/demo_ai_gateway.sh`
- Modify: `文档/AI模型调用网关方案.md`

- [ ] Enable `demo_trace_enabled` only in example config.
- [ ] Configure three providers: A and B normal, C configured but not started.
- [ ] Update demo script to start A/B, leave C down, and print the `/demo` URL.
- [ ] Update docs to mark the enhanced HTML demo as implemented.

### Task 5: Verification

**Commands:**
- `cmake -S . -B build`
- `cmake --build build --target test_http_load_balance_client test_ai_gateway_servlet ai_gateway_module mock_model_provider bin_sylar`
- `./bin/test_http_load_balance_client`
- `./bin/test_ai_gateway_servlet`
- `git diff --check`
