#!/usr/bin/env sh
# treeland-appearance / -manager 冒烟：
#   bind 即推送 11 项当前值、set_* 真改状态并广播给活着的观测端、
#   越界值 clamp、枚举外值静默忽略、只记录的设置不假装生效，
#   以及标题栏高度真的改到了已映射窗口的 SSD 边距上。
#
# 用法: sh run_appearance_smoke.sh <woodland> <appearance_probe> <wctl> [deco_probe]
#
# deco_probe 可选：给了就多验一条"设置真的到了渲染路径"（SSD 边距）；
# 没给就跳过那一条，其余断言照跑。
set -eu

. "$(dirname "$0")/lib.sh"
smoke_init appearance

BIN="${1:-./build/src/woodland}"
PROBE="${2:-./build/tests/appearance_probe}"
WCTL="${3:-}"
DECO="${4:-}"
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
deco_pid=""
smoke_cleanup() {
    [ -n "$deco_pid" ] && kill -TERM "$deco_pid" 2>/dev/null || true
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
# 圆角有没有渲染后端：与 deco smoke 同一口径（get_tree 顶层 effects 字段，
# 编译期开关不参与时恒为 false），脚本据此分叉断言。
EFFECTS="$("$WCTL" -s "$SOCK" get_tree | jq -r '.effects')"

wctl_cmd() {
    "$WCTL" -s "$SOCK" -t 1000 "$1"
}

# 发一条 set_*；探针自己会断言连接没被协议错误杀掉
set_req() {
    set +e
    "$PROBE" --set "$@" >/dev/null
    _rc=$?
    set -e
    [ "$_rc" -eq 0 ] || smoke_fail "probe --set $* exited with $_rc"
}

# 查 appearance 段里的一个字段
expect() {
    _filter="$1" _what="$2"
    _tree="$(wctl_cmd get_tree)" || smoke_fail "get_tree failed"
    printf '%s' "$_tree" | jq -e "$_filter" >/dev/null \
        || smoke_fail "unexpected appearance state ($_what): $(printf '%s' "$_tree" | jq -c '.appearance')"
}

OBS_INIT="$XDG_RUNTIME_DIR/obs_init.txt"
OBS_LIVE="$XDG_RUNTIME_DIR/obs_live.txt"

# ---------- 1. bind 即推送：11 条事件，值来自内置默认 ----------
"$PROBE" --observe 300 >"$OBS_INIT"
grep -qx 'ready' "$OBS_INIT" || smoke_fail "observer never became ready"
# 事件条数：11 项设置各一条（多一条少一条都说明协议实现漂了）
_events="$(grep -c -E '^(cursor_theme|cursor_size|font|monospace_font|font_size|icon_theme|accent_color|window_opacity|color_scheme|titlebar_height|corner_radius) ' "$OBS_INIT")"
[ "$_events" -eq 11 ] || smoke_fail "expected 11 initial events, got $_events: $(cat "$OBS_INIT")"
grep -q '^cursor_theme default$' "$OBS_INIT" || smoke_fail "initial cursor theme wrong: $(cat "$OBS_INIT")"
grep -q '^cursor_size 24 24$' "$OBS_INIT" || smoke_fail "initial cursor size wrong: $(cat "$OBS_INIT")"
grep -qi '^font sans$' "$OBS_INIT" || smoke_fail "initial font wrong: $(cat "$OBS_INIT")"
grep -q '^font_size 100$' "$OBS_INIT" || smoke_fail "initial font size (10pt*10) wrong: $(cat "$OBS_INIT")"
grep -q '^color_scheme 0$' "$OBS_INIT" || smoke_fail "initial scheme should be light: $(cat "$OBS_INIT")"
grep -q '^titlebar_height 26$' "$OBS_INIT" || smoke_fail "initial titlebar height wrong: $(cat "$OBS_INIT")"
grep -q '^corner_radius 0$' "$OBS_INIT" || smoke_fail "initial corner radius wrong: $(cat "$OBS_INIT")"
grep -q '^window_opacity 1.000$' "$OBS_INIT" || smoke_fail "initial opacity wrong: $(cat "$OBS_INIT")"

# wctl 侧同样是"已就绪但没有请求"的样子
expect '.appearance.available == true
        and .appearance.request_count == 0
        and .appearance.last_outcome == "none"' "initial"

# ---------- 2. 设置生效 + 广播给活着的观测端 ----------
"$PROBE" --observe 1200 >"$OBS_LIVE" &
obs_pid=$!
# 等观测端 bind 完成（它打出 ready 就说明初始推送已经收全）
# 注意别写成 `[ "$(grep -c … || echo 0)" … ]`：文件还不存在时 grep -c 既
# 输出 0 又返回 1，`||` 会再追加一个 0，拼出 "0\n0"，[ ] 直接报"需要整数"。
i=0
while [ "$i" -lt 50 ]; do
    if grep -qx 'ready' "$OBS_LIVE" 2>/dev/null; then
        break
    fi
    sleep 0.1
    i=$((i+1))
done
grep -qx 'ready' "$OBS_LIVE" || smoke_fail "live observer never became ready"

set_req set_cursor_size 32 32
set_req set_titlebar_height 30
set_req set_color_scheme 1
wait "$obs_pid" || smoke_fail "live observer exited non-zero"

grep -q '^cursor_size 32 32$' "$OBS_LIVE" || smoke_fail "cursor_size broadcast missing: $(cat "$OBS_LIVE")"
grep -q '^titlebar_height 30$' "$OBS_LIVE" || smoke_fail "titlebar_height broadcast missing: $(cat "$OBS_LIVE")"
grep -q '^color_scheme 1$' "$OBS_LIVE" || smoke_fail "color_scheme broadcast missing: $(cat "$OBS_LIVE")"

expect '.appearance.cursor_width == 32 and .appearance.cursor_height == 32' "cursor size"
expect '.appearance.titlebar_height == 30' "titlebar height"
expect '.appearance.color_scheme == "dark" and .appearance.color_scheme_set == true' "scheme"

# ---------- 3. 越界 clamp（协议要求收进支持范围，不报错） ----------
set_req set_cursor_size 0 0
expect '.appearance.cursor_width == 8 and .appearance.cursor_height == 8
        and .appearance.last_request == "set_cursor_size"
        and .appearance.last_outcome == "clamped"' "cursor clamp"

# 非方形请求降级到 square：wlr_xcursor_manager 尺寸是单值，广播的必须是实际
# 生效值（32x32），不能把被降级的 height 原样报回去。
set_req set_cursor_size 32 48
expect '.appearance.cursor_width == 32 and .appearance.cursor_height == 32
        and .appearance.last_outcome == "clamped"' "non-square cursor degraded to square"

set_req set_titlebar_height 999
expect '.appearance.titlebar_height == 200 and .appearance.last_outcome == "clamped"' "titlebar clamp"

set_req set_corner_radius 12
if [ "$EFFECTS" = "true" ]; then
    expect '.appearance.corner_radius == 12 and .appearance.last_outcome == "applied"' "radius"
else
    # 没有渲染后端：值照记、广播报 0、outcome 如实报 recorded（不假装上屏）
    expect '.appearance.corner_radius == 0 and .appearance.last_outcome == "recorded"' "radius recorded (no backend)"
fi
set_req set_corner_radius 99999
if [ "$EFFECTS" = "true" ]; then
    expect '.appearance.corner_radius == 1000 and .appearance.last_outcome == "clamped"' "radius clamp"
else
    expect '.appearance.corner_radius == 0 and .appearance.last_outcome == "recorded"' "radius recorded (no backend)"
fi

set_req set_accent_color 300 0 0
expect '.appearance.accent == [255,0,0] and .appearance.accent_set == true
        and .appearance.last_outcome == "clamped"' "accent clamp"

# ---------- 4. 字体：族名与字号各自独立改，互不覆盖 ----------
set_req set_font "Noto Sans"
expect '.appearance.font_family == "Noto Sans"
        and .appearance.font_size_pt10 == 100' "font family"
set_req set_font_size 125
expect '.appearance.font_family == "Noto Sans"
        and .appearance.font_size_pt10 == 125' "font size"
set_req set_font_size 0
expect '.appearance.font_size_pt10 == 50 and .appearance.last_outcome == "clamped"' "font size clamp"

# ---------- 5. 枚举外的配色方案：静默忽略，当前值不变 ----------
_before="$(wctl_cmd get_tree | jq -r '.appearance.color_scheme')"
set_req set_color_scheme 7
_tree="$(wctl_cmd get_tree)" || smoke_fail "get_tree failed"
printf '%s' "$_tree" | jq -e --arg want "$_before" \
    '.appearance.color_scheme == $want
     and .appearance.last_outcome == "invalid"
     and .appearance.invalid_count == 1' >/dev/null \
    || smoke_fail "invalid color scheme changed state: $(printf '%s' "$_tree" | jq -c '.appearance')"

# ---------- 6. 只记录的两项：状态可见、事件照发，但明写不渲染 ----------
set_req set_window_opacity 0.5
expect '.appearance.window_opacity_milli == 500
        and .appearance.window_opacity_set == true
        and .appearance.last_outcome == "recorded"' "opacity"
set_req set_monospace_font "DejaVu Sans Mono"
expect '.appearance.monospace_font == "DejaVu Sans Mono"
        and .appearance.last_outcome == "recorded"' "monospace"

expect '.appearance.recorded_only == ["set_monospace_font","set_window_opacity"]' "recorded_only"

# ---------- 7. 图标主题 ----------
set_req set_icon_theme "Papirus"
expect '.appearance.icon_theme == "Papirus"' "icon theme"

# ---------- 8. 真的到了渲染路径：已映射窗口的 SSD 边距跟着变 ----------
if [ -n "$DECO" ] && [ -x "$DECO" ]; then
    "$DECO" --hold 4000 >"$XDG_RUNTIME_DIR/deco.log" 2>&1 &
    deco_pid=$!
    i=0
    while ! wctl_cmd get_tree | jq -e '.count >= 1' >/dev/null 2>&1; do
        [ "$i" -lt 40 ] || break
        sleep 0.1; i=$((i+1))
    done
    _tree="$(wctl_cmd get_tree)" || smoke_fail "get_tree failed after map"
    printf '%s' "$_tree" | jq -e '.count >= 1 and .views[0].ssd.present == true' >/dev/null \
        || smoke_fail "no mapped window with SSD: $_tree"

    # 标题栏 30 + 边框 1 = 内容上方 31px
    # margins 是数组且顺序为 [left, top, right, bottom]（见 src/ipc/commands.c）
    set_req set_titlebar_height 30
    _tree="$(wctl_cmd get_tree)" || smoke_fail "get_tree failed"
    printf '%s' "$_tree" | jq -e '.views[0].ssd.margins[1] == 31' >/dev/null \
        || smoke_fail "titlebar height did not reach the SSD margins: $(printf '%s' "$_tree" | jq -c '.views[0].ssd')"

    # 0 = 隐藏标题栏：边距只剩边框
    set_req set_titlebar_height 0
    _tree="$(wctl_cmd get_tree)" || smoke_fail "get_tree failed"
    printf '%s' "$_tree" | jq -e '.views[0].ssd.margins[1] == 1' >/dev/null \
        || smoke_fail "titlebar_height 0 did not hide the titlebar: $(printf '%s' "$_tree" | jq -c '.views[0].ssd')"
    kill -TERM "$deco_pid" 2>/dev/null || true
    wait "$deco_pid" 2>/dev/null || true
    deco_pid=""
else
    echo "note: deco_probe not provided, skipping the SSD margin assertion"
fi

# ---------- 9. 决策轨迹与不变量 ----------
_trace="$(wctl_cmd get_trace)" || smoke_fail "get_trace failed"
_applied="$(printf '%s' "$_trace" | jq '[.entries[] | select(.what == "appearance")] | length')"
[ "$_applied" -ge 10 ] || smoke_fail "expected >=10 appearance trace entries, got $_applied"
printf '%s' "$_trace" | jq -e \
    '[.entries[] | select(.what == "appearance" and (.detail | contains("no per-window alpha path")))] | length == 1' \
    >/dev/null || smoke_fail "recorded-only opacity not explained in trace: $_trace"
printf '%s' "$_trace" | jq -e \
    '[.entries[] | select(.what == "appearance" and (.detail | contains("no monospace consumer")))] | length == 1' \
    >/dev/null || smoke_fail "recorded-only monospace font not explained in trace: $_trace"

_check="$(wctl_cmd self_check)" || smoke_fail "self_check failed: $_check"
printf '%s' "$_check" | jq -e '.ok == true' >/dev/null \
    || smoke_fail "self_check invariants broken: $_check"

grep -q 'treeland-appearance-v1 and -manager-v1 enabled' "$SMOKE_LOG" \
    || smoke_fail "compositor never advertised the appearance globals"
grep -q 'recorded (no per-window alpha path)' "$SMOKE_LOG" \
    || smoke_fail "compositor did not log the recorded-only opacity"

echo "appearance smoke passed"
