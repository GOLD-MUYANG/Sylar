#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root_dir"

gateway_pid=""
conf_dir="examples/ai_gateway_conf"
config_file="${conf_dir}/ai_gateway.yml"

cleanup() {
    if [[ -n "$gateway_pid" ]] && kill -0 "$gateway_pid" 2>/dev/null; then
        kill "$gateway_pid" 2>/dev/null || true
        wait "$gateway_pid" 2>/dev/null || true
    fi
    return 0
}
trap cleanup EXIT INT TERM

enabled_provider_key_envs() {
    # 只解析本演示 yml 当前使用的简单结构：
    # real_providers.enabled=true 且 providers[*].enabled=true 时，收集 api_key_env。
    awk '
        function value_after_colon(line) {
            sub(/^[^:]*:[[:space:]]*/, "", line)
            sub(/[[:space:]]+#.*$/, "", line)
            gsub(/^["'\'']|["'\'']$/, "", line)
            return line
        }
        function flush_provider() {
            if (in_real && real_enabled == "true" && provider_seen && provider_enabled == "true") {
                print provider_key
            }
            provider_seen = 0
            provider_enabled = "false"
            provider_key = ""
        }
        /^real_providers:[[:space:]]*$/ {
            flush_provider()
            in_real = 1
            real_enabled = "false"
            next
        }
        in_real && /^[^[:space:]][^:]*:[[:space:]]*$/ {
            flush_provider()
            in_real = 0
            next
        }
        in_real && /^  enabled:[[:space:]]*/ {
            real_enabled = value_after_colon($0)
            next
        }
        in_real && /^    -[[:space:]]/ {
            flush_provider()
            provider_seen = 1
            provider_enabled = "false"
            provider_key = ""
            if ($0 ~ /enabled:[[:space:]]*/) {
                provider_enabled = value_after_colon($0)
            }
            next
        }
        in_real && provider_seen && /^      enabled:[[:space:]]*/ {
            provider_enabled = value_after_colon($0)
            next
        }
        in_real && provider_seen && /^      api_key_env:[[:space:]]*/ {
            provider_key = value_after_colon($0)
            next
        }
        END {
            flush_provider()
        }
    ' "$config_file" | sort -u
}

if [[ "${SYLAR_AI_GATEWAY_REAL_DEMO:-}" != "1" ]]; then
    echo "拒绝执行：真实 Provider 页面演示需要显式设置 SYLAR_AI_GATEWAY_REAL_DEMO=1。"
    echo "脚本默认不触网，也不会启动真实 Provider 网关。"
    exit 2
fi

if [[ ! -f "$config_file" ]]; then
    echo "拒绝执行：找不到 AI Gateway 配置 ${root_dir}/${config_file}。"
    exit 2
fi

missing_key_envs=()
while IFS= read -r api_key_env; do
    [[ -z "$api_key_env" ]] && continue
    if [[ ! "$api_key_env" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]]; then
        echo "拒绝执行：${config_file} 中的 api_key_env 不是合法环境变量名：${api_key_env}"
        exit 2
    fi
    if [[ -z "${!api_key_env:-}" ]]; then
        missing_key_envs+=("$api_key_env")
    fi
done < <(enabled_provider_key_envs)

if [[ "${#missing_key_envs[@]}" -gt 0 ]]; then
    echo "拒绝执行：当前 shell 中看不到 yml 里 enabled provider 需要的 API Key 环境变量。"
    for api_key_env in "${missing_key_envs[@]}"; do
        echo "  - ${api_key_env}"
    done
    echo "脚本不会接收或打印 Key。请先在当前 shell export 对应变量。"
    echo "如果你用 Windows setx 设置过，请确认当前 WSL shell 能通过 echo \"\$变量名\" 看到值。"
    exit 2
fi

cmake --build build --target bin_sylar

echo "使用配置目录: ${root_dir}/${conf_dir}"
echo "AI Gateway 配置: ${root_dir}/${config_file}"
echo "真实 Provider 配置完全以 yml 为准，脚本不会补默认 provider。"

./bin/sylar -s -c "$conf_dir" &
gateway_pid=$!

for _ in $(seq 1 40); do
    if curl --fail -sS --max-time 1 http://127.0.0.1:18080/demo >/dev/null 2>&1; then
        echo "真实 Provider 演示页面: http://127.0.0.1:18080/demo"
        echo "按 Ctrl+C 停止网关。"
        wait "$gateway_pid"
        exit 0
    fi
    sleep 0.25
done

echo "演示页面未在预期时间内可用，请检查前面的网关日志。"
echo "常见原因：端口被占用、当前环境禁止监听 127.0.0.1:18080，或网关启动日志中还有配置错误。"
exit 1
