#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root_dir"

if [[ "${SYLAR_AI_GATEWAY_REAL_SMOKE:-}" != "1" ]]; then
    echo "拒绝执行：请显式设置 SYLAR_AI_GATEWAY_REAL_SMOKE=1"
    echo "需要同时设置："
    echo "  SYLAR_AI_GATEWAY_REAL_BASE_URL"
    echo "  SYLAR_AI_GATEWAY_REAL_UPSTREAM_MODEL"
    echo "  SYLAR_AI_GATEWAY_REAL_API_KEY_ENV"
    echo "并在本机环境中设置该 API Key 环境变量的值。脚本不会接收或打印 Key。"
    exit 2
fi

if [[ ! -x ./bin/ai_gateway_real_provider_smoke ]]; then
    cmake --build build --target ai_gateway_real_provider_smoke
fi

./bin/ai_gateway_real_provider_smoke
