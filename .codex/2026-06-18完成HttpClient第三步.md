# 2026-06-18 完成 HttpClient 第三步

## 目标

根据 `文档/接下来要做什么.md` 的第三步，新增稳定的 `HttpClient`，让业务层通过更上层入口发起 HTTP 请求，而不是直接操作 `HttpConnectionPool`。

## 排查与验证过程

1. `sed -n '1,260p' 文档/接下来要做什么.md`
   - 原因：确认第三步的真实要求，避免凭记忆实现。
   - 结论：第三步要求新增 `HttpClient::request()` / `get()` / `post()`，由 `HttpClient` 管理连接池、超时、错误码和日志。

2. `sed -n '1,520p' sylar/http/http_connection.h` 和 `sed -n '1,900p' sylar/http/http_connection.cc`
   - 原因：确认现有 `HttpConnectionPool`、`HttpRequestOptions` 和错误码边界。
   - 结论：底层连接池已有超时选项入口，适合在上层 `HttpClient` 做封装和错误映射。

3. `sed -n '1,260p' tests/test_http_request_options.cc` 与 `sed -n '1,320p' tests/test_http_connection.cc`
   - 原因：参考现有 HTTP 测试风格，同时避免继续膨胀已有测试文件。
   - 结论：新增独立 `tests/test_http_client.cc` 更符合测试边界。

4. `cmake --build build --target test_http_client`
   - 原因：TDD 红灯验证新增测试确实会因为缺少 `HttpClient` 失败。
   - 结论：第一次失败为 `No rule to make target 'test_http_client'`，随后运行 `cmake -S . -B build` 后失败为缺少 `sylar/http/http_client.cc`，红灯成立。

5. `./bin/test_http_client`
   - 原因：验证新增行为测试。
   - 结论：普通沙箱下本地 `socket()` 返回 `EPERM`，不是业务逻辑失败。

6. `nl -ba sylar/socket.cc | sed -n '490,530p'` 和 `rg -n "socket_f|accept_f|recv_f|send_f|close_f" sylar/hook.cc sylar/hook.h`
   - 原因：定位测试本地服务端为什么会被 hook 影响。
   - 结论：测试服务端应使用 `hook.h` 暴露的原始系统调用指针，例如 `socket_f`、`accept_f`、`recv_f`、`send_f`、`close_f`。

7. `./bin/test_http_client`（允许本地 127.0.0.1 socket 后运行）
   - 原因：验证本地 HTTP 服务端测试的真实行为。
   - 结论：第一次真实运行发现 POST body 为空；继续检查 `HttpRequest::dump()` 后确认它输出小写 `content-length`，测试服务端需要大小写无关解析。

8. `cmake --build build --target test_http_client` 与 `./bin/test_http_client`（允许本地 socket 后运行）
   - 原因：验证修复后的新增测试。
   - 结论：新增目标构建通过，`test_http_client` 通过。

9. `cmake --build build`
   - 原因：C++ 源码和 CMake 均有变更，需要跑完整构建。
   - 结论：完整构建通过。

10. `./bin/test_http_request_options`（允许本地 socket 后运行）
    - 原因：确认第二步超时模型没有被 `HttpClient` 封装影响。
    - 结论：测试通过。

11. `./bin/test_http_connection`（允许访问外网后运行）
    - 原因：确认连接池已有 www.baidu.com 回归没有被本次变更影响。
    - 结论：测试通过。

## 变更范围

- `sylar/http/http_client.h`
- `sylar/http/http_client.cc`
- `sylar/http/http_connection.h`
- `tests/test_http_client.cc`
- `CMakeLists.txt`
- `文档/接下来要做什么.md`
