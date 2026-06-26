# 2026-06-26 demo 脚本 mock-c 启动卡住

## 问题

用户在 `scripts/demo_ai_gateway.sh` 里新增 mock-c 启动命令后，脚本无法继续往后执行。

## 排查命令

1. `git diff -- scripts/demo_ai_gateway.sh`
   - 原因：先确认用户刚改了哪一行，避免直接猜测脚本行为。
   - 结论：新增了 `./bin/mock_model_provider --port 19003 --name mock-c --mode error`，但没有放到后台执行。

2. `sed -n '1,220p' scripts/demo_ai_gateway.sh`
   - 原因：查看脚本整体启动顺序、pid 记录和 cleanup 逻辑。
   - 结论：mock-a、mock-b 都使用 `&` 后台启动并记录 pid；mock-c 是前台启动。

3. `timeout 5 bash -x scripts/demo_ai_gateway.sh`
   - 原因：用短超时观察脚本实际执行到哪一步。
   - 结论：脚本执行到 mock-c 启动命令处；当前沙箱因为不允许 bind 本地端口导致 mock-c 很快失败返回，但在正常本机环境中 mock-c 成功绑定端口后会常驻运行，脚本会停在这一行。

4. `sed -n '100,240p' examples/mock_model_provider.cc`
   - 原因：确认 `mock_model_provider` 是否是常驻服务进程。
   - 结论：`StartProvider()` 绑定端口、注册 servlet、调用 `server->start()`，成功启动后会作为 HTTP Server 持续处理请求，不会自动退出。

## 根因

`mock-c` 启动命令缺少 `&`，导致它以前台进程运行。`mock_model_provider` 本身是常驻 HTTP 服务，所以脚本不会继续执行后面的网关启动和请求验证。

## 处理建议

- 如果目标是“C 已挂掉”：不要启动 mock-c，删除新增的 mock-c 启动命令。
- 如果目标是“C 启动但返回错误”：需要把 mock-c 放到后台执行、记录 `mock_c_pid` 并在 cleanup 里清理，同时把脚本中“C 为 DOWN / skipped_down”的断言改成“C 返回 503 后故障转移”。
