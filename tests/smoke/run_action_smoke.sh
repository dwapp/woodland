#!/usr/bin/env sh
# treeland-compositor-action-unstable-v1 冒烟：
#   bind / trigger / destroy、工作区动作真生效、置空动作不假装生效、
#   非法枚举静默忽略，以及每一步都能在 wctl 与 --trace 里查得到。
#
# 用法: sh run_action_smoke.sh <woodland 二进制> <action_probe> <wctl>
set -eu

. "$(dirname "$0")/lib.sh"
smoke_init compositor-action

BIN="${1:-./build/src/woodland}"
PROBE="${2:-./build/tests/action_probe}"
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
SMOKE_FLAGS="--trace"
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

# 发一条 trigger；探针自己会断言连接没被协议错误杀掉
trigger() {
    set +e
    "$PROBE" --trigger "$1"
    _rc=$?
    set -e
    [ "$_rc" -eq 0 ] || smoke_fail "probe --trigger $1 exited with $_rc"
}

# 断言 workspace / 最近一次处置 / 累计请求数，四者一起比
expect_state() {
    _ws="$1" _act="$2" _out="$3" _count="$4"
    _tree="$(wctl_cmd get_tree)" || smoke_fail "get_tree failed"
    printf '%s' "$_tree" | jq -e --arg ws "$_ws" --argjson act "$_act" \
        --arg out "$_out" --argjson count "$_count" \
        '.workspace.current == $ws
         and .compositor_action.available == true
         and .compositor_action.last_action == $act
         and .compositor_action.last_outcome == $out
         and .compositor_action.trigger_count == $count' >/dev/null \
        || smoke_fail "unexpected state after action $_act: $_tree"
}

_tree="$(wctl_cmd get_tree)" || smoke_fail "initial get_tree failed"
printf '%s' "$_tree" | jq -e \
    '.compositor_action.available == true
     and .compositor_action.trigger_count == 0
     and .compositor_action.last_action == 0
     and .compositor_action.last_outcome == "none"' >/dev/null \
    || smoke_fail "protocol state was not exposed: $_tree"

# 真做的部分：指定工作区 + 前后切换
trigger 2;   expect_state 2  2  switched 1
trigger 13;  expect_state 1  13 switched 2   # prev_workspace
trigger 14;  expect_state 2  14 switched 3   # next_workspace
trigger 1;   expect_state 1  1  switched 4

# 不能执行的部分：边界不绕回、本合成器只有 10 个工作区
trigger 13;  expect_state 1  13 no_workspace 5
trigger 11;  expect_state 1  11 no_workspace 6
trigger 10;  expect_state 10 10 switched 7
trigger 14;  expect_state 10 14 no_workspace 8

# 置空的部分：不改状态、不发事件，但 wctl / 日志 / trace 要说明忽略了哪一个
trigger 23;  expect_state 10 23 unsupported 9   # lockscreen
trigger 30;  expect_state 10 30 unsupported 10  # shutdown_menu_log_out

# 枚举外的值：静默忽略（协议没有错误枚举，连协议错误都不该有）
trigger 999; expect_state 10 999 invalid 11
trigger 0;   expect_state 10 0   invalid 12

# destroy 请求不改变任何合成器状态，且连接仍然健康
set +e
"$PROBE" --destroy
_rc=$?
set -e
[ "$_rc" -eq 0 ] || smoke_fail "probe --destroy exited with $_rc"
expect_state 10 0 invalid 12

# 决策轨迹：12 条 action（每条 trigger 一条），5 次真正的 workspace 切换
# （1→2→1→2→1→10），并且把"未实现"写成决策依据而不是含糊的 ignore
_trace="$(wctl_cmd get_trace)" || smoke_fail "get_trace failed"
printf '%s' "$_trace" | jq -e \
    '[.entries[] | select(.what == "action")] | length == 12' >/dev/null \
    || smoke_fail "expected 12 action trace entries: $_trace"
printf '%s' "$_trace" | jq -e \
    '[.entries[] | select(.what == "workspace")] | length == 5' >/dev/null \
    || smoke_fail "expected 5 workspace switches in trace: $_trace"
printf '%s' "$_trace" | jq -e \
    '[.entries[] | select(.what == "action"
        and (.detail | contains("not implemented: lockscreen")))] | length == 1' >/dev/null \
    || smoke_fail "unsupported action not explained in trace: $_trace"

grep -q 'treeland-compositor-action-v1' "$SMOKE_LOG" \
    || smoke_fail "compositor never advertised the action global"
grep -q 'not implemented: lockscreen' "$SMOKE_LOG" \
    || smoke_fail "compositor did not log the ignored lockscreen action"

# 工作区切了六次，焦点/激活态等不变量必须还成立
_check="$(wctl_cmd self_check)" || smoke_fail "self_check failed: $_check"
printf '%s' "$_check" | jq -e '.ok == true' >/dev/null \
    || smoke_fail "self_check invariants broken: $_check"

echo "compositor-action smoke passed"
