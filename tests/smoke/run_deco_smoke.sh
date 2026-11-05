#!/usr/bin/env sh
# treeland-decoration-unstable-v1 冒烟：起 headless 合成器 + 真客户端，然后用
# wctl 断言 get_tree 里的 deco / ssd.margins。
#
# 断言的是**状态**而不是像素：标题栏隐藏后边距是不是只剩边框、边框宽度有没有
# 进边距、第二份 context 有没有被打成 already_used——这些用 jq 一句话就说清，
# 截图只能回答"看起来像不像"。
#
# 用法: sh run_deco_smoke.sh <woodland 二进制> <deco_probe> <wctl>
set -eu

. "$(dirname "$0")/lib.sh"
smoke_init decoration

BIN="${1:-./build/src/woodland}"
PROBE="${2:-./build/tests/deco_probe}"
WCTL="${3:-./build/src/wctl/wctl}"
[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 125; }
[ -x "$PROBE" ] || { echo "probe not built: $PROBE" >&2; exit 125; }
[ -x "$WCTL" ] || { echo "wctl not found: $WCTL" >&2; exit 125; }
command -v jq >/dev/null 2>&1 || { echo "jq not found, skip decoration smoke" >&2; exit 77; }

unset WAYLAND_DISPLAY
export XDG_RUNTIME_DIR="$(mktemp -d)"
# 断言的基准是内置默认外观（26 标题栏 + 1 边框），别让 ~/.config 里的
# 用户配置把它改掉
export XDG_CONFIG_HOME="$XDG_RUNTIME_DIR/config"
mkdir -p "$XDG_CONFIG_HOME"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1

SMOKE_LOG="$XDG_RUNTIME_DIR/woodland.log"
SMOKE_FLAGS="--self-check --trace"
comp_pid=""
probe_pid=""
SOCK=""
TREE=""

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

# 等到视图数回到 0（上一个场景的窗口撤干净）
wait_view_count() {
    _want="$1"
    _i=0
    while [ "$_i" -lt 100 ]; do
        [ "$(view_count)" = "$_want" ] && return 0
        sleep 0.05; _i=$((_i+1))
    done
    return 1
}

# 起一个探针并等它就绪，然后把这一刻的 get_tree 抓进 $TREE
scenario() {
    _name="$1"; shift
    _out="$XDG_RUNTIME_DIR/probe.$_name.log"
    "$PROBE" "$@" >"$_out" 2>&1 &
    probe_pid=$!
    _i=0
    while [ "$_i" -lt 120 ]; do
        if grep -q '^READY' "$_out" 2>/dev/null; then
            break
        fi
        if ! kill -0 "$probe_pid" 2>/dev/null; then
            echo "--- probe output ($_name):" >&2
            cat "$_out" >&2 || true
            smoke_fail "probe exited before READY ($_name)"
        fi
        sleep 0.05; _i=$((_i+1))
    done
    grep -q '^READY' "$_out" 2>/dev/null || smoke_fail "probe never became ready ($_name)"

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

# expect <jq 表达式> <说明>：表达式在当前 $TREE 上求值
expect() {
    echo "$TREE" | jq -e "$1" >/dev/null || {
        echo "--- get_tree:" >&2
        echo "$TREE" | jq '.views[0] | {deco, ssd, outer, content}' >&2 || true
        smoke_fail "$2"
    }
}

echo "== 默认：没有任何 decoration context =="
scenario default --no-context
expect '.views[0].deco.managed == false' "no context yet, managed must be false"
expect '.views[0].deco.rendered == []' "nothing overridden, rendered must be empty"
# 内置默认：1px 边框 + 26px 标题栏
expect '.views[0].ssd.margins == [1, 27, 1, 1]' "default margins must be border+titlebar"
stop_probe

echo "== set_border：宽度进边距，颜色记录 =="
scenario border border 5 255 0 0 255
expect '.views[0].deco.managed == true' "context must mark the view as managed"
expect '.views[0].deco.border_width == 5' "border width must be 5"
expect '.views[0].deco.border_rgba == [255, 0, 0, 255]' "border color must be recorded"
expect '.views[0].deco.rendered == ["border"]' "border must be rendered"
expect '.views[0].ssd.margins == [5, 31, 5, 5]' "margins must follow the new border width"
stop_probe

echo "== set_titlebar_mode(hide)：边距只剩边框 =="
scenario hide titlebar 1
expect '.views[0].deco.titlebar == "hidden"' "titlebar must be hidden"
expect '.views[0].deco.rendered == ["titlebar"]' "titlebar must be rendered"
expect '.views[0].ssd.margins == [1, 1, 1, 1]' "hidden titlebar leaves only the border"
stop_probe

echo "== 半 CSD 半 SSD：隐藏标题栏 + 加粗边框 =="
scenario half border 8 0 0 0 255 titlebar 1
expect '.views[0].ssd.margins == [8, 8, 8, 8]' "no titlebar, 8px border all around"
expect '.views[0].deco.border_width == 8' "border width must be 8"
stop_probe

echo "== border 0 + hide：完全没有装饰 =="
scenario bare border 0 0 0 0 0 titlebar 1
expect '.views[0].ssd.margins == [0, 0, 0, 0]' "zero border + hidden titlebar means no decoration"
stop_probe

echo "== 圆角/阴影：只记录，不算 rendered =="
scenario effect radius 12 shadow 40 0 10 0 0 0 102
expect '.views[0].deco.corner_radius == 12' "corner radius must be recorded"
expect '.views[0].deco.shadow_radius == 40' "shadow radius must be recorded"
expect '.views[0].deco.shadow_offset == [0, 10]' "shadow offset must be recorded"
expect '.views[0].deco.rendered == []' "radius/shadow have no render backend"
expect '.views[0].ssd.margins == [1, 27, 1, 1]' "radius/shadow must not change geometry"
stop_probe

echo "== context 销毁：边框恢复默认，标题栏保持隐藏 =="
OUT_REVERT="$XDG_RUNTIME_DIR/probe.revert.log"
"$PROBE" border 6 255 0 0 255 titlebar 1 --drop-context --hold 3000 >"$OUT_REVERT" 2>&1 &
probe_pid=$!
i=0
while [ "$i" -lt 120 ]; do
    grep -q '^REVERTED' "$OUT_REVERT" 2>/dev/null && break
    kill -0 "$probe_pid" 2>/dev/null || smoke_fail "probe died before REVERTED"
    sleep 0.05; i=$((i+1))
done
grep -q '^REVERTED' "$OUT_REVERT" 2>/dev/null || smoke_fail "context was never dropped"
TREE="$("$WCTL" -s "$SOCK" get_tree)" || smoke_fail "get_tree failed (revert)"
expect '.views[0].deco.managed == false' "dropped context must clear managed"
expect '.views[0].deco.border_width == null' "border override must be reverted"
expect '.views[0].deco.titlebar == "hidden"' "titlebar override outlives the context"
expect '.views[0].ssd.margins == [1, 1, 1, 1]' "default border back, titlebar still hidden"
stop_probe

echo "== 已有 context 时再建一个：already_used =="
DUP_OUT="$XDG_RUNTIME_DIR/probe.dup.log"
rc=0
"$PROBE" dup >"$DUP_OUT" 2>&1 || rc=$?
[ "$rc" = 3 ] || {
    echo "--- probe output (dup):" >&2
    cat "$DUP_OUT" >&2 || true
    smoke_fail "duplicate context must be rejected with a protocol error (rc=$rc)"
}
grep -q '^PROTOCOL_ERROR 0 treeland_decoration_manager_v1' "$DUP_OUT" \
    || smoke_fail "expected already_used (error 0) on the manager, got: $(cat "$DUP_OUT")"
wait_view_count 0 || smoke_fail "dup probe window did not go away"

echo "== 决策轨迹与不变量 =="
"$WCTL" -s "$SOCK" get_trace | jq -e '
    [.entries[] | select(.what == "deco")] | length >= 5' >/dev/null \
    || smoke_fail "deco trace entries missing"
"$WCTL" -s "$SOCK" get_trace | jq -e '
    [.entries[] | select(.what == "deco" and (.detail | contains("no render backend")))]
    | length >= 2' >/dev/null \
    || smoke_fail "trace must spell out that radius/shadow are not rendered"
"$WCTL" -s "$SOCK" self_check | jq -e '.ok == true' >/dev/null \
    || smoke_fail "self_check failed after decoration changes"

echo "decoration smoke passed"
