#!/usr/bin/env sh
# 端到端验证截图链路：headless woodland + weston-simple-shm，
# 用 lswt 读窗口列表（ext-foreign-toplevel-list-v1），
# 用 grim 抓整屏（zwlr_screencopy_v1 / ext-image-copy-capture-v1）、
# 区域、以及按 identifier 抓单个窗口（ext-foreign-toplevel-image-capture-source-v1）。
#
# 用法: sh run_capture_smoke.sh <woodland 二进制路径>
set -eu

. "$(dirname "$0")/lib.sh"
smoke_init capture

BIN="${1:-./build/src/woodland}"
[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 125; }

for tool in grim lswt weston-simple-shm file; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "$tool not found, skip capture smoke" >&2
        exit 77
    }
done

unset WAYLAND_DISPLAY
export XDG_RUNTIME_DIR="$(mktemp -d)"
WORK="$(mktemp -d)"

comp_pid=""
client_pid=""

smoke_cleanup() {
    [ -n "$client_pid" ] && kill -TERM "$client_pid" 2>/dev/null || true
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

SMOKE_LOG="$XDG_RUNTIME_DIR/woodland.log"
WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 "$BIN" \
    >"$SMOKE_LOG" 2>&1 &
comp_pid=$!

i=0
while [ "$(ls "$XDG_RUNTIME_DIR/" 2>/dev/null | grep -c '^wayland-')" -eq 0 ] && [ "$i" -lt 50 ]; do
    sleep 0.1; i=$((i+1))
done
socket_name="$(ls "$XDG_RUNTIME_DIR/" 2>/dev/null | grep '^wayland-' | head -1)"
if [ -z "$socket_name" ]; then
    echo "wayland socket did not appear" >&2
    exit 1
fi
export WAYLAND_DISPLAY="$socket_name"

# 一个 250x250 的客户端窗口，作为截图目标
weston-simple-shm >"$WORK/client.log" 2>&1 &
client_pid=$!

# 协议是异步的：轮询 lswt 直到窗口出现在列表里
toplevels=""
i=0
while [ "$i" -lt 50 ]; do
    toplevels="$(lswt -j 2>/dev/null || true)"
    echo "$toplevels" | grep -q 'simple-shm' && break
    sleep 0.1; i=$((i+1))
done
echo "$toplevels" | grep -q 'org.freedesktop.weston.simple-shm' || {
    echo "lswt did not list the weston-simple-shm toplevel:" >&2
    echo "$toplevels" >&2
    exit 1
}

png_size() {
    file "$1" | sed -n 's/.*PNG image data, \([0-9][0-9]*\) x \([0-9][0-9]*\),.*/\1 \2/p'
}

# 1) 整屏截图（grim 默认走输出捕获）
grim "$WORK/full.png"
size="$(png_size "$WORK/full.png")"
[ "$size" = "1280 720" ] || {
    echo "full capture size unexpected: '$size' (want 1280 720)" >&2
    exit 1
}

# 2) 区域截图
grim -g "0,0 200x150" "$WORK/region.png"
size="$(png_size "$WORK/region.png")"
[ "$size" = "200 150" ] || {
    echo "region capture size unexpected: '$size' (want 200 150)" >&2
    exit 1
}

# 3) 按 toplevel identifier 截图
identifier="$(echo "$toplevels" \
    | sed -n 's/.*"identifier": "\([^"]*\)".*/\1/p' | head -1)"
if [ -z "$identifier" ]; then
    echo "no toplevel identifier in lswt output" >&2
    exit 1
fi
grim -T "$identifier" "$WORK/toplevel.png"
size="$(png_size "$WORK/toplevel.png")"
if [ -z "$size" ]; then
    echo "toplevel capture did not produce a PNG" >&2
    exit 1
fi
tw="${size% *}"
th="${size#* }"
# 抓的是窗口（250x250 + SSD 边距），不是整屏
[ "$tw" -lt 1280 ] && [ "$th" -lt 720 ] || {
    echo "toplevel capture size unexpected: '$size'" >&2
    exit 1
}

echo "capture smoke OK: screencopy 1280x720 + region 200x150 + toplevel ${tw}x${th}"
