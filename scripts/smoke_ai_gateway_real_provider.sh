#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root_dir"

# 火山方舟 Doubao 默认 smoke 配置。需要改模型接入点时，只覆盖
# VOLCENGINE_ARK_MODEL 或 SYLAR_AI_GATEWAY_REAL_UPSTREAM_MODEL。
: "${SYLAR_AI_GATEWAY_REAL_SMOKE:=1}"
: "${SYLAR_AI_GATEWAY_REAL_PROVIDER_NAME:=volcengine-ark}"
: "${SYLAR_AI_GATEWAY_REAL_BASE_URL:=https://ark.cn-beijing.volces.com/api/v3}"
: "${SYLAR_AI_GATEWAY_REAL_CHAT_PATH:=/chat/completions}"
: "${SYLAR_AI_GATEWAY_REAL_LOGICAL_MODEL:=general-chat}"
: "${SYLAR_AI_GATEWAY_REAL_UPSTREAM_MODEL:=${VOLCENGINE_ARK_MODEL:-doubao-seed-2-0-lite-260428}}"
: "${SYLAR_AI_GATEWAY_REAL_API_KEY_ENV:=ARK_API_KEY}"
: "${SYLAR_AI_GATEWAY_REAL_TLS_SERVER_NAME:=ark.cn-beijing.volces.com}"
: "${SYLAR_AI_GATEWAY_REAL_REQUEST_DEADLINE_MS:=30000}"
: "${SYLAR_AI_GATEWAY_REAL_PROMPT:=请只回复 pong}"

export SYLAR_AI_GATEWAY_REAL_SMOKE
export SYLAR_AI_GATEWAY_REAL_PROVIDER_NAME
export SYLAR_AI_GATEWAY_REAL_BASE_URL
export SYLAR_AI_GATEWAY_REAL_CHAT_PATH
export SYLAR_AI_GATEWAY_REAL_LOGICAL_MODEL
export SYLAR_AI_GATEWAY_REAL_UPSTREAM_MODEL
export SYLAR_AI_GATEWAY_REAL_API_KEY_ENV
export SYLAR_AI_GATEWAY_REAL_TLS_SERVER_NAME
export SYLAR_AI_GATEWAY_REAL_REQUEST_DEADLINE_MS
export SYLAR_AI_GATEWAY_REAL_PROMPT

api_key_env="${SYLAR_AI_GATEWAY_REAL_API_KEY_ENV}"
if [[ -z "${!api_key_env:-}" ]]; then
    if [[ -t 0 ]]; then
        read -r -s -p "${api_key_env}: " provider_api_key
        echo
        export "${api_key_env}=${provider_api_key}"
    else
        echo "拒绝执行：请先设置 ${api_key_env}。脚本不会接收或打印 Key。"
        exit 2
    fi
fi

cmake --build build --target ai_gateway_real_provider_smoke

./bin/ai_gateway_real_provider_smoke
