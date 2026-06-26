# 2026-06-26 demo 脚本 mock-c normal 后退出

## 问题

用户把 `scripts/demo_ai_gateway.sh` 里的 mock-c 改成 `--mode normal` 后，脚本启动后无法继续完成。

## 排查命令

1. `git diff -- scripts/demo_ai_gateway.sh examples/ai_gateway_conf/ai_gateway.yml`
   - 原因：确认 mock-c 当前启动方式、配置和断言是否一致。
   - 结论：脚本已经后台启动 mock-c normal，但仍保留了 `mock-c DOWN` / `skipped_down` 的断言。

2. `nl -ba scripts/demo_ai_gateway.sh | sed -n '1,130p'`
   - 原因：定位脚本行号，检查 `set -e` 下哪些命令会导致提前退出。
   - 结论：第 54 行检查 `'"health":"DOWN"'`，第 76 行检查 `'"outcome":"skipped_down"'`，都只适用于 C 不可用的场景。

3. `nl -ba examples/ai_gateway_conf/ai_gateway.yml | sed -n '1,80p'`
   - 原因：确认健康检查是否启用，以及 provider 顺序。
   - 结论：健康检查已启用，providers 顺序是 mock-c、mock-a、mock-b。

4. `timeout 20 bash -x scripts/demo_ai_gateway.sh`
   - 原因：真实启动本地端口并复现脚本退出点。
   - 结论：`/internal/status` 返回 mock-c health 为 `UP`，随后脚本在 `grep -q '"health":"DOWN"'` 处失败退出。

## 根因

`mock-c --mode normal` 会让 `/health` 返回成功，所以网关健康检查把 C 标记为 `UP`。但脚本仍然按“C 已挂掉”的演示语义写断言，要求状态里出现 `DOWN`，因此在 `set -e` 下提前退出。

## 处理方向

- 如果目标仍是“AB 正常，C 已挂掉”：不要启动 mock-c，或把 mock-c 启动为 `--mode error`，并保持 DOWN/skipped_down 断言。
- 如果目标改成“三个 provider 都正常”：需要把脚本断言改成检查 `UP`，并把第一、第二次请求预期改成按 `ROUND_ROBIN` 命中 mock-c、mock-a，而不是跳过 C 后命中 mock-a、mock-b。
