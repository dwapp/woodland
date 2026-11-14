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

# 探针的参数解析自检。--dry-run 不连 X，所以在"X 连不上"的机器上也会执行——
# 这条路径以前是坏的：开关模式用 atoi(argv[1]) 取 x，拿到的是 "--managed-window"
# 的 0，窗口恒贴屏幕左边缘，再往后传几何还会整体错位一格。
if [ -n "$XOVERRIDE" ] && [ -x "$XOVERRIDE" ]; then
    dry="$("$XOVERRIDE" --dry-run --managed-window 10 20 300 200)" || {
        echo "xoverride --dry-run failed" >&2
        exit 1
    }
    case "$dry" in
    *"mode=managed x=10 y=20 w=300 h=200"*) ;;
    *)
        echo "xoverride parses --managed-window geometry wrong: $dry" >&2
        exit 1
        ;;
    esac
fi

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
    '.xwindow_control.request_count == 0
     and .xwindow_control.success_count == 0
     and .xwindow_control.failure_count == 0
     and .xwindow_control.last_wid == 0
     and .xwindow_control.last_outcome == "none"
     and .xwindow_control.last_anchor == null
     and .xwindow_control.last_target == null' >/dev/null \
    || smoke_fail "protocol state was not exposed: $_tree"

AVAILABLE="$(printf '%s' "$_tree" | jq -r '.xwindow_control.available')"
XWAYLAND="$(printf '%s' "$_tree" | jq -r '.xwindow_control.xwayland')"

# 编译期关闭 XWayland 时该 global 不该广播（与上游 DISABLE_XWAYLAND 一致）：
# DDE Dock 用 isActive() 判通道可用性，广播一个永远回 done(1) 的 global 会
# 让它一直发请求、不再回退到 X11 原生 reparent 路径。
if [ "$AVAILABLE" != "true" ]; then
    printf '%s' "$_tree" | jq -e \
        '.xwindow_control.available == false
         and .xwindow_control.xwayland == false
         and .xwindow_control.request_count == 0
         and .xwindow_control.last_outcome == "none"' >/dev/null \
        || smoke_fail "noxwl protocol state unexpected: $_tree"
    _out="$(probe --no-global)"
    printf '%s' "$_out" | grep -q '^no global ok$' \
        || smoke_fail "noxwl build must not advertise xwindow-control: $_out"
    grep -q 'global not advertised' "$SMOKE_LOG" \
        || smoke_fail "noxwl build did not log the skipped global"
    _check="$(wctl_cmd self_check)" || smoke_fail "self_check failed: $_check"
    printf '%s' "$_check" | jq -e '.ok == true' >/dev/null \
        || smoke_fail "self_check invariants broken: $_check"
    echo "xwindow-control smoke passed (noxwl: global absent, Dock can fall back)"
    exit 0
fi

# 运行期 Xwayland 层没建起来（编译开启但 lazy 未起 / 启动失败）时，请求一律
# 回 no_xwayland；global 仍在，属可诊断的降级。
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

# 客户端没报出 wid 时，先把探针自己的输出打出来。
# 最常见的失败根本不是"窗口没报号"，而是 X 连接没建成——比如这台机器上还跑着
# 别的 X server（宿主的会话），同一个 display 号被它占着，客户端拿着对不上号的
# cookie 去连，只得到一句 "cannot connect to X display"。只报
# "did not report its X11 window id" 会把人往合成器那边引，实际跟它无关。
fail_no_wid() {
    echo "--- $1 log (DISPLAY=$DISPLAY):" >&2
    cat "$2" >&2 2>/dev/null || :
    smoke_fail "$1 did not report its X11 window id"
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
[ -n "$OR_WID" ] || fail_no_wid xoverride "$WORK/xor.log"
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
[ -n "$MANAGED_WID" ] || fail_no_wid "managed client" "$WORK/xmanaged.log"
i=0
_treed=""
while [ "$i" -lt 60 ]; do
    _treed="$(wctl_cmd get_tree)"
    printf '%s' "$_treed" | jq -e '[.views[] | select(.kind == "x11" and .mapped)] | length > 0' \
        >/dev/null 2>&1 && break
    sleep 0.1; i=$((i+1))
done
# 循环只 break 不断言的话，窗口压根没起来时会一路走到下面的 --move，最后报成
# "no_window"——那是结果不是原因。在这里说清"窗口没出现"。
[ "$i" -lt 60 ] || smoke_fail "managed X11 view never appeared in get_tree: $_treed"

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

# ---- 小数偏移：原始 24.8 值进 wctl，取整策略可独立核对 ----
# DDE Dock 用 wl_fixed_from_double 传 QML 布局坐标，经常带小数。
_out="$(probe --move mapped "$MANAGED_WID" 12.25 -3.75)"
printf '%s' "$_out" | grep -q '^callback:0$' \
    || smoke_fail "fractional offset should succeed: $_out"
_tree="$(wctl_cmd get_tree)" || smoke_fail "get_tree failed"
printf '%s' "$_tree" | jq -e \
    '.xwindow_control.last_outcome == "moved"
     and .xwindow_control.last_dx == 12
     and .xwindow_control.last_dy == -4
     and .xwindow_control.last_dx_fixed == 3136
     and .xwindow_control.last_dy_fixed == -960
     and .xwindow_control.success_count == 3
     and .xwindow_control.request_count == 5
     and .xwindow_control.last_target
         == [(.xwindow_control.last_anchor[0] + 12),
             (.xwindow_control.last_anchor[1] - 4)]' >/dev/null \
    || smoke_fail "fractional offset not recorded honestly: $_tree"
TX="$(printf '%s' "$_tree" | jq -r '.xwindow_control.last_target[0]')"
TY="$(printf '%s' "$_tree" | jq -r '.xwindow_control.last_target[1]')"
printf '%s' "$_tree" | jq -e --argjson vid "$VID" --argjson tx "$TX" --argjson ty "$TY" \
    '[.views[] | select(.id == $vid) | (.content[0] == $tx and .content[1] == $ty)]
     | any' >/dev/null \
    || smoke_fail "fractional offset did not move the view to ($TX,$TY): $_tree"

# ---- 超出 X11 int16 坐标范围：回 done(1)，且窗口一动不动 ----
# 40000 是协议值域内的合法 fixed 值，以前会被 (int16_t) 截断成 -25536，
# 回 done(0) 之后 X server 的 ConfigureNotify 又会把 scene 节点拉回去。
_out="$(probe --move mapped "$MANAGED_WID" 40000 0)"
printf '%s' "$_out" | grep -q '^callback:1$' \
    || smoke_fail "out-of-range target should fail: $_out"
_tree="$(wctl_cmd get_tree)" || smoke_fail "get_tree failed"
printf '%s' "$_tree" | jq -e --argjson vid "$VID" --argjson tx "$TX" --argjson ty "$TY" \
    '.xwindow_control.last_outcome == "out_of_range"
     and .xwindow_control.last_target == null
     and .xwindow_control.success_count == 3
     and .xwindow_control.failure_count == 3
     and .xwindow_control.request_count == 6
     and ([.views[] | select(.id == $vid)
           | (.content[0] == $tx and .content[1] == $ty)] | any)' >/dev/null \
    || smoke_fail "out-of-range target moved or was misreported: $_tree"
_scene="$(wctl_cmd dump_scene)" || smoke_fail "dump_scene failed"
printf '%s' "$_scene" | jq -e \
    '[.. | objects | select(.type? == "tree" and (.x? > 32767))] | length == 0' \
    >/dev/null || smoke_fail "a scene tree was placed beyond the X11 int16 range: $_scene"

_trace="$(wctl_cmd get_trace)" || smoke_fail "get_trace failed"
printf '%s' "$_trace" | jq -e \
    '[.entries[] | select(.what == "xwindow" and (.detail | contains("-> moved")))] | length == 3' \
    >/dev/null || smoke_fail "successful moves missing from trace: $_trace"
printf '%s' "$_trace" | jq -e \
    '[.entries[] | select(.what == "xwindow" and (.detail | contains("-> out_of_range")))] | length == 1' \
    >/dev/null || smoke_fail "out-of-range rejection missing from trace: $_trace"

_check="$(wctl_cmd self_check)" || smoke_fail "self_check failed: $_check"
printf '%s' "$_check" | jq -e '.ok == true' >/dev/null \
    || smoke_fail "self_check invariants broken: $_check"

echo "xwindow-control smoke passed (bind/destroy/failures + unmanaged/managed/fractional moves + range rejection)"
