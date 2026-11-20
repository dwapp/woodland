#!/usr/bin/env sh
# --debug-ctl 写命令冒烟：
#   1. 不带 --debug-ctl：写命令必须被明确拒绝，状态不变，help 里不列出它们
#   2. 带 --debug-ctl：move / maximize / workspace / pointer_move / 坏参数
#
# 用法: sh run_debug_ctl_smoke.sh <woodland 二进制> <wctl 二进制> [客户端命令]
set -eu

. "$(dirname "$0")/lib.sh"
smoke_init debug_ctl

BIN="${1:-./build/src/woodland}"
WCTL="${2:-./build/src/wctl/wctl}"
CLIENT="${3:-weston-simple-shm}"
[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 125; }
[ -x "$WCTL" ] || { echo "wctl not found: $WCTL" >&2; exit 125; }
command -v jq >/dev/null 2>&1 || { echo "jq not found, skip" >&2; exit 77; }
if [ ! -x "$CLIENT" ] && ! command -v "$CLIENT" >/dev/null 2>&1; then
    echo "$CLIENT not found, skip" >&2
    exit 77
fi

unset WAYLAND_DISPLAY
export XDG_RUNTIME_DIR="$(mktemp -d)"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1

SMOKE_LOG="$XDG_RUNTIME_DIR/woodland.log"
comp_pid=""

stop_comp() {
    if [ -n "$comp_pid" ]; then
        kill -TERM "$comp_pid" 2>/dev/null || true
        i=0
        while kill -0 "$comp_pid" 2>/dev/null && [ "$i" -lt 50 ]; do
            sleep 0.1; i=$((i+1))
        done
        kill -KILL "$comp_pid" 2>/dev/null || true
        wait "$comp_pid" 2>/dev/null || true
        comp_pid=""
    fi
    rm -f "$XDG_RUNTIME_DIR"/woodland-*.sock
}
smoke_cleanup() {
    stop_comp
    rm -rf "$XDG_RUNTIME_DIR"
}

# start_comp <额外参数...>：起合成器 + 客户端，等 socket 与 1 个视图
start_comp() {
    SMOKE_FLAGS="--self-check --trace $*"
    # shellcheck disable=SC2086
    "$BIN" --self-check --trace "$@" -s "$CLIENT" >"$SMOKE_LOG" 2>&1 &
    comp_pid=$!
    SOCK=""
    i=0
    while [ "$i" -lt 100 ]; do
        for f in "$XDG_RUNTIME_DIR"/woodland-*.sock; do
            [ -S "$f" ] && SOCK="$f"
        done
        [ -n "$SOCK" ] && break
        sleep 0.05; i=$((i+1))
    done
    [ -n "$SOCK" ] || smoke_fail "debug socket never appeared"
    "$WCTL" -s "$SOCK" -t 5000 wait view_mapped >/dev/null || smoke_fail "client never mapped"
    return 0
}

# wctl 在 ok=false 时退出码为 1：这里要读响应，不能被 set -e 吃掉
w() { "$WCTL" -s "$SOCK" "$@" 2>&1 || true; }

# ---------- 1. 默认只读 ----------
echo "== disabled by default =="
start_comp
ID="$(w get_tree | jq '.views[0].id')"
BEFORE="$(w get_tree | jq -c '.views[0].outer')"
w move "$ID" 300 200 | jq -e '.ok == false and (.error | test("--debug-ctl"))' >/dev/null \
    || smoke_fail "move must be refused without --debug-ctl"
[ "$(w get_tree | jq -c '.views[0].outer')" = "$BEFORE" ] \
    || smoke_fail "a refused move changed the window"
w help | jq -e '[.commands[].name] | index("move") | not' >/dev/null \
    || smoke_fail "help must not list write commands when disabled"
stop_comp

# ---------- 2. 打开之后 ----------
echo "== enabled =="
start_comp --debug-ctl
ID="$(w get_tree | jq '.views[0].id')"

w help | jq -e '[.commands[].name] | (index("move") != null) and (index("inject_key") != null)' >/dev/null \
    || smoke_fail "help should list write commands"

w move "$ID" 300 200 | jq -e '.ok == true' >/dev/null || smoke_fail "move failed"
w get_tree | jq -e '.views[0].outer[0] == 300 and .views[0].outer[1] == 200' >/dev/null \
    || smoke_fail "outer box should be at 300,200 after move"

w maximize "$ID" 1 | jq -e '.ok == true' >/dev/null || smoke_fail "maximize failed"
w get_tree | jq -e '.views[0].maximized == true' >/dev/null || smoke_fail "view should be maximized"
w maximize "$ID" 0 | jq -e '.ok == true' >/dev/null || smoke_fail "unmaximize failed"
w get_tree | jq -e '.views[0].maximized == false' >/dev/null || smoke_fail "view should be restored"

w focus_view "$ID" | jq -e '.ok == true' >/dev/null || smoke_fail "focus failed"
w get_focus | jq -e ".view == $ID" >/dev/null || smoke_fail "focus did not land on the view"

w workspace 2 | jq -e '.ok == true' >/dev/null || smoke_fail "workspace 2 failed"
w get_tree | jq -e '.workspace.current == "2"' >/dev/null || smoke_fail "workspace should be 2"
w workspace 1 | jq -e '.ok == true' >/dev/null || smoke_fail "workspace 1 failed"

w pointer_move 640 360 | jq -e '.ok == true' >/dev/null || smoke_fail "pointer_move failed"

# fullscreen / resize / inject_key
w fullscreen "$ID" 1 | jq -e '.ok == true' >/dev/null || smoke_fail "fullscreen failed"
w get_tree | jq -e '.views[0].fullscreen == true' >/dev/null || smoke_fail "view should be fullscreen"
w move "$ID" 5 5 | jq -e '.ok == false' >/dev/null || smoke_fail "move must be refused while fullscreen"
w resize "$ID" 400 300 | jq -e '.ok == false' >/dev/null || smoke_fail "resize must be refused while fullscreen"
w fullscreen "$ID" 0 | jq -e '.ok == true' >/dev/null || smoke_fail "leave fullscreen failed"
w resize "$ID" 400 300 | jq -e '.ok == true' >/dev/null || smoke_fail "resize failed"
w resize "$ID" 0 300 | jq -e '.ok == false' >/dev/null || smoke_fail "zero-size resize should fail"
w focus_view "$ID" | jq -e ".ok == true" >/dev/null || smoke_fail "refocus failed"
# headless 没有键盘设备：只能断言"没有键盘焦点"这条明确的错误；
# 真正送键的路径要在有 seat keyboard 的会话里验证
w inject_key 30 tap | jq -e '.ok == false and (.error | startswith("no keyboard focus"))' >/dev/null \
    || smoke_fail "inject_key should report missing keyboard focus on a headless seat"
w inject_key 30 smash | jq -e '.ok == false' >/dev/null || smoke_fail "bad key action should fail"
w pointer_button left click | jq -e '.ok == true' >/dev/null || smoke_fail "pointer_button failed"

# 坏参数：明确报错、不改状态、合成器不崩
w move "$ID" abc 1 | jq -e '.ok == false' >/dev/null || smoke_fail "bad number should fail"
w move 99999 1 1 | jq -e '.ok == false and .error == "no such view"' >/dev/null \
    || smoke_fail "unknown view id should fail"
w move "$ID" 1 | jq -e '.ok == false' >/dev/null || smoke_fail "missing arg should fail"
w move "$ID" 1 2 3 | jq -e '.ok == false' >/dev/null || smoke_fail "extra arg should fail"
w workspace nope | jq -e '.ok == false' >/dev/null || smoke_fail "unknown workspace should fail"
w pointer_button left smash | jq -e '.ok == false' >/dev/null || smoke_fail "bad action should fail"

# 写命令留下决策轨迹，且不破坏不变量
w get_trace | jq -e '[.entries[] | select(.what == "action" and (.detail | startswith("debug-ctl")))] | length > 0' \
    >/dev/null || smoke_fail "debug-ctl commands should leave an action trace"
w self_check | jq -e '.ok == true' >/dev/null || smoke_fail "self_check failed after debug-ctl commands"
w ping | jq -e '.ok == true' >/dev/null || smoke_fail "compositor stopped answering"

echo "OK"
