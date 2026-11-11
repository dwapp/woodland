#!/usr/bin/env sh
# treeland-xwindow-control-unstable-v1 冒烟：
#   bind / destroy、每条请求都回一个 wl_callback.done、失败路径如实记账、
#   以及（有 Xwayland 时）按 X11 wid 真的把窗口摆到 anchor 左上角 + (dx,dy)。
#
# 不数像素：全部断言走 wctl get_tree / get_trace / dump_scene。
#
# 用法: sh run_xwindow_smoke.sh <woodland> <xwindow_probe> <wctl> [xoverride]
set -eu

. "$(dirname "$0")/lib.sh"
smoke_init xwindow-control

BIN="${1:-./build/src/woodland}"
PROBE="${2:-./build/tests/xwindow_probe}"
WCTL="${3:-}"
XOVERRIDE="${4:-}"

[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 125; }
[ -x "$PROBE" ] || { echo "probe not built: $PROBE" >&2; exit 125; }
if [ -z "$WCTL" ] || [ ! -x "$WCTL" ]; then
    echo "wctl not found: $WCTL" >&2
    exit 125
fi
command -v jq >/dev/null 2>&1 || { echo "jq is required" >&2; exit 125; }

unset WAYLAND_DISPLAY
unset DISPLAY
export XDG_RUNTIME_DIR="$(mktemp -d)"
export XDG_CONFIG_HOME="$XDG_RUNTIME_DIR/config"
export XDG_DATA_HOME="$XDG_RUNTIME_DIR/data"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME"
WORK="$(mktemp -d)"

export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1
SMOKE_FLAGS="--trace"
SMOKE_LOG="$WORK/comp.log"
"$BIN" --trace >"$SMOKE_LOG" 2>&1 &
pid=$!
x_pids=""

smoke_cleanup() {
    for p in $x_pids; do
        kill -TERM "$p" 2>/dev/null || true
    done
    kill -TERM "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    rm -rf "$XDG_RUNTIME_DIR" "$WORK"
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
    _out="$("$PROBE" "$@" 2>&1)"
    _rc=$?
    set -e
    [ "$_rc" -eq 0 ] || smoke_fail "probe $* exited $_rc: $_out"
    printf '%s\n' "$_out"
}

expect_xw() {
    _filter="$1"
    _tree="$(wctl_cmd get_tree)" || smoke_fail "get_tree failed"
    printf '%s' "$_tree" | jq -e ".xwindow_control | $_filter" >/dev/null \
        || smoke_fail "unexpected xwindow_control state ($_filter): $_tree"
}

# ---------------------------------------------------------------- 初始状态
_tree="$(wctl_cmd get_tree)" || smoke_fail "initial get_tree failed"
printf '%s' "$_tree" | jq -e \
    '.xwindow_control.available == true
     and .xwindow_control.request_count == 0
     and .xwindow_control.success_count == 0
     and .xwindow_control.failure_count == 0
     and .xwindow_control.last_wid == 0
     and .xwindow_control.last_outcome == "none"
     and .xwindow_control.last_anchor == null
     and .xwindow_control.last_target == null' >/dev/null \
    || smoke_fail "protocol state was not exposed: $_tree"

XWAYLAND="$(printf '%s' "$_tree" | jq -r '.xwindow_control.xwayland')"
# 没有 Xwayland（编译期关掉或 lazy 未起）时，请求一律回 no_xwayland；
# 这也让本脚本能在 build-noxwl 里照样验证协议接线。
if [ "$XWAYLAND" = "true" ]; then
    BARE_OUTCOME=invalid_anchor
    NOWID_OUTCOME=no_window
else
    BARE_OUTCOME=no_xwayland
    NOWID_OUTCOME=no_xwayland
fi

# ------------------------------------------------- bind / destroy 不产生状态
_out="$(probe --bind)"
printf '%s' "$_out" | grep -q '^bind ok$' || smoke_fail "bind failed: $_out"
_out="$(probe --destroy)"
printf '%s' "$_out" | grep -q '^destroy ok$' || smoke_fail "destroy failed: $_out"
expect_xw '.available == true and .request_count == 0 and .last_outcome == "none"'

# --------------------------------------- 失败 1：裸 wl_surface 不在场景里
_out="$(probe --move bare 0 0 0)"
printf '%s' "$_out" | grep -q '^callback:1$' || smoke_fail "bare anchor should fail: $_out"
_tree="$(wctl_cmd get_tree)" || smoke_fail "get_tree failed"
printf '%s' "$_tree" | jq -e --arg out "$BARE_OUTCOME" \
    '.xwindow_control.request_count == 1
     and .xwindow_control.success_count == 0
     and .xwindow_control.failure_count == 1
     and .xwindow_control.last_outcome == $out
     and .xwindow_control.last_anchor == null
     and .xwindow_control.last_target == null' >/dev/null \
    || smoke_fail "bare anchor was not recorded honestly: $_tree"

# ------------------------------ 失败 2：anchor 合法但 wid 找不到 X11 窗口
_out="$(probe --move mapped 0 0 0)"
printf '%s' "$_out" | grep -q '^callback:1$' || smoke_fail "unknown wid should fail: $_out"
_tree="$(wctl_cmd get_tree)" || smoke_fail "get_tree failed"
printf '%s' "$_tree" | jq -e --arg out "$NOWID_OUTCOME" \
    '.xwindow_control.request_count == 2
     and .xwindow_control.success_count == 0
     and .xwindow_control.failure_count == 2
     and .xwindow_control.last_outcome == $out
     and .xwindow_control.last_target == null' >/dev/null \
    || smoke_fail "unknown wid was not recorded honestly: $_tree"
if [ "$XWAYLAND" = "true" ]; then
    printf '%s' "$_tree" | jq -e \
        '.xwindow_control.last_anchor | type == "array"' >/dev/null \
        || smoke_fail "mapped anchor position was not resolved: $_tree"
fi

# ------------------------------- 轨迹：两条失败都写了决策依据，不是空 ignore
_trace="$(wctl_cmd get_trace)" || smoke_fail "get_trace failed"
printf '%s' "$_trace" | jq -e --arg a "$BARE_OUTCOME" --arg b "$NOWID_OUTCOME" \
    '[.entries[] | select(.what == "xwindow")] | length == 2' >/dev/null \
    || smoke_fail "expected 2 xwindow trace entries: $_trace"
# 两个 length 判断各自括起来：jq 里 `|` 比 `and` 绑得紧，不括的话
# 右半段的输入会变成左半段产出的数组，`.entries` 就索引到数组上了。
printf '%s' "$_trace" | jq -e --arg a "$BARE_OUTCOME" --arg b "$NOWID_OUTCOME" \
    '([.entries[] | select(.what == "xwindow" and (.detail | contains($a)))] | length >= 1)
     and ([.entries[] | select(.what == "xwindow" and (.detail | contains($b)))] | length >= 1)' \
    >/dev/null || smoke_fail "failure reasons missing from trace: $_trace"

grep -q 'treeland-xwindow-control-v1' "$SMOKE_LOG" \
    || smoke_fail "compositor never advertised the xwindow-control global"

# ------------------------------------------------------------------ 成功路径
# 需要真 Xwayland + xcb 测试客户端；缺工具时只跑上面的协议级断言，并明确说明。
if [ "$XWAYLAND" != "true" ] || [ -z "$XOVERRIDE" ] || [ ! -x "$XOVERRIDE" ] \
        || ! command -v Xwayland >/dev/null 2>&1; then
    echo "xwindow-control smoke passed (protocol paths only; no Xwayland client available)"
    _check="$(wctl_cmd self_check)" || smoke_fail "self_check failed: $_check"
    printf '%s' "$_check" | jq -e '.ok == true' >/dev/null \
        || smoke_fail "self_check invariants broken: $_check"
    exit 0
fi

i=0
while ! grep -q 'DISPLAY=:[0-9]' "$SMOKE_LOG" 2>/dev/null && [ "$i" -lt 50 ]; do
    sleep 0.1; i=$((i+1))
done
DISPLAY_NAME="$(sed -n 's/.*DISPLAY=\(:[0-9][0-9]*\)$/\1/p' "$SMOKE_LOG" | head -1)"
[ -n "$DISPLAY_NAME" ] || smoke_fail "compositor did not publish DISPLAY"
export DISPLAY="$DISPLAY_NAME"

# 等某个 X11 窗口在场景里出现（buffer 160x120 = xoverride 的默认尺寸）
wait_xwindow_scene() {
    _i=0
    while [ "$_i" -lt 60 ]; do
        _scene="$(wctl_cmd dump_scene 2>/dev/null || true)"
        if printf '%s' "$_scene" | jq -e \
            '[.. | objects | select(.type? == "buffer" and .w? == 160 and .h? == 120)] | length > 0' \
            >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1; _i=$((_i+1))
    done
    return 1
}

parse_wid() {
    _log="$1"
    sed -n 's/.*window \(0x[0-9a-fA-F][0-9a-fA-F]*\) at .*/\1/p' "$_log" | head -1
}

# ---- override-redirect（Dock 的 XEmbed 托盘图标就是这种） ----
"$XOVERRIDE" 500 400 160 120 >"$WORK/xor.log" 2>&1 &
xor_pid=$!
x_pids="$x_pids $xor_pid"
i=0
while [ -z "$(parse_wid "$WORK/xor.log" 2>/dev/null)" ] && [ "$i" -lt 50 ]; do
    sleep 0.1; i=$((i+1))
done
OR_WID="$(parse_wid "$WORK/xor.log" 2>/dev/null)"
[ -n "$OR_WID" ] || smoke_fail "xoverride did not report its X11 window id"
wait_xwindow_scene || smoke_fail "override-redirect X11 window never appeared in the scene"

_out="$(probe --move mapped "$OR_WID" 40 25)"
printf '%s' "$_out" | grep -q '^callback:0$' \
    || smoke_fail "unmanaged move should succeed: $_out"
_tree="$(wctl_cmd get_tree)" || smoke_fail "get_tree failed"
printf '%s' "$_tree" | jq -e \
    '.xwindow_control.last_outcome == "moved"
     and .xwindow_control.last_target_kind == "unmanaged"
     and .xwindow_control.last_view_id == 0
     and .xwindow_control.success_count == 1
     and .xwindow_control.request_count == 3
     and (.xwindow_control.last_anchor | type == "array")
     and .xwindow_control.last_target
         == [(.xwindow_control.last_anchor[0] + 40),
             (.xwindow_control.last_anchor[1] + 25)]' >/dev/null \
    || smoke_fail "unmanaged move not reflected in wctl: $_tree"

# 独立核对场景：窗口的 scene tree 真的被挪到了目标坐标（不是只记了个数）
TX="$(printf '%s' "$_tree" | jq -r '.xwindow_control.last_target[0]')"
TY="$(printf '%s' "$_tree" | jq -r '.xwindow_control.last_target[1]')"
_scene="$(wctl_cmd dump_scene)" || smoke_fail "dump_scene failed"
printf '%s' "$_scene" | jq -e --argjson tx "$TX" --argjson ty "$TY" \
    '[.. | objects | select(.type? == "tree" and .x? == $tx and .y? == $ty)] | length > 0' \
    >/dev/null || smoke_fail "no scene tree at target ($TX,$TY): $_scene"

# ---- 受管理的 X11 顶层窗口（走 woodland_view + X11 configure 那条分支） ----
"$XOVERRIDE" --managed-window >"$WORK/xmanaged.log" 2>&1 &
managed_pid=$!
x_pids="$x_pids $managed_pid"
i=0
while [ -z "$(parse_wid "$WORK/xmanaged.log" 2>/dev/null)" ] && [ "$i" -lt 50 ]; do
    sleep 0.1; i=$((i+1))
done
MANAGED_WID="$(parse_wid "$WORK/xmanaged.log" 2>/dev/null)"
[ -n "$MANAGED_WID" ] || smoke_fail "managed X11 client did not report its window id"
i=0
while [ "$i" -lt 60 ]; do
    _tree2="$(wctl_cmd get_tree)"
    printf '%s' "$_tree2" | jq -e '[.views[] | select(.kind == "x11" and .mapped)] | length > 0' \
        >/dev/null 2>&1 && break
    sleep 0.1; i=$((i+1))
done

_out="$(probe --move mapped "$MANAGED_WID" 300 100)"
printf '%s' "$_out" | grep -q '^callback:0$' \
    || smoke_fail "managed move should succeed: $_out"
_tree="$(wctl_cmd get_tree)" || smoke_fail "get_tree failed"
printf '%s' "$_tree" | jq -e \
    '.xwindow_control.last_outcome == "moved"
     and .xwindow_control.last_target_kind == "managed"
     and .xwindow_control.last_view_id > 0
     and .xwindow_control.success_count == 2
     and .xwindow_control.request_count == 4' >/dev/null \
    || smoke_fail "managed move not reflected in wctl: $_tree"

# content 的布局坐标由视图的 scene_tree 原点算出，是独立于协议记账的核对
VID="$(printf '%s' "$_tree" | jq -r '.xwindow_control.last_view_id')"
TX="$(printf '%s' "$_tree" | jq -r '.xwindow_control.last_target[0]')"
TY="$(printf '%s' "$_tree" | jq -r '.xwindow_control.last_target[1]')"
printf '%s' "$_tree" | jq -e --argjson vid "$VID" --argjson tx "$TX" --argjson ty "$TY" \
    '[.views[] | select(.id == $vid) | (.content[0] == $tx and .content[1] == $ty)]
     | any' >/dev/null \
    || smoke_fail "managed view content did not move to ($TX,$TY): $_tree"

_trace="$(wctl_cmd get_trace)" || smoke_fail "get_trace failed"
printf '%s' "$_trace" | jq -e \
    '[.entries[] | select(.what == "xwindow" and (.detail | contains("-> moved")))] | length == 2' \
    >/dev/null || smoke_fail "successful moves missing from trace: $_trace"

_check="$(wctl_cmd self_check)" || smoke_fail "self_check failed: $_check"
printf '%s' "$_check" | jq -e '.ok == true' >/dev/null \
    || smoke_fail "self_check invariants broken: $_check"

echo "xwindow-control smoke passed (bind/destroy/failures + unmanaged/managed moves)"
