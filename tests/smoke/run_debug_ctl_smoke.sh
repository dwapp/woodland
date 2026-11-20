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
client1_pid=""
client2_pid=""

stop_comp() {
    smoke_stop_pid "$client2_pid" 10
    client2_pid=""
    smoke_stop_pid "$client1_pid" 10
    client1_pid=""
    smoke_stop_compositor
}
smoke_cleanup() {
    stop_comp
    rm -rf "$XDG_RUNTIME_DIR"
}

# wctl 在 ok=false 时退出码为 1：这里要读响应，不能被 set -e 吃掉
w() { "$WCTL" -s "$SOCK" "$@" 2>&1 || true; }

# ---------- 1. 默认只读 ----------
echo "== disabled by default =="
smoke_start_compositor --self-check --trace -s "$CLIENT"
"$WCTL" -s "$SOCK" -t 5000 wait view_mapped >/dev/null || smoke_fail "client never mapped"
ID="$(w get_tree | jq '.views[0].id')"
BEFORE="$(w get_tree | jq -c '.views[0].outer')"
w move "$ID" 300 200 | jq -e '.ok == false and (.error | test("--debug-ctl"))' >/dev/null \
    || smoke_fail "move must be refused without --debug-ctl"
[ "$(w get_tree | jq -c '.views[0].outer')" = "$BEFORE" ] \
    || smoke_fail "a refused move changed the window"
w key 30 tap | jq -e '.ok == false and (.error | test("--debug-ctl"))' >/dev/null \
    || smoke_fail "key must be refused without --debug-ctl"
w screenshot --dump-ppm "$XDG_RUNTIME_DIR/nope.ppm" | jq -e '.ok == false and (.error | test("--debug-ctl"))' >/dev/null \
    || smoke_fail "--dump-ppm must be refused without --debug-ctl"
[ ! -e "$XDG_RUNTIME_DIR/nope.ppm" ] || smoke_fail "--dump-ppm wrote file without --debug-ctl"
w help | jq -e '[.commands[].name] | (index("move") | not) and (index("key") | not)' >/dev/null \
    || smoke_fail "help must not list write commands when disabled"
stop_comp

# ---------- 2. 打开之后 ----------
echo "== enabled =="
smoke_start_compositor --debug-ctl --self-check --trace
"$CLIENT" --log-keys >"$XDG_RUNTIME_DIR/client1.log" 2>&1 &
client1_pid=$!
"$WCTL" -s "$SOCK" -t 5000 wait view_mapped >/dev/null || smoke_fail "client never mapped"
ID="$(w get_tree | jq '.views[0].id')"

w help | jq -e '[.commands[].name] | (index("move") != null) and (index("inject_key") != null) and (index("key") != null)' >/dev/null \
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

# fullscreen / resize / inject_key / key
w fullscreen "$ID" 1 | jq -e '.ok == true' >/dev/null || smoke_fail "fullscreen failed"
w get_tree | jq -e '.views[0].fullscreen == true' >/dev/null || smoke_fail "view should be fullscreen"
w move "$ID" 5 5 | jq -e '.ok == false' >/dev/null || smoke_fail "move must be refused while fullscreen"
w resize "$ID" 400 300 | jq -e '.ok == false' >/dev/null || smoke_fail "resize must be refused while fullscreen"
w fullscreen "$ID" 0 | jq -e '.ok == true' >/dev/null || smoke_fail "leave fullscreen failed"
w resize "$ID" 400 300 | jq -e '.ok == true' >/dev/null || smoke_fail "resize failed"
w resize "$ID" 0 300 | jq -e '.ok == false' >/dev/null || smoke_fail "zero-size resize should fail"
w focus_view "$ID" | jq -e ".ok == true" >/dev/null || smoke_fail "refocus failed"
"$WCTL" -s "$SOCK" -t 2000 wait focus view="$ID" >/dev/null || smoke_fail "focus never settled"

echo "== inject_key -> real Wayland key delivery =="
w inject_key 30 tap | jq -e '.ok == true' >/dev/null || smoke_fail "inject_key should succeed on focused client"
i=0
while [ "$i" -lt 40 ]; do
    if grep -q '^KEY 30 1$' "$XDG_RUNTIME_DIR/client1.log" 2>/dev/null &&
       grep -q '^KEY 30 0$' "$XDG_RUNTIME_DIR/client1.log" 2>/dev/null; then
        break
    fi
    sleep 0.05; i=$((i+1))
done
grep -q '^KEY 30 1$' "$XDG_RUNTIME_DIR/client1.log" && grep -q '^KEY 30 0$' "$XDG_RUNTIME_DIR/client1.log" \
    || smoke_fail "client did not receive inject_key events: $(cat "$XDG_RUNTIME_DIR/client1.log" 2>/dev/null || true)"
w inject_key 30 smash | jq -e '.ok == false' >/dev/null || smoke_fail "bad key action should fail"

w workspace 3 | jq -e '.ok == true' >/dev/null || smoke_fail "workspace 3 failed"
w inject_key 30 tap | jq -e '.ok == false and (.error | startswith("no keyboard focus"))' >/dev/null \
    || smoke_fail "inject_key should report missing keyboard focus on empty workspace"
w workspace 1 | jq -e '.ok == true' >/dev/null || smoke_fail "workspace 1 failed"
w focus_view "$ID" | jq -e '.ok == true' >/dev/null || smoke_fail "refocus on ws1 failed"

echo "== debug virtual keyboard shortcuts / UI interactions =="
w key 30 smash | jq -e '.ok == false' >/dev/null || smoke_fail "bad key action should fail"
w key 0 tap | jq -e '.ok == false' >/dev/null || smoke_fail "zero keycode should fail"

# 1) Alt+2 / Alt+1 切工作区
w key 56 press | jq -e '.ok == true' >/dev/null || smoke_fail "Alt press failed"
w key 3 tap | jq -e '.ok == true' >/dev/null || smoke_fail "2 tap failed"
w key 56 release | jq -e '.ok == true' >/dev/null || smoke_fail "Alt release failed"
"$WCTL" -s "$SOCK" -t 2000 wait workspace name=2 >/dev/null || smoke_fail "Alt+2 did not switch workspace"
w key 56 press | jq -e '.ok == true' >/dev/null || smoke_fail "Alt press failed"
w key 2 tap | jq -e '.ok == true' >/dev/null || smoke_fail "1 tap failed"
w key 56 release | jq -e '.ok == true' >/dev/null || smoke_fail "Alt release failed"
"$WCTL" -s "$SOCK" -t 2000 wait workspace name=1 >/dev/null || smoke_fail "Alt+1 did not switch back"

# 2) Alt+S 多任务视图 + Esc 关闭
w key 56 press | jq -e '.ok == true' >/dev/null || smoke_fail "Alt press failed"
w key 31 tap | jq -e '.ok == true' >/dev/null || smoke_fail "S tap failed"
w key 56 release | jq -e '.ok == true' >/dev/null || smoke_fail "Alt release failed"
"$WCTL" -s "$SOCK" -t 2000 wait multitask active=1 >/dev/null || smoke_fail "Alt+S did not open multitask"
w key 1 tap | jq -e '.ok == true' >/dev/null || smoke_fail "Esc tap failed"
"$WCTL" -s "$SOCK" -t 2000 wait multitask active=0 >/dev/null || smoke_fail "Esc did not close multitask"

# 3) 右键标题栏打开窗口菜单 + Esc 关闭
w move "$ID" 200 150 | jq -e '.ok == true' >/dev/null || smoke_fail "move before menu failed"
w pointer_move 260 160 | jq -e '.ok == true' >/dev/null || smoke_fail "pointer_move to titlebar failed"
w pointer_button right click | jq -e '.ok == true' >/dev/null || smoke_fail "right click titlebar failed"
"$WCTL" -s "$SOCK" -t 2000 wait window_menu active=1 >/dev/null || smoke_fail "right click did not open window_menu"
w key 1 tap | jq -e '.ok == true' >/dev/null || smoke_fail "Esc menu failed"
"$WCTL" -s "$SOCK" -t 2000 wait window_menu active=0 >/dev/null || smoke_fail "Esc did not close window_menu"

# 4) 拖标题栏贴左边显示 snap_preview 并左半屏贴边
w pointer_move 260 160 | jq -e '.ok == true' >/dev/null || smoke_fail "pointer_move to titlebar failed"
w pointer_button left press | jq -e '.ok == true' >/dev/null || smoke_fail "left press failed"
w pointer_move 2 360 | jq -e '.ok == true' >/dev/null || smoke_fail "drag to left edge failed"
"$WCTL" -s "$SOCK" -t 2000 wait snap_preview active=1 | jq -e '.edges > 0' >/dev/null \
    || smoke_fail "snap_preview did not activate on left edge drag"
w pointer_button left release | jq -e '.ok == true' >/dev/null || smoke_fail "left release failed"
"$WCTL" -s "$SOCK" -t 2000 wait snap_preview active=0 >/dev/null || smoke_fail "snap_preview did not clear on release"
w get_tree | jq -e ".views[] | select(.id == $ID) | .outer[0] == 0" >/dev/null \
    || smoke_fail "view did not snap to left edge"

# 5) Alt+Tab 窗口切换器
"$CLIENT" --title simple-ssd-2 >"$XDG_RUNTIME_DIR/client2.log" 2>&1 &
client2_pid=$!
"$WCTL" -s "$SOCK" -t 5000 wait view_mapped title=simple-ssd-2 >/dev/null || smoke_fail "2nd client never mapped"
"$WCTL" -s "$SOCK" -t 2000 wait focus title=simple-ssd-2 >/dev/null || smoke_fail "2nd client never focused"
BEFORE_FOCUS="$(w get_focus | jq -r '.view')"
w key 56 press | jq -e '.ok == true' >/dev/null || smoke_fail "Alt press failed"
w key 15 tap | jq -e '.ok == true' >/dev/null || smoke_fail "Tab tap failed"
"$WCTL" -s "$SOCK" -t 2000 wait window_switcher active=1 >/dev/null || smoke_fail "Alt+Tab did not open switcher"
w key 56 release | jq -e '.ok == true' >/dev/null || smoke_fail "Alt release failed"
"$WCTL" -s "$SOCK" -t 2000 wait window_switcher active=0 >/dev/null || smoke_fail "Alt release did not commit switcher"
AFTER_FOCUS="$(w get_focus | jq -r '.view')"
[ "$AFTER_FOCUS" != "$BEFORE_FOCUS" ] || smoke_fail "Alt+Tab did not switch focus ($BEFORE_FOCUS -> $AFTER_FOCUS)"
smoke_stop_pid "$client2_pid" 10
client2_pid=""
"$WCTL" -s "$SOCK" -t 3000 wait view_count count=1 >/dev/null || smoke_fail "2nd client did not unmap"

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
