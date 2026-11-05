#!/usr/bin/env sh
# treeland 壁纸协议冒烟：manager（配置）+ notifier（广播）+ shell（角色）。
#
# 用法: sh run_wallpaper_smoke.sh <woodland 二进制路径> <wallpaper_probe 路径> [wctl 路径]
#
# 除了探针自己断言的事件序列，这里还检查两件只有合成器侧能看到的事：
#   1. wallpaper.json 真的写出来了，且带着输出名和新设的源；
#   2. 合成器日志里确实建了壁纸 surface（scene 节点那一侧没被跳过）；
#   3. 壁纸树确实在 scene 最底层（wctl self_check 的 wallpaper_below_layers），
#      这条不变量掉了只表现为"壁纸被 layer-shell 盖住"，肉眼几乎看不出来。
set -eu

. "$(dirname "$0")/lib.sh"
smoke_init wallpaper

BIN="${1:-./build/src/woodland}"
PROBE="${2:-./build/tests/wallpaper_probe}"
WCTL="${3:-}"
[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 125; }
[ -x "$PROBE" ] || { echo "probe not built: $PROBE" >&2; exit 125; }

unset WAYLAND_DISPLAY
export XDG_RUNTIME_DIR="$(mktemp -d)"
export XDG_CONFIG_HOME="$XDG_RUNTIME_DIR/config"
export XDG_DATA_HOME="$XDG_RUNTIME_DIR/data"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME"

# 壁纸协议依赖 headless 输出（manager 的 get_treeland_wallpaper 要一个 wl_output）
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1

SMOKE_LOG="$XDG_RUNTIME_DIR/log"
"$BIN" >"$SMOKE_LOG" 2>&1 &
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
if [ -z "$socket_name" ]; then
    smoke_fail "wayland socket did not appear"
fi
export WAYLAND_DISPLAY="$socket_name"

set +e
"$PROBE"
rc=$?
set -e
if [ "$rc" -ne 0 ]; then
    smoke_fail "wallpaper probe exited with $rc"
fi

config="$XDG_CONFIG_HOME/woodland/wallpaper.json"
if [ ! -f "$config" ]; then
    smoke_fail "wallpaper config was not written to $config"
fi
grep -q '"outputName"' "$config" \
    || smoke_fail "config has no outputName: $(cat "$config")"
grep -q 'smoke-image.jpg' "$config" \
    || smoke_fail "config does not record the image source: $(cat "$config")"
grep -q 'smoke-video.mp4' "$config" \
    || smoke_fail "config does not record the video source: $(cat "$config")"

grep -q 'wallpaper surface for' "$SMOKE_LOG" \
    || smoke_fail "compositor never created the wallpaper surface"

# 壁纸树必须还是 scene 根的第一个子节点。这条只有在**有输出**的时候才有意义，
# 所以放在主流程之后、重启之前跑。
if [ -n "$WCTL" ] && [ -x "$WCTL" ]; then
    SOCK="$XDG_RUNTIME_DIR/woodland-$WAYLAND_DISPLAY.sock"
    CHECK="$("$WCTL" -s "$SOCK" self_check)" || smoke_fail "self_check failed: $CHECK"
    printf '%s' "$CHECK" | jq -e \
        '.checks[] | select(.name == "wallpaper_below_layers") | .ok == true' >/dev/null \
        || smoke_fail "wallpaper tree is not the bottom-most scene child: $CHECK"
fi

# 重启一次读配置：解析器必须能吃回自己写的文件（写/读不对称是最常见的坑）。
# 先清掉上一轮的 socket（它已经没人监听了），否则下面的等待循环会立刻返回，
# 拿到的却是个死 socket。
kill -TERM "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
rm -f "$XDG_RUNTIME_DIR"/wayland-* "$XDG_RUNTIME_DIR"/*.lock

"$BIN" >"$SMOKE_LOG.restart" 2>&1 &
pid=$!
i=0
while [ "$(ls "$XDG_RUNTIME_DIR/" 2>/dev/null | grep -c '^wayland-')" -eq 0 ] \
        && [ "$i" -lt 50 ]; do
    sleep 0.1; i=$((i+1))
done
socket_name="$(ls "$XDG_RUNTIME_DIR/" 2>/dev/null | grep '^wayland-' | head -1)"
[ -n "$socket_name" ] || smoke_fail "wayland socket did not come back after restart"
export WAYLAND_DISPLAY="$socket_name"

grep -q 'loaded wallpaper config: [1-9][0-9]* outputs' "$SMOKE_LOG.restart" \
    || smoke_fail "config was not reloaded: $(cat "$SMOKE_LOG.restart")"
if grep -q 'is malformed' "$SMOKE_LOG.restart"; then
    smoke_fail "config written by the compositor could not be parsed back"
fi

# 读回来的桌面壁纸必须还是第一轮设的那张
set +e
"$PROBE" --expect-desktop "$XDG_DATA_HOME/wallpapers/smoke-image.jpg"
rc=$?
set -e
if [ "$rc" -ne 0 ]; then
    cat "$SMOKE_LOG.restart" >&2 || true
    smoke_fail "restored wallpaper does not match the saved config"
fi

# ---------------------------------------------------------------------------
# 工作区联动：协议本身没法"只给某个工作区设壁纸"（set_*_source 会写全部
# workspace，和 treeland 的 setOutputWallpaper 一致），所以这里直接改配置
# 文件，把 workspaceIndex 1 的桌面壁纸换成另一张，再起一份干净的合成器。
#
# 用 sed 按 workspaceIndex 定位而不是数"第几次出现"：合成器写的是紧凑
# JSON（整份文件一行），按行计数根本数不到。这样也顺带把写入器的键顺序
# 钉住了——它变了这里就会失败，而不是悄悄改错一行。
# ---------------------------------------------------------------------------
kill -TERM "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
rm -f "$XDG_RUNTIME_DIR"/wayland-* "$XDG_RUNTIME_DIR"/*.lock

second_source="$XDG_DATA_HOME/wallpapers/smoke-workspace-2.jpg"
echo "second workspace wallpaper" >"$second_source"

# 分隔符用 | 而不是 /：替换进去的是绝对路径，自带一堆 /。
sed 's|"workspaceIndex":1,"desktopWallpaper":"[^"]*"|"workspaceIndex":1,"desktopWallpaper":"'"$second_source"'"|' \
    "$config" >"$config.new"
mv "$config.new" "$config"

grep -q 'smoke-workspace-2.jpg' "$config" \
    || smoke_fail "could not patch the second workspace in the config"

"$BIN" >"$SMOKE_LOG.workspace" 2>&1 &
pid=$!
i=0
while [ "$(ls "$XDG_RUNTIME_DIR/" 2>/dev/null | grep -c '^wayland-')" -eq 0 ] \
        && [ "$i" -lt 50 ]; do
    sleep 0.1; i=$((i+1))
done
socket_name="$(ls "$XDG_RUNTIME_DIR/" 2>/dev/null | grep '^wayland-' | head -1)"
[ -n "$socket_name" ] || smoke_fail "wayland socket did not come back for the workspace phase"
export WAYLAND_DISPLAY="$socket_name"

set +e
"$PROBE" --workspace "$XDG_DATA_HOME/wallpapers/smoke-image.jpg" "$second_source"
rc=$?
set -e
if [ "$rc" -ne 0 ]; then
    cat "$SMOKE_LOG.workspace" >&2 || true
    smoke_fail "workspace wallpaper selection probe exited with $rc"
fi

grep -q 'activating workspace 2' "$SMOKE_LOG.workspace" \
    || smoke_fail "compositor never switched to workspace 2"

echo "wallpaper smoke passed"
