#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
script="${root_dir}/scripts/demo_ai_gateway_real_provider.sh"
tmp_dir=$(mktemp -d)

cleanup() {
    rm -rf "$tmp_dir"
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

mkdir -p "$tmp_dir/fake-bin"
cat >"$tmp_dir/fake-bin/cmake" <<'FAKE_CMAKE'
#!/usr/bin/env bash
echo "cmake should not be called before demo preflight" >&2
exit 77
FAKE_CMAKE
chmod +x "$tmp_dir/fake-bin/cmake"

run_and_expect_refusal() {
    local output_file="$1"
    shift

    set +e
    env -i PATH="$tmp_dir/fake-bin:/usr/bin:/bin" HOME="${HOME:-/tmp}" "$@" \
        >"$output_file" 2>&1
    local status=$?
    set -e

    [[ "$status" -eq 2 ]] || fail "expected exit 2, got ${status}; output=$(cat "$output_file")"
    ! grep -q "cmake should not be called" "$output_file" ||
        fail "script called cmake before refusing; output=$(cat "$output_file")"
}

no_opt_in_output="$tmp_dir/no-opt-in.out"
run_and_expect_refusal "$no_opt_in_output" bash "$script"
grep -q "SYLAR_AI_GATEWAY_REAL_DEMO=1" "$no_opt_in_output" ||
    fail "missing opt-in guidance; output=$(cat "$no_opt_in_output")"

missing_key_output="$tmp_dir/missing-key.out"
run_and_expect_refusal "$missing_key_output" \
    SYLAR_AI_GATEWAY_REAL_DEMO=1 \
    bash "$script"
grep -q "API_KEY_DOUBAO" "$missing_key_output" ||
    fail "missing yml key guidance; output=$(cat "$missing_key_output")"
grep -q "API_KEY_DEEPSEEK" "$missing_key_output" ||
    fail "missing key guidance; output=$(cat "$missing_key_output")"

set +e
env -i PATH="$tmp_dir/fake-bin:/usr/bin:/bin" HOME="${HOME:-/tmp}" \
    SYLAR_AI_GATEWAY_REAL_DEMO=1 \
    API_KEY_DOUBAO=fake-doubao \
    API_KEY_DEEPSEEK=fake-deepseek \
    bash "$script" >"$tmp_dir/preflight-ok.out" 2>&1
status=$?
set -e
[[ "$status" -eq 77 ]] ||
    fail "expected fake cmake after successful preflight, got ${status}; output=$(cat "$tmp_dir/preflight-ok.out")"
grep -q "cmake should not be called" "$tmp_dir/preflight-ok.out" ||
    fail "script did not reach build after yml keys were present; output=$(cat "$tmp_dir/preflight-ok.out")"

echo "test_demo_ai_gateway_real_provider_script passed"
