#!/usr/bin/env sh
# wp-security-context-v1 冒烟：
#   父客户端注册沙箱、沙箱客户端接入、特权接口过滤、窗口安全上下文属性注入，
#   以及状态在 wctl get_tree 与 get_trace 中的可观测性。
#
# 用法: sh run_sec_ctx_smoke.sh <woodland 二进制> <sec_ctx_probe> <wctl>
set -eu

. "$(dirname "$0")/lib.sh"
smoke_init security-context

BIN="${1:-./build/src/woodland}"
PROBE="${2:-./build/tests/smoke/sec_ctx_probe}"
WCTL="${3:-}"
[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 125; }
[ -x "$PROBE" ] || { echo "probe not built: $PROBE" >&2; exit 125; }
if [ -z "$WCTL" ] || [ ! -x "$WCTL" ]; then
    echo "wctl not found: $WCTL" >&2
    exit 125
fi
command -v jq >/dev/null 2>&1 || { echo "jq is required" >&2; exit 125; }

unset WAYLAND_DISPLAY
export XDG_RUNTIME_DIR="$(mktemp -d)"
export XDG_CONFIG_HOME="$XDG_RUNTIME_DIR/config"
export XDG_DATA_HOME="$XDG_RUNTIME_DIR/data"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME"

export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1
SMOKE_FLAGS="--trace"
SMOKE_LOG="$XDG_RUNTIME_DIR/log"
"$BIN" --trace >"$SMOKE_LOG" 2>&1 &
pid=$!

probe_pid=""
smoke_cleanup() {
    if [ -n "$probe_pid" ]; then
        kill -TERM "$probe_pid" 2>/dev/null || true
        wait "$probe_pid" 2>/dev/null || true
    fi
    kill -TERM "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    rm -rf "$XDG_RUNTIME_DIR"
}

i=0
while [ "$(ls "$XDG_RUNTIME_DIR/" 2>/dev/null | grep -c '^wayland-')" -eq 0 ] \
        && [ "$i" -lt 50 ]; do
    sleep 0.1; i=$((i+1))
done
socket_name="$(ls "$XDG_RUNTIME_DIR/" 2>/dev/null | grep '^wayland-' | head -1)"
[ -n "$socket_name" ] || smoke_fail "wayland socket did not appear"
export WAYLAND_DISPLAY="$socket_name"
smoke_wait_socket || smoke_fail "debug socket did not appear"

wctl_cmd() {
    "$WCTL" -s "$SOCK" -t 1000 "$1"
}

# 1. 验证启动时 get_tree 暴露了 security_context 段且 commit_count 为 0
tree="$(wctl_cmd get_tree)" || smoke_fail "initial get_tree failed"
printf '%s' "$tree" | jq -e '
    .security_context.available == true and
    .security_context.commit_count == 0 and
    .security_context.last_engine == null
' >/dev/null || smoke_fail "initial security_context status unexpected: $(printf '%s' "$tree" | jq -c '.security_context')"

# 2. 启动探针（父客户端注册上下文 + 子客户端建立窗口）
PROBE_OUT="$XDG_RUNTIME_DIR/probe.log"
"$PROBE" >"$PROBE_OUT" 2>&1 &
probe_pid=$!

# 等待探针输出 SANDBOX_READY
i=0
while [ "$i" -lt 100 ]; do
    if grep -q '^SANDBOX_READY' "$PROBE_OUT" 2>/dev/null; then
        break
    fi
    if ! kill -0 "$probe_pid" 2>/dev/null; then
        cat "$PROBE_OUT" >&2 || true
        smoke_fail "probe exited prematurely"
    fi
    sleep 0.05
    i=$((i+1))
done
grep -q '^SANDBOX_READY' "$PROBE_OUT" 2>/dev/null || smoke_fail "probe did not report SANDBOX_READY"

# 3. 校验 get_tree 全局统计
tree="$(wctl_cmd get_tree)" || smoke_fail "get_tree failed"
printf '%s' "$tree" | jq -e '
    .security_context.available == true and
    .security_context.commit_count == 1 and
    .security_context.last_engine == "org.flatpak" and
    .security_context.last_app_id == "org.example.SecCtxSmoke" and
    .security_context.last_instance_id == "smoke-42"
' >/dev/null || smoke_fail "get_tree security_context fields unexpected: $(printf '%s' "$tree" | jq -c '.security_context')"

# 4. 按 app_id 定位沙箱窗口并校验 security_context 注入
printf '%s' "$tree" | jq -e '
    ([.views[] | select(.app_id == "org.example.SecCtxSmoke")]) as $v |
    ($v | length) == 1 and
    $v[0].security_context.sandboxed == true and
    $v[0].security_context.sandbox_engine == "org.flatpak" and
    $v[0].security_context.app_id == "org.example.SecCtxSmoke" and
    $v[0].security_context.instance_id == "smoke-42"
' >/dev/null || smoke_fail "view security_context fields unexpected: $(printf '%s' "$tree" | jq -c '.views')"

# 5. 校验 get_trace 中记录的决策轨迹
trace="$(wctl_cmd get_trace)" || smoke_fail "get_trace failed"
printf '%s' "$trace" | jq -e '
    [.entries[] | select(.what == "sec_ctx")] | length >= 1
' >/dev/null || smoke_fail "missing sec_ctx in trace: $trace"

printf '%s' "$trace" | jq -e '
    .entries[] | select(.what == "sec_ctx" and (.detail | contains("engine=org.flatpak") and contains("app_id=org.example.SecCtxSmoke") and contains("instance_id=smoke-42")))
' >/dev/null || smoke_fail "sec_ctx detail mismatch in trace: $trace"

# 6. 校验无未分类协议 ERROR 日志
if grep -q 'Blocking unknown protocol' "$SMOKE_LOG" 2>/dev/null; then
    cat "$SMOKE_LOG" >&2 || true
    smoke_fail "unexpected unknown protocol error logged during sandbox connection"
fi

# 7. 正常终止探针并等待退出
kill -TERM "$probe_pid" 2>/dev/null || true
wait "$probe_pid" 2>/dev/null || true
probe_pid=""

echo "smoke-security-context: OK"
