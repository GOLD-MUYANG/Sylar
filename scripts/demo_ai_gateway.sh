#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
mock_pid=""
gateway_pid=""

cleanup() {
    [[ -n "$gateway_pid" ]] && kill "$gateway_pid" 2>/dev/null || true
    [[ -n "$mock_pid" ]] && kill "$mock_pid" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

cd "$root_dir"
./bin/mock_model_provider --port 19001 --name mock-a --mode normal &
mock_pid=$!
./bin/sylar -s -c examples/ai_gateway_conf &
gateway_pid=$!

for _ in $(seq 1 20); do
    if curl -sS --max-time 1 http://127.0.0.1:18080/ >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done

curl -sS -X POST http://127.0.0.1:18080/v1/chat/completions \
    -H 'Content-Type: application/json' \
    -d '{"model":"demo-chat","messages":[{"role":"user","content":"解释熔断"}]}'
