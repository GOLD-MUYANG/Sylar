# 2026-06-30 真实Provider脚本只显示mock排查

## 问题

运行 `scripts/demo_ai_gateway_real_provider.sh` 后，页面只展示 `mock-a`、
`mock-b`、`mock-c`，没有展示 DeepSeek 或 Ark 真实 provider。

## 排查命令和原因

- `sed -n '1,140p' scripts/demo_ai_gateway_real_provider.sh`
  - 确认脚本现在直接使用 `./bin/sylar -s -c examples/ai_gateway_conf`，
    不再生成临时 `/tmp/.../ai_gateway.yml`。
- `sed -n '1,130p' examples/ai_gateway_conf/ai_gateway.yml`
  - 确认当前实际读取的 YAML 中 `real_providers.enabled: false`。
- `sed -n '230,330p' modules/ai_gateway/ai_gateway_module.cc`
  - 确认 `real_config.enabled` 为 false 时不会创建 `RealProviderRuntime`，
    路由和状态会继续走 mock `ai_gateway` 配置。
- `sed -n '70,120p' modules/ai_gateway/ai_gateway_status_servlet.cc`
  - 确认只有创建 real runtime 时 `/internal/status` 才会带
    `real_providers` 字段。
- `sed -n '390,430p' modules/ai_gateway/ai_gateway_demo.html`
  - 确认页面优先渲染 `status.real_providers.providers`；没有该字段时回退渲染
    `status.providers`，因此当前只显示 mock。

## 结论

脚本已经没有绕开 YAML；当前只显示 mock 的直接原因是
`examples/ai_gateway_conf/ai_gateway.yml` 里 `real_providers.enabled` 仍为
`false`。

如果把 `real_providers.enabled` 改成 `true` 后仍只显示 mock，需要继续检查所有
`enabled: true` 的真实 provider 对应 `api_key_env` 是否在当前 shell 可见。当前配置里
Ark 和 DeepSeek 都是 enabled provider；只测 DeepSeek 时应把 Ark 设为 `enabled: false`。
