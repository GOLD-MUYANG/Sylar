# 2026-06-18 实现 HttpClient 失败重试

## 目标

根据 `文档/接下来要做什么.md` 第四步，为 `HttpClient` 增加失败重试能力。

本次边界：

- 重试策略放在 `HttpClient` 层，继续让 `HttpConnectionPool` 只负责连接复用和单次请求。
- `HttpRequestOptions` 继续只表达单次请求的超时模型，不混入重试策略。
- 默认不改变旧行为：不传 `HttpRetryOptions` 时 `max_retry = 0`。

## 使用过的命令和原因

1. `sed -n '1,240p' 文档/接下来要做什么.md`
   - 原因：确认第四步的需求边界，包括幂等请求默认允许重试、POST/RPC 默认不重试、可重试错误类型和退避策略。

2. `sed -n '1,260p' sylar/http/http_client.h`
   - 原因：查看 `HttpClient` 对业务层暴露的入口，决定重试选项应该添加在哪些重载上。

3. `sed -n '1,360p' sylar/http/http_client.cc`
   - 原因：查看 `HttpClient` 当前请求转发、错误归一化和日志位置，确认重试循环应放在 `request()` 的连接池调用外层。

4. `sed -n '1,280p' sylar/http/http_connection.h`
   - 原因：确认已有 `HttpResult::Error` 分类，避免新增重复错误码。

5. `sed -n '1,360p' tests/test_http_client.cc`
   - 原因：查看现有本地测试服务器和测试风格，把重试测试继续放在 HTTP 客户端测试文件里。

6. `cmake --build build --target test_http_client`
   - 原因：先只新增重试测试，确认 RED。结果为预期失败：`HttpRetryOptions` 尚不存在。

7. `cmake --build build --target test_http_client`
   - 原因：实现 `HttpRetryOptions` 和重试逻辑后，确认客户端测试目标可以编译。

8. `./bin/test_http_client`
   - 原因：运行本地 HTTP 客户端测试，验证 GET 遇到 503 后按配置重试一次并成功。

9. `clang-format -i sylar/http/http_client.h sylar/http/http_client.cc tests/test_http_client.cc`
   - 原因：尝试按仓库格式化触及文件；本机没有 `clang-format`，命令返回 `command not found`，后续改为人工检查格式。

10. `cmake --build build`
    - 原因：完整构建，确认新增接口没有破坏库、测试目标和主程序链接。

11. `./bin/test_http_request_options`
    - 原因：回归第二步的超时模型测试，确认新增重试选项没有影响单次请求超时语义。

12. `./bin/test_http_connection`
    - 原因：回归连接池和底层连接测试，确认 HttpClient 层变化没有破坏连接池行为。

## 结果

- 新增 `HttpRetryOptions`，支持：
  - `max_retry`
  - `retry_interval_ms`
  - `Backoff::FIXED`
  - `Backoff::LINEAR`
  - `Backoff::EXPONENTIAL`
  - `retry_non_idempotent`
- 新增带 `HttpRetryOptions` 的 `Request/Get/Post/request/get/post` 重载。
- 默认只自动重试幂等方法：`GET`、`HEAD`、`PUT`、`DELETE`、`OPTIONS`、`TRACE`。
- 默认不重试 `POST/PATCH` 等非幂等请求，除非显式设置 `retry_non_idempotent = true`。
- 当前可重试错误：
  - `CONNECT_FAIL`
  - `CONNECT_TIMEOUT`
  - `RECV_TIMEOUT`
  - HTTP `500/502/503/504`
- `tests/test_http_client.cc` 增加本地 `/flaky` 场景，覆盖 GET 首次 503、第二次成功的重试行为。

## 验证

- `cmake --build build`：通过。
- `./bin/test_http_client`：通过。
- `./bin/test_http_request_options`：通过。
- `./bin/test_http_connection`：通过。

## 注意

- `clang-format` 未安装，因此没有自动格式化。
- 跑构建会更新 `bin/` 下的构建产物；这些文件属于构建输出，不是本次源码逻辑改动重点。
