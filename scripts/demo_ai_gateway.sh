#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
mock_pid=""
mock_b_pid=""
gateway_pid=""
log_dir=""

cleanup() {
    [[ -n "$gateway_pid" ]] && kill "$gateway_pid" 2>/dev/null || true
    [[ -n "$mock_pid" ]] && kill "$mock_pid" 2>/dev/null || true
    [[ -n "$mock_b_pid" ]] && kill "$mock_b_pid" 2>/dev/null || true
    [[ -n "$log_dir" ]] && rm -rf "$log_dir"
}
trap cleanup EXIT INT TERM

cd "$root_dir"
log_dir=$(mktemp -d)
./bin/mock_model_provider --port 19001 --name mock-a --mode normal >"$log_dir/mock-a.log" 2>&1 &
mock_pid=$!
./bin/mock_model_provider --port 19002 --name mock-b --mode normal >"$log_dir/mock-b.log" 2>&1 &
mock_b_pid=$!
./bin/sylar -s -c examples/ai_gateway_conf &
gateway_pid=$!

for _ in $(seq 1 20); do
    if curl -sS --max-time 1 http://127.0.0.1:18080/ >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done

request_body='{"model":"demo-chat","messages":[{"role":"user","content":"解释熔断"}]}'

echo '--- 第 1 次请求（预期 mock-a） ---'
curl -sS -X POST http://127.0.0.1:18080/v1/chat/completions -H 'Content-Type: application/json' -d "$request_body"
echo
echo '--- 第 2 次请求（预期 mock-b） ---'
curl -sS -X POST http://127.0.0.1:18080/v1/chat/completions -H 'Content-Type: application/json' -d "$request_body"
echo

kill "$mock_pid"
wait "$mock_pid" 2>/dev/null || true
mock_pid=""

echo '--- 停止 mock-a 后的第 3 次请求（预期故障转移到 mock-b） ---'
curl -sS -X POST http://127.0.0.1:18080/v1/chat/completions -H 'Content-Type: application/json' -d "$request_body"
echo
sleep 0.1
echo '--- Provider 请求日志 ---'
grep -h 'mock provider received request' "$log_dir"/mock-*.log
grep -q 'mock provider received request name=mock-a' "$log_dir/mock-a.log"
test "$(grep -c 'mock provider received request name=mock-b' "$log_dir/mock-b.log")" -eq 2
