# 2026-06-18 实现 HttpClient 多实例负载均衡前半部分

## 任务范围

根据 `文档/接下来要做什么.md` 第五步，只完成前半部分：

- 抽象 `HttpEndpoint`。
- 每个 Endpoint 对应一个 `HttpConnectionPool`。
- 实现第一版 `RoundRobin` 和 `Random` 策略。

未实现第五步后续部分：

- `WeightedRoundRobin`
- `LeastConnection`
- 健康检查

## 排查和验证命令

1. `sed -n '1,240p' 文档/接下来要做什么.md`
   - 原因：确认当前文档中第一到第四步状态，以及第五步的任务边界。

2. `sed -n '240,380p' 文档/接下来要做什么.md`
   - 原因：确认第五步只要求 Endpoint、连接池、RoundRobin、Random，后续策略暂不做。

3. `sed -n '1,260p' sylar/http/http_client.h`
   - 原因：查看现有 `HttpClient` 对外接口和重试选项，决定多实例客户端如何复用请求语义。

4. `sed -n '1,520p' sylar/http/http_client.cc`
   - 原因：确认错误归一化、HTTP 状态码映射和重试判断位置。

5. `sed -n '1,460p' tests/test_http_client.cc`
   - 原因：查看现有本地 HTTP 测试服务器风格，但新增测试单独拆文件，避免测试文件继续膨胀。

6. `cmake --build build --target test_http_load_balance_client`
   - 原因：TDD 红灯验证。第一次失败是构建目录还未生成新目标；重新 `cmake -S . -B build` 后，失败点为缺少 `sylar/http/http_load_balance_client.h`，符合预期。

7. `gdb -batch -ex run -ex bt --args ./bin/test_http_load_balance_client`
   - 原因：普通沙箱运行测试出现 139，先取回溯定位根因。gdb 下正常退出，说明不是稳定代码崩溃。

8. `./bin/test_http_load_balance_client`
   - 原因：复现普通沙箱失败。日志显示 `socket(2, 1, 0) errno=1 errstr=Operation not permitted`，确认失败来自沙箱不允许本地 socket。

9. `./bin/test_http_load_balance_client`（提升权限后）
   - 原因：该测试依赖本地 `127.0.0.1` socket，提升权限后通过。

10. `clang-format -i sylar/http/http_client.h sylar/http/http_load_balance_client.h sylar/http/http_load_balance_client.cc tests/test_http_load_balance_client.cc`
    - 原因：尝试按项目格式化 C++ 文件。当前环境没有 `clang-format` 命令，无法自动格式化。

11. `cmake --build build`
    - 原因：完整构建回归，确认新源码、新测试目标和已有目标都能编译链接。

12. `./bin/test_http_client`（提升权限后）
    - 原因：回归单实例 `HttpClient` 行为，确认友元声明和复用逻辑没有影响既有客户端。

13. `./bin/test_http_request_options`（提升权限后）
    - 原因：回归 HTTP 请求超时模型。

14. `./bin/test_http_connection`
    - 原因：回归连接池和底层 HTTP 连接行为。该测试依赖外网访问。

15. `./bin/test_http_load_balance_client`（提升权限后）
    - 原因：最终验证新增多实例负载均衡客户端测试。

## 实现结果

- 新增 `sylar/http/http_load_balance_client.h`。
- 新增 `sylar/http/http_load_balance_client.cc`。
- 新增 `tests/test_http_load_balance_client.cc`。
- `CMakeLists.txt` 接入新源码和新测试目标。
- `sylar/http/http_client.h` 增加 `HttpLoadBalanceClient` 友元声明，用于复用已有错误归一化和重试判断。
- `文档/接下来要做什么.md` 第五步小节更新为“前半部分已完成”，并保留后续未做项。

## 最终验证

- `cmake --build build` 通过。
- `./bin/test_http_load_balance_client` 通过。该测试使用本地 `127.0.0.1` socket。
- `./bin/test_http_client` 通过。该测试使用本地 `127.0.0.1` socket。
- `./bin/test_http_request_options` 通过。该测试使用本地 `127.0.0.1` socket。
- `./bin/test_http_connection` 通过。该测试依赖外网访问。
