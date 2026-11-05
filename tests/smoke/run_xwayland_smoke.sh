#!/usr/bin/env sh
# Xwayland 基础支持冒烟：
#   headless woodland → 拉起 Xwayland → xterm（managed）与 xoverride（override-redirect）
#
# 断言：
#   1) xterm 出现在 ext-foreign-toplevel-list（说明建了正常的 view、进了窗口列表）
#   2) 截图顶部 26px 有 SSD 标题栏底色（说明服务端装饰生效）
#   3) override-redirect 窗口不产生 toplevel（unmanaged 路径），但像素上可见（红）
#
# 用法: sh run_xwayland_smoke.sh <woodland> <xoverride>
set -eu

. "$(dirname "$0")/lib.sh"
smoke_init xwayland

BIN="${1:-./build/src/woodland}"
XOVERRIDE="${2:-./build/tests/xoverride}"
[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 125; }

for tool in Xwayland grim lswt convert identify file; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "$tool not found, skip xwayland smoke" >&2
        exit 77
    }
done
[ -x "$XOVERRIDE" ] || { echo "xoverride not built, skip" >&2; exit 77; }

# 终端窗口最接近真实使用场景，但它要分配 pty；受限沙箱里没有 /dev/ptmx
# 时退到 xlogo —— 同样是普通 X11 顶层窗口，只是不依赖 pty。
if [ -e /dev/ptmx ] && command -v xterm >/dev/null 2>&1; then
    XAPP=xterm
    XAPP_ARGS="-geometry 60x12"
else
    command -v xlogo >/dev/null 2>&1 || {
        echo "neither xterm nor xlogo available, skip" >&2; exit 77; }
    XAPP=xlogo
    XAPP_ARGS="-geometry 300x200"
fi

unset WAYLAND_DISPLAY
export XDG_RUNTIME_DIR="$(mktemp -d)"
WORK="$(mktemp -d)"

comp_pid=""
client_pids=""

smoke_cleanup() {
    for p in $client_pids; do
        kill -TERM "$p" 2>/dev/null || true
    done
    if [ -n "$comp_pid" ]; then
        kill -TERM "$comp_pid" 2>/dev/null || true
        i=0
        while kill -0 "$comp_pid" 2>/dev/null && [ "$i" -lt 50 ]; do
            sleep 0.1; i=$((i+1))
        done
        kill -KILL "$comp_pid" 2>/dev/null || true
        wait "$comp_pid" 2>/dev/null || true
    fi
    rm -rf "$XDG_RUNTIME_DIR" "$WORK"
}

SMOKE_LOG="$WORK/comp.log"
WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 \
    "$BIN" >"$SMOKE_LOG" 2>&1 &
comp_pid=$!

i=0
while [ "$(ls "$XDG_RUNTIME_DIR/" 2>/dev/null | grep -c '^wayland-')" -eq 0 ] \
        && [ "$i" -lt 50 ]; do
    sleep 0.1; i=$((i+1))
done
socket_name="$(ls "$XDG_RUNTIME_DIR/" 2>/dev/null | grep '^wayland-' | head -1)"
[ -n "$socket_name" ] || { echo "wayland socket did not appear" >&2; exit 1; }
export WAYLAND_DISPLAY="$socket_name"

# DISPLAY 由合成器在 start 时写进日志（Xwayland lazy 模式，名字在 init 时就定了）
i=0
while ! grep -q 'DISPLAY=:[0-9]' "$WORK/comp.log" 2>/dev/null && [ "$i" -lt 50 ]; do
    sleep 0.1; i=$((i+1))
done
DISPLAY_NAME="$(sed -n 's/.*DISPLAY=\(:[0-9][0-9]*\)$/\1/p' "$WORK/comp.log" | head -1)"
[ -n "$DISPLAY_NAME" ] || {
    echo "compositor did not publish DISPLAY:" >&2
    cat "$WORK/comp.log" >&2
    exit 1
}
export DISPLAY="$DISPLAY_NAME"

# ---- 1) managed 的 X11 顶层窗口 ----
# shellcheck disable=SC2086
"$XAPP" $XAPP_ARGS >"$WORK/xapp.log" 2>&1 &
client_pids="$client_pids $!"

toplevels=""
i=0
while [ "$i" -lt 80 ]; do
    toplevels="$(lswt -j 2>/dev/null || true)"
    echo "$toplevels" | grep -q "\"$XAPP\"" && break
    sleep 0.1; i=$((i+1))
done
echo "$toplevels" | grep -q "\"$XAPP\"" || {
    echo "$XAPP toplevel not listed by lswt:" >&2
    echo "$toplevels" >&2
    echo "--- client log ---" >&2
    cat "$WORK/xapp.log" >&2 || true
    exit 1
}
count_before="$(echo "$toplevels" | grep -c '"app_id"' || true)"

# ---- 2) SSD 标题栏底色 ----
sleep 0.5
grim "$WORK/managed.png"

color_count() {
    # $1=png $2=crop(WxH+X+Y) $3=十六进制颜色(#RRGGBB) → 该颜色像素数
    # 用 hex 而不是十进制三元组：纯色在 histogram 里显示为 "#FF0000 red"，
    # 没有 srgb(...) 后缀，只有非纯色才有。
    convert "$1" -crop "$2" +repage -format %c histogram:info:- 2>/dev/null \
        | sed -n "s/^ *\([0-9][0-9]*\):.*$3.*/\1/p" | head -1
}

color_count_near() {
    # $1=png $2=crop $3=#RRGGBB $4=容差(如 1%) → 落在该颜色 ±容差 内的像素数
    #
    # 底色断言不能要求逐位相等：同一份像素走 wlroots 自带 renderer 和走
    # scenefx 的 FX renderer，在 sRGB 换算的舍入上会差一个最低位
    # （0.965*255 一边得 247、一边得 246）。差一位不代表画错了，但"是不是
    # 变成了别的底色"必须还能分辨，所以用 -fuzz 把目标色染成洋红再数。
    convert "$1" -crop "$2" +repage -fuzz "$4" -fill "#FF00FF" -opaque "$3" \
        -format %c histogram:info:- 2>/dev/null \
        | sed -n 's/^ *\([0-9][0-9]*\):.*#FF00FF.*/\1/p' | head -1
}

TITLEBAR="#E1DEDB"       # labwc 激活态标题栏底色
TITLEBAR_DIM="#F7F6F4"   # 非激活态（0.965*255 约为 246~247，按 renderer 差一位）
n="$(color_count_near "$WORK/managed.png" "400x26+0+0" "$TITLEBAR" 1%)"
[ -n "${n:-}" ] && [ "$n" -gt 1000 ] || {
    echo "no SSD titlebar pixels at top of the X11 window (got '${n:-0}')" >&2
    convert "$WORK/managed.png" -crop 400x26+0+0 +repage "$WORK/crop.png" || true
    exit 1
}

# ---- 3) override-redirect：不产生 toplevel，但要在画面上 ----
"$XOVERRIDE" 500 400 160 120 >"$WORK/xoverride.log" 2>&1 &
client_pids="$client_pids $!"

# 轮询：Xwayland 用软件渲染，窗口上屏有几帧延迟
n=0
i=0
while [ "$i" -lt 30 ]; do
    sleep 0.2
    grim "$WORK/popup.png" 2>/dev/null || true
    n="$(color_count "$WORK/popup.png" "160x120+500+400" "#FF0000")"
    [ -n "${n:-}" ] && [ "$n" -gt 5000 ] && break
    i=$((i+1))
done
[ -n "${n:-}" ] && [ "$n" -gt 5000 ] || {
    echo "override-redirect window not visible (red pixels: '${n:-0}')" >&2
    convert "$WORK/popup.png" -crop 200x160+480+380 +repage \
        -format %c histogram:info:- 2>/dev/null | head -5 >&2 || true
    exit 1
}

toplevels_after="$(lswt -j 2>/dev/null || true)"
count_after="$(echo "$toplevels_after" | grep -c '"app_id"' || true)"
[ "$count_before" = "$count_after" ] || {
    echo "override-redirect window leaked into toplevel list" \
         "($count_before -> $count_after)" >&2
    echo "$toplevels_after" >&2
    exit 1
}

# ---- 4) 焦点唯一 + 关闭后焦点回退 ----
# 同一个工作区里只能有一个窗口是"激活态"：第二个窗口拿到焦点后，
# 第一个的标题栏必须变暗；关掉焦点窗口后，剩下的窗口要重新变亮。
"$XAPP" $XAPP_ARGS >"$WORK/xapp2.log" 2>&1 &
second_pid=$!
client_pids="$client_pids $second_pid"
sleep 1.2
grim "$WORK/focus-two.png"
dim="$(identify -format '%wx%h' "$WORK/focus-two.png")"
inactive_px="$(color_count_near "$WORK/focus-two.png" "$dim+0+0" "$TITLEBAR_DIM" 1%)"
[ -n "${inactive_px:-}" ] && [ "$inactive_px" -gt 500 ] || {
    echo "no inactive titlebar: focus is not exclusive (got '${inactive_px:-0}')" >&2
    exit 1
}

kill -TERM "$second_pid" 2>/dev/null || true
sleep 1.2
grim "$WORK/focus-after.png"
active_px="$(color_count_near "$WORK/focus-after.png" "$dim+0+0" "$TITLEBAR" 1%)"
[ -n "${active_px:-}" ] && [ "$active_px" -gt 500 ] || {
    echo "focus did not fall back after closing the focused window" \
         "(active pixels: '${active_px:-0}')" >&2
    exit 1
}

echo "xwayland smoke OK: $XAPP toplevel + SSD titlebar + override-redirect" \
     "+ focus fallback ($DISPLAY_NAME)"
