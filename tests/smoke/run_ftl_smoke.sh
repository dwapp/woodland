#!/usr/bin/env sh
# treeland-foreign-toplevel-manager-v3 冒烟：起 headless 合成器 + 一个双角色探针
# （既是窗口客户端又是 Dock），断言协议事件与合成器内部状态两边对得上。
#
# 断言的是**状态**而不是渲染：探针打出它收到的 handle 属性快照，wctl 打出
# 合成器认为的窗口状态。两边都查，才能抓到"事件发出去了但这头没照做"
# （只查探针会漏，只查 wctl 会漏"根本没发事件"）。
#
# 用法: sh run_ftl_smoke.sh <woodland 二进制> <ftl_probe> <wctl>
set -eu

. "$(dirname "$0")/lib.sh"
smoke_init foreign_toplevel

BIN="${1:-./build/src/woodland}"
PROBE="${2:-./build/tests/ftl_probe}"
WCTL="${3:-./build/src/wctl/wctl}"
[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 125; }
[ -x "$PROBE" ] || { echo "probe not built: $PROBE" >&2; exit 125; }
[ -x "$WCTL" ] || { echo "wctl not found: $WCTL" >&2; exit 125; }
command -v jq >/dev/null 2>&1 || { echo "jq not found, skip foreign-toplevel smoke" >&2; exit 77; }

unset WAYLAND_DISPLAY
export XDG_RUNTIME_DIR="$(mktemp -d)"
export XDG_CONFIG_HOME="$XDG_RUNTIME_DIR/config"
mkdir -p "$XDG_CONFIG_HOME"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1

SMOKE_LOG="$XDG_RUNTIME_DIR/woodland.log"
SMOKE_FLAGS="--self-check --trace"
comp_pid=""
probe_pid=""
SOCK=""
TREE=""
PROBE_OUT=""

smoke_cleanup() {
    if [ -n "$probe_pid" ]; then
        kill -KILL "$probe_pid" 2>/dev/null || true
        probe_pid=""
    fi
    if [ -n "$comp_pid" ]; then
        kill -TERM "$comp_pid" 2>/dev/null || true
        i=0
        while kill -0 "$comp_pid" 2>/dev/null && [ "$i" -lt 50 ]; do
            sleep 0.1; i=$((i+1))
        done
        kill -KILL "$comp_pid" 2>/dev/null || true
        wait "$comp_pid" 2>/dev/null || true
    fi
    rm -rf "$XDG_RUNTIME_DIR"
}

"$BIN" --self-check --trace >"$SMOKE_LOG" 2>&1 &
comp_pid=$!

i=0
while [ "$i" -lt 100 ]; do
    for f in "$XDG_RUNTIME_DIR"/woodland-*.sock; do
        [ -S "$f" ] && SOCK="$f"
    done
    [ -n "$SOCK" ] && break
    sleep 0.05; i=$((i+1))
done
[ -n "$SOCK" ] || smoke_fail "debug socket never appeared"

view_count() {
    "$WCTL" -s "$SOCK" get_tree 2>/dev/null | jq -r '.count // -1'
}

wait_view_count() {
    _want="$1"
    _i=0
    while [ "$_i" -lt 100 ]; do
        [ "$(view_count)" = "$_want" ] && return 0
        sleep 0.05; _i=$((_i+1))
    done
    return 1
}

# 起探针 → 等 OPS_DONE（所有请求都发完并留过处理窗口）→ 抓这一刻的 get_tree
scenario() {
    _name="$1"; shift
    PROBE_OUT="$XDG_RUNTIME_DIR/probe.$_name.log"
    "$PROBE" --hold 4000 "$@" >"$PROBE_OUT" 2>&1 &
    probe_pid=$!
    _i=0
    while [ "$_i" -lt 200 ]; do
        if grep -q '^OPS_DONE' "$PROBE_OUT" 2>/dev/null; then
            break
        fi
        if ! kill -0 "$probe_pid" 2>/dev/null; then
            echo "--- probe output ($_name):" >&2
            cat "$PROBE_OUT" >&2 || true
            smoke_fail "probe exited before OPS_DONE ($_name)"
        fi
        sleep 0.05; _i=$((_i+1))
    done
    grep -q '^OPS_DONE' "$PROBE_OUT" 2>/dev/null || smoke_fail "probe never finished its ops ($_name)"
    wait_view_count 1 || smoke_fail "view did not appear ($_name)"
    TREE="$("$WCTL" -s "$SOCK" get_tree)" || smoke_fail "get_tree failed ($_name)"
}

stop_probe() {
    if [ -n "$probe_pid" ]; then
        kill -TERM "$probe_pid" 2>/dev/null || true
        wait "$probe_pid" 2>/dev/null || true
        probe_pid=""
    fi
    wait_view_count 0 || smoke_fail "previous probe window did not go away"
}

# expect <jq 表达式> <说明>：在 $TREE 上求值
expect() {
    echo "$TREE" | jq -e "$1" >/dev/null || {
        echo "--- get_tree:" >&2
        echo "$TREE" | jq '{toplevel_manager, view: (.views[0] | {id, title, app_id, maximized, minimized, fullscreen, activated, foreign_toplevel})}' >&2 || true
        smoke_fail "$2"
    }
}

# expect_probe <grep 模式> <说明>
expect_probe() {
    grep -q "$1" "$PROBE_OUT" 2>/dev/null || {
        echo "--- probe output:" >&2
        cat "$PROBE_OUT" >&2 || true
        smoke_fail "$2"
    }
}

echo "== 窗口 map → handle 出现，属性快照完整 =="
scenario basic
expect '.toplevel_manager.available == true' "protocol family must be advertised"
expect '.toplevel_manager.bindings == 1' "one manager binding"
expect '.toplevel_manager.toplevels == 1' "one tracked toplevel"
expect '.views[0].foreign_toplevel.tracked == true' "the mapped view must be tracked"
expect '.views[0].foreign_toplevel.identifier > 0' "identifier must be assigned"
expect '.views[0].foreign_toplevel.handles == 1' "one handle per binding"
IDENT="$(echo "$TREE" | jq -r '.views[0].foreign_toplevel.identifier')"
expect_probe "^PROPS ident=$IDENT " "probe must see the same identifier as wctl"
expect_probe "^PROPS ident=$IDENT title=ftl-probe " "title must arrive before done"
expect_probe "^PROPS ident=$IDENT title=ftl-probe app=ftl.probe " "app_id must arrive"
# pid 必须是探针自己的进程号：协议里它是"拥有这个窗口的进程"
expect_probe "pid=$probe_pid " "pid must be the client's pid"
# 只有一个输出，且探针绑了 wl_output，所以必须收到一条 output_enter
expect_probe "outputs=1 " "output_enter must be sent for the bound output"
# 新窗口拿到焦点（headless 没有键盘设备，但 activated 是合成器记录的真值）
expect_probe "state=.*activated" "a freshly mapped window is activated"
stop_probe

echo "== bind 晚于 map：补快照 =="
scenario late-bind --late-bind
expect_probe '^TOPLEVEL' "late bind must still emit a toplevel event"
expect_probe '^PROPS' "late bind must send the initial property snapshot"
expect '.views[0].foreign_toplevel.tracked == true' "existing window must be tracked after late bind"
stop_probe

echo "== set_maximized：状态事件 + 合成器真的最大化 =="
scenario maximize max
expect '.views[0].maximized == true' "maximize request must be applied"
expect_probe 'state=.*maximized' "state event must report maximized"
expect '.toplevel_manager.last_request == "set_maximized"' "last request must be recorded"
expect '.toplevel_manager.last_outcome == "applied"' "outcome must be applied"
stop_probe

echo "== set_minimized：窗口隐藏但仍在列表里 =="
scenario minimize min
expect '.views[0].minimized == true' "minimize request must be applied"
expect '.views[0].scene_visible == false' "minimized window must not be drawn"
expect_probe 'state=.*minimized' "state event must report minimized"
stop_probe

echo "== set_fullscreen =="
scenario fullscreen full
expect '.views[0].fullscreen == true' "fullscreen request must be applied"
expect '.toplevel_manager.last_request == "set_fullscreen"' "fullscreen request must be recorded"
# probe 的 full 操作不传 output（NULL）：协议让合成器挑，outcome 必须如实说清
expect '.toplevel_manager.last_outcome == "applied on compositor choice"' \
    "null output must be reported as compositor choice"
expect_probe 'state=.*fullscreen' "state event must report fullscreen"
stop_probe

echo "== set_icon_geometry：只记录，不假装有最小化动画 =="
scenario icon icon 11 12 130 140
expect '.views[0].foreign_toplevel.icon_geometry == [11, 12, 130, 140]' "icon geometry must be recorded"
expect '.toplevel_manager.last_request == "set_icon_geometry"' "icon geometry request must be recorded"
expect '.toplevel_manager.last_outcome == "recorded (no minimize animation)"' \
    "outcome must spell out that nothing is animated"
stop_probe

echo "== activate：只认本合成器的 seat =="
scenario activate activate
expect '.toplevel_manager.last_request == "activate"' "activate must reach the protocol layer"
expect '.toplevel_manager.last_outcome == "focused"' "activate must focus the view"
stop_probe

echo "== dock preview：请求被接收但不渲染 =="
scenario dock show
expect '.toplevel_manager.contexts == 1' "dock preview context must exist"
expect '.toplevel_manager.dock_preview == "recorded"' "dock preview must report 'recorded', not 'rendered'"
expect '.toplevel_manager.last_dock_request == "show"' "show must be recorded"
"$WCTL" -s "$SOCK" get_trace | jq -e '
    [.entries[] | select(.what == "ftl" and (.detail | contains("popover not rendered")))] | length >= 1
' >/dev/null || smoke_fail "trace must spell out that the popover is not rendered"
stop_probe

echo "== show_tooltip_surface：没有提交内容的 surface 静默忽略 =="
scenario tooltip tooltip-empty
expect '.toplevel_manager.last_dock_request == "show_tooltip"' "tooltip request must be recorded"
"$WCTL" -s "$SOCK" get_trace | jq -e '
    [.entries[] | select(.what == "ftl" and (.detail | contains("no committed buffer")))] | length >= 1
' >/dev/null || smoke_fail "a tooltip without committed content must be ignored, and traced as such"
stop_probe

echo "== close：请求转发给客户端 =="
scenario close close
expect_probe '^CLOSE' "client must receive xdg_toplevel.close"
expect '.toplevel_manager.last_request == "close"' "close request must be recorded"
stop_probe

echo "== stop：finished 之后不再发 toplevel =="
scenario stop stop
expect_probe '^FINISHED' "stop must produce finished"
stop_probe

echo "== 第二个 context：context_already_exists =="
DUP_OUT="$XDG_RUNTIME_DIR/probe.dup.log"
rc=0
"$PROBE" dup-context >"$DUP_OUT" 2>&1 || rc=$?
[ "$rc" = 3 ] || {
    echo "--- probe output (dup):" >&2
    cat "$DUP_OUT" >&2 || true
    smoke_fail "duplicate dock preview context must be rejected (rc=$rc)"
}
grep -q '^PROTOCOL_ERROR 1 treeland_foreign_toplevel_manager_v3' "$DUP_OUT" \
    || smoke_fail "expected context_already_exists (error 1), got: $(cat "$DUP_OUT")"
wait_view_count 0 || smoke_fail "dup probe window did not go away"

echo "== 决策轨迹与不变量 =="
"$WCTL" -s "$SOCK" get_trace | jq -e '
    [.entries[] | select(.what == "ftl")] | length >= 10' >/dev/null \
    || smoke_fail "ftl trace entries missing"
"$WCTL" -s "$SOCK" get_trace | jq -e '
    [.entries[] | select(.what == "ftl" and (.detail | startswith("toplevel closed")))] | length >= 3' >/dev/null \
    || smoke_fail "every window that went away must produce a 'closed' trace"
"$WCTL" -s "$SOCK" self_check | jq -e '.ok == true' >/dev/null \
    || smoke_fail "self_check failed after foreign-toplevel traffic"

echo "foreign-toplevel smoke passed"
