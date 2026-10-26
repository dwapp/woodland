#!/usr/bin/env sh
# 启动 woodland（headless），验证 Wayland 客户端能连接并看到关键全局对象。
# 用法: sh run_protocol_smoke.sh <woodland 二进制路径>
set -eu

BIN="${1:-./build/src/woodland}"
[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 125; }

command -v wayland-info >/dev/null 2>&1 || {
    echo "wayland-info not found, skip" >&2
    exit 77
}

# 用独立 socket 目录避免污染，并清掉父进程的 WAYLAND_DISPLAY
unset WAYLAND_DISPLAY
export XDG_RUNTIME_DIR="$(mktemp -d)"

WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 "$BIN" &
pid=$!

# 等待 socket 就绪（wl_display_add_socket_auto 命名 wayland-N）
i=0
while [ "$(ls "$XDG_RUNTIME_DIR/" 2>/dev/null | grep -c '^wayland-')" -eq 0 ] && [ "$i" -lt 50 ]; do
    sleep 0.1; i=$((i+1))
done

socket_name="$(ls "$XDG_RUNTIME_DIR/" 2>/dev/null | grep '^wayland-' | head -1)"
if [ -z "$socket_name" ]; then
    echo "wayland socket did not appear" >&2
    kill -TERM "$pid" 2>/dev/null || true
    exit 1
fi

# wayland-info 会连接并打印所有全局对象；验证 xdg_wm_base 和 wl_compositor 存在
export WAYLAND_DISPLAY="$socket_name"
output="$(wayland-info 2>&1 || true)"

kill -TERM "$pid" 2>/dev/null || true
i=0
while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 50 ]; do
    sleep 0.1; i=$((i+1))
done
if kill -0 "$pid" 2>/dev/null; then
    kill -KILL "$pid" 2>/dev/null || true
fi
wait "$pid" 2>/dev/null || true

rm -rf "$XDG_RUNTIME_DIR"

echo "$output" | grep -q 'wl_compositor' || {
    echo "wl_compositor global missing from output:" >&2
    echo "$output" >&2
    exit 1
}
echo "$output" | grep -q 'xdg_wm_base' || {
    echo "xdg_wm_base global missing from output:" >&2
    echo "$output" >&2
    exit 1
}
echo "$output" | grep -q 'ext_workspace_manager_v1' || {
    echo "ext_workspace_manager_v1 global missing from output:" >&2
    echo "$output" >&2
    exit 1
}
echo "$output" | grep -q 'zwlr_foreign_toplevel_manager_v1' || {
    echo "zwlr_foreign_toplevel_manager_v1 global missing from output:" >&2
    echo "$output" >&2
    exit 1
}

echo "protocol smoke OK: wl_compositor + xdg_wm_base + ext_workspace_manager_v1 + zwlr_foreign_toplevel_manager_v1 present"
