#!/usr/bin/env sh
# treeland-shortcut-manager-unstable-v3 冒烟：
#   bind / acquire / occupied / not_acquired、bind_key 的成功与三种
#   bind_failure、upsert、手势只登记不触发、capture 的失败与取消，
#   以及每一步都能在 wctl、trace 和合成器日志里查得到。
#
# 用法: sh run_shortcut_smoke.sh <woodland 二进制> <shortcut_probe> <wctl>
set -eu

. "$(dirname "$0")/lib.sh"
smoke_init shortcut

BIN="${1:-./build/src/woodland}"
PROBE="${2:-./build/tests/shortcut_probe}"
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

export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1
SMOKE_LOG="$XDG_RUNTIME_DIR/log"
"$BIN" --trace >"$SMOKE_LOG" 2>&1 &
pid=$!
smoke_cleanup() {
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

probe() {
    set +e
    "$PROBE" "$@" >"$XDG_RUNTIME_DIR/probe.out" 2>"$XDG_RUNTIME_DIR/probe.err"
    _rc=$?
    set -e
    if [ "$_rc" -ne 0 ]; then
        echo "probe $* failed (rc=$_rc):" >&2
        cat "$XDG_RUNTIME_DIR/probe.err" >&2
        smoke_fail "probe $* failed"
    fi
    cat "$XDG_RUNTIME_DIR/probe.out"
}

_tree="$(wctl_cmd get_tree)" || smoke_fail "initial get_tree failed"
printf '%s' "$_tree" | jq -e \
    '.shortcut_manager.available == true
     and .shortcut_manager.acquired == false
     and .shortcut_manager.binding_count == 0
     and .shortcut_manager.capture_count == 0
     and .shortcut_manager.last_outcome == "none"
     and (.shortcut_manager.bindings | length) == 0' >/dev/null \
    || smoke_fail "shortcut state was not exposed: $_tree"

# ---- 生命周期：bind / destroy / acquire 释放独占 ----
probe bind
probe acquire
_tree="$(wctl_cmd get_tree)" || smoke_fail "get_tree after lifecycle failed"
printf '%s' "$_tree" | jq -e \
    '.shortcut_manager.acquired == false
     and .shortcut_manager.binding_count == 0' >/dev/null \
    || smoke_fail "destroy did not release the manager: $_tree"

# ---- bind_key 成功 + unbind：不留绑定，轨迹写明决策 ----
probe bind-key "ws1" "Ctrl+Alt+K" 1 1
_tree="$(wctl_cmd get_tree)" || smoke_fail "get_tree after bind-key failed"
printf '%s' "$_tree" | jq -e \
    '.shortcut_manager.binding_count == 0
     and .shortcut_manager.last_outcome == "unbound"' >/dev/null \
    || smoke_fail "bind-key/unbind state wrong: $_tree"

# ---- 四类 bind_failure：非法 key、枚举外动作、非法 flags、flags=0 ----
probe bind-failure "bad-key" "Ctrl+NoSuchKeyHere" 1 0 3
_tree="$(wctl_cmd get_tree)" || smoke_fail "get_tree after bad-key failed"
printf '%s' "$_tree" | jq -e \
    '.shortcut_manager.binding_count == 0
     and .shortcut_manager.last_outcome == "invalid_argument"' >/dev/null \
    || smoke_fail "bad key was not rejected: $_tree"

probe bind-failure "bad-action" "Ctrl+Alt+T" 1 999 3
probe bind-failure "bad-flags" "Ctrl+Alt+T" 8 0 3
# flags==0：三种 trigger 位全不设，永远不命中，按 out-of-range 拒绝
probe bind-failure "zero-flags" "Ctrl+Alt+Y" 0 0 3

# ---- name_conflict / duplicate_binding / upsert ----
probe name-conflict
_tree="$(wctl_cmd get_tree)" || smoke_fail "get_tree after name-conflict failed"
printf '%s' "$_tree" | jq -e \
    '.shortcut_manager.last_outcome == "name_conflict"
     and .shortcut_manager.binding_count == 1
     and (.shortcut_manager.bindings | length) == 1
     and .shortcut_manager.bindings[0].name == "dup"' >/dev/null \
    || smoke_fail "name conflict state wrong: $_tree"

probe duplicate-binding
_tree="$(wctl_cmd get_tree)" || smoke_fail "get_tree after duplicate failed"
printf '%s' "$_tree" | jq -e \
    '.shortcut_manager.last_outcome == "duplicate_binding"
     and .shortcut_manager.binding_count == 2
     and ([.shortcut_manager.bindings[] | select(.kind == "swipe")] | length) == 1' >/dev/null \
    || smoke_fail "duplicate swipe state wrong: $_tree"

probe upsert
_tree="$(wctl_cmd get_tree)" || smoke_fail "get_tree after upsert failed"
printf '%s' "$_tree" | jq -e \
    '.shortcut_manager.last_outcome == "upsert"
     and .shortcut_manager.binding_count == 3
     and ([.shortcut_manager.bindings[] | select(.kind == "key")] | length) == 2' >/dev/null \
    || smoke_fail "upsert should not add a binding: $_tree"
printf '%s' "$_tree" | jq -e \
    '[.shortcut_manager.bindings[] | select(.name == "first" and .flags == 2)] | length == 1' >/dev/null \
    || smoke_fail "upsert did not refresh the flags: $_tree"
printf '%s' "$_tree" | jq -e \
    '[.shortcut_manager.bindings[] | select(.name == "second")] | length == 0' >/dev/null \
    || smoke_fail "upsert created a second binding: $_tree"

# ---- 手势只登记不触发：绑定存在，但轨迹明写 gesture_unwired ----
probe gesture
_tree="$(wctl_cmd get_tree)" || smoke_fail "get_tree after gesture failed"
printf '%s' "$_tree" | jq -e \
    '.shortcut_manager.last_outcome == "gesture_unwired"
     and .shortcut_manager.binding_count == 5
     and ([.shortcut_manager.bindings[] | select(.kind == "hold")] | length) == 1' >/dev/null \
    || smoke_fail "gesture registration state wrong: $_tree"

# ---- 协议错误：未 acquire 就 bind、以及重复 acquire ----
probe not-acquired
_tree="$(wctl_cmd get_tree)" || smoke_fail "get_tree after not-acquired failed"
printf '%s' "$_tree" | jq -e \
    '.shortcut_manager.last_outcome == "not_acquired"' >/dev/null \
    || smoke_fail "not_acquired was not recorded: $_tree"

probe occupied
_tree="$(wctl_cmd get_tree)" || smoke_fail "get_tree after occupied failed"
printf '%s' "$_tree" | jq -e \
    '.shortcut_manager.last_outcome == "occupied"' >/dev/null \
    || smoke_fail "occupied was not recorded: $_tree"

# ---- capture：没有键盘焦点的 surface 必须立刻 not_active ----
probe capture-not-active
_tree="$(wctl_cmd get_tree)" || smoke_fail "get_tree after capture failed"
printf '%s' "$_tree" | jq -e \
    '.shortcut_manager.capture_count == 1
     and .shortcut_manager.last_outcome == "not_active"
     and .shortcut_manager.capture_pending == false' >/dev/null \
    || smoke_fail "capture not_active state wrong: $_tree"

# ---- capture：map 一个 toplevel 后尝试拿焦点；destroy 取消 ----
# headless 后端不提供键盘设备，seat 没有 keyboard，toplevel 拿不到键盘
# 焦点，所以这几条在这里一律走 not_active：这是环境限制，不是协议分支
# 没实现。注意 busy 依赖 drain_keycode 非零（那要一次真实 captured），在
# headless 下必然测不到，所以 smoke 只断言计数与无协议错误，busy 分支
# 的语义留给有真实输入设备的环境或单测覆盖。
probe capture
_tree="$(wctl_cmd get_tree)" || smoke_fail "get_tree after focused capture failed"
printf '%s' "$_tree" | jq -e \
    '.shortcut_manager.capture_count == 2
     and .shortcut_manager.last_outcome == "not_active"
     and .shortcut_manager.capture_pending == false' >/dev/null \
    || smoke_fail "focused capture was not counted: $_tree"

probe capture-busy
probe capture-destroy
_tree="$(wctl_cmd get_tree)" || smoke_fail "get_tree after capture series failed"
printf '%s' "$_tree" | jq -e \
    '.shortcut_manager.capture_count == 5
     and .shortcut_manager.capture_pending == false' >/dev/null \
    || smoke_fail "capture objects leaked or were not counted: $_tree"

# ---- 轨迹、日志与不变量 ----
_trace="$(wctl_cmd get_trace)" || smoke_fail "get_trace failed"
printf '%s' "$_trace" | jq -e \
    '[.entries[] | select(.what == "shortcut")] | length >= 10' >/dev/null \
    || smoke_fail "shortcut decisions missing from trace: $_trace"
printf '%s' "$_trace" | jq -e \
    '[.entries[] | select(.what == "shortcut"
        and (.detail | contains("gesture_unwired")))] | length >= 1' >/dev/null \
    || smoke_fail "unwired gestures not explained in trace: $_trace"
printf '%s' "$_trace" | jq -e \
    '[.entries[] | select(.what == "shortcut"
        and (.detail | contains("name_conflict")))] | length >= 1' >/dev/null \
    || smoke_fail "name_conflict not explained in trace: $_trace"

grep -q 'treeland-shortcut-manager-v3' "$SMOKE_LOG" \
    || smoke_fail "compositor never advertised the shortcut global"

_check="$(wctl_cmd self_check)" || smoke_fail "self_check failed: $_check"
printf '%s' "$_check" | jq -e '.ok == true' >/dev/null \
    || smoke_fail "self_check invariants broken: $_check"

echo "shortcut-manager smoke passed"
