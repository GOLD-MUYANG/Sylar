# 2026-06-01 收窄 HttpRequestOptions 测试边界

## 背景

`tests/test_http_request_options.cc` 里的超时断言原先使用 `elapsed >= 40 && elapsed < 500`，范围过宽，不能有效证明 50ms 超时预算生效；同时测试没有明确说明延迟发生在响应阶段，容易看起来只是设置 options 后观察总耗时。

## 排查和修正命令

1. `sed -n '1,280p' tests/test_http_request_options.cc`

   原因：检查当前测试代码，确认 `SlowHttpServer` 延迟点、超时参数和耗时断言。

2. `rg -n "HttpRequestOptions|MergeTimeout|setSendTimeOut|setRecvTimeOut|doRequest\\(" sylar/http/http_connection.cc sylar/socket.cc tests/test_http_request_options.cc`

   原因：对照生产代码里 connect/send/recv/total timeout 的使用路径，确认测试应该覆盖哪条路径。

3. `cmake --build build --target test_http_request_options && ./bin/test_http_request_options`

   原因：修改测试后单独构建并运行新目标，验证收窄后的断言仍然通过。该测试需要本地 `127.0.0.1` socket，因此使用提升权限运行。

## 修正内容

- 将响应延迟设置为 300ms，避免服务端太快返回导致测试含义不清。
- 将耗时断言从 `< 500ms` 收窄到 `>= 40ms && < 150ms`。
- 在服务端读到完整请求后再 sleep，并记录 `hasRequest()`，测试里断言服务端确实收到了请求，说明 connect/send 已经过了，超时发生在等待响应阶段。
- `recv_timeout_ms` 场景设置 `recv_timeout_ms=50`、`total_timeout_ms=1000`，并检查错误信息中包含对应参数。
- `total_timeout_ms` 场景设置 `recv_timeout_ms=1000`、`total_timeout_ms=50`，并检查错误信息中包含对应参数。

## 验证结果

- `cmake --build build --target test_http_request_options && ./bin/test_http_request_options` 通过。
