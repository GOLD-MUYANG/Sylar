#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
mock_a_pid=""
mock_b_pid=""
mock_c_pid=""
gateway_pid=""
log_dir=""
serve_only=false

# 修改这里即可切换三个 provider 的状态。
# mode 支持：normal / slow / error / off；off 表示不启动该 provider。
mock_a_mode="${MOCK_A_MODE:-normal}"
mock_b_mode="${MOCK_B_MODE:-normal}"
mock_c_mode="${MOCK_C_MODE:-normal}"
mock_a_delay_ms="${MOCK_A_DELAY_MS:-0}"
mock_b_delay_ms="${MOCK_B_DELAY_MS:-0}"
mock_c_delay_ms="${MOCK_C_DELAY_MS:-0}"

if [[ "${1:-}" == "--serve" ]]; then
    serve_only=true
fi

cleanup() {
    [[ -n "$gateway_pid" ]] && kill "$gateway_pid" 2>/dev/null || true
    [[ -n "$mock_a_pid" ]] && kill "$mock_a_pid" 2>/dev/null || true
    [[ -n "$mock_b_pid" ]] && kill "$mock_b_pid" 2>/dev/null || true
    [[ -n "$mock_c_pid" ]] && kill "$mock_c_pid" 2>/dev/null || true
    [[ -n "$log_dir" ]] && rm -rf "$log_dir"
}
trap cleanup EXIT INT TERM

start_mock_provider() {
    local label="$1"
    local port="$2"
    local mode="$3"
    local delay_ms="$4"
    local log_file="$5"

    if [[ "$mode" == "off" ]]; then
        echo "mock-${label}: off, 不启动"
        return 0
    fi

    ./bin/mock_model_provider --port "$port" --name "mock-${label}" --mode "$mode" \
        --delay-ms "$delay_ms" >"$log_file" 2>&1 &

    case "$label" in
        a)
            mock_a_pid=$!
            ;;
        b)
            mock_b_pid=$!
            ;;
        c)
            mock_c_pid=$!
            ;;
    esac

    echo "mock-${label}: mode=${mode}, port=${port}"
}

cd "$root_dir"
log_dir=$(mktemp -d)
echo '--- 启动 Mock Provider ---'
start_mock_provider a 19001 "$mock_a_mode" "$mock_a_delay_ms" "$log_dir/mock-a.log"
start_mock_provider b 19002 "$mock_b_mode" "$mock_b_delay_ms" "$log_dir/mock-b.log"
start_mock_provider c 19003 "$mock_c_mode" "$mock_c_delay_ms" "$log_dir/mock-c.log"

./bin/sylar -s -c examples/ai_gateway_conf &
gateway_pid=$!

for _ in $(seq 1 20); do
    if curl -sS --max-time 1 http://127.0.0.1:18080/demo >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done

request_body=$(tr -d '\n' < examples/ai_gateway_request.json)
status_url="http://127.0.0.1:18080/internal/status"
completion_url="http://127.0.0.1:18080/v1/chat/completions"

echo '--- 初始状态（/internal/status） ---'
status_body=$(curl -sS "$status_url")
echo "$status_body"
echo
echo "浏览器演示页面: http://127.0.0.1:18080/demo"

if [[ "$serve_only" == true ]]; then
    echo '按 Ctrl+C 停止 mock provider 和网关。'
    wait "$gateway_pid"
    exit 0
fi

echo '--- 第 1 次请求 ---'
headers_file="$log_dir/headers-1.txt"
body_file="$log_dir/body-1.json"
curl -sS -D "$headers_file" -o "$body_file" -X POST "$completion_url" \
    -H 'Content-Type: application/json' \
    -H 'X-Ai-Gateway-Demo-Trace: 1' \
    -d "$request_body"
cat "$body_file"
echo
echo '--- 第 1 次请求 trace header ---'
grep -i '^X-Ai-Gateway-Trace:' "$headers_file" || echo '未返回 X-Ai-Gateway-Trace'
echo

echo '--- 第 2 次请求 ---'
curl -sS -X POST "$completion_url" -H 'Content-Type: application/json' -d "$request_body"
echo
echo '--- 请求后的状态（/internal/status） ---'
status_body=$(curl -sS "$status_url")
echo "$status_body"
sleep 0.1
echo '--- Provider 请求日志 ---'
grep -h 'mock provider received request' "$log_dir"/mock-*.log || echo '暂无 provider 请求日志'
