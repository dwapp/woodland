#!/usr/bin/env sh
# 启动 woodland（headless），验证 Wayland 客户端能连接并看到关键全局对象。
# 用法: sh run_protocol_smoke.sh <woodland 二进制路径>
set -eu

. "$(dirname "$0")/lib.sh"
smoke_init protocol

BIN="${1:-./build/src/woodland}"
[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 125; }

command -v wayland-info >/dev/null 2>&1 || {
    echo "wayland-info not found, skip" >&2
    exit 77
}

# 用独立 socket 目录避免污染，并清掉父进程的 WAYLAND_DISPLAY
unset WAYLAND_DISPLAY
export XDG_RUNTIME_DIR="$(mktemp -d)"

SMOKE_LOG="$XDG_RUNTIME_DIR/woodland.log"
WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 "$BIN" \
    >"$SMOKE_LOG" 2>&1 &
pid=$!

# 这个脚本原来没有收尾：跑完把合成器留在后台了。接上 lib 之后由它负责。
smoke_cleanup() {
    kill -TERM "$pid" 2>/dev/null || :
    i=0
    while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 50 ]; do
        sleep 0.1; i=$((i+1))
    done
    kill -KILL "$pid" 2>/dev/null || :
    wait "$pid" 2>/dev/null || :
    rm -rf "$XDG_RUNTIME_DIR"
}

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

# primary-selection 端到端跑一次：只有握手没有行为的话，这个协议等于没接。
# 用 --paste-once 让 wl-copy 的守护进程粘一次就退出，不留残进程。
if command -v wl-copy >/dev/null 2>&1 && command -v wl-paste >/dev/null 2>&1; then
    printf 'woodland-primary' | wl-copy --primary --paste-once -n 2>/dev/null || true
    got=""
    i=0
    while [ "$i" -lt 20 ]; do
        got="$(wl-paste --primary --no-newline 2>/dev/null || true)"
        [ "$got" = "woodland-primary" ] && break
        sleep 0.1; i=$((i+1))
    done
    if [ "$got" != "woodland-primary" ]; then
        echo "primary selection round trip failed (got '$got')" >&2
        kill -TERM "$pid" 2>/dev/null || true
        exit 1
    fi
else
    echo "wl-copy/wl-paste not found, skip primary-selection round trip" >&2
fi

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
echo "$output" | grep -q 'zwlr_layer_shell_v1' || {
    echo "zwlr_layer_shell_v1 global missing from output:" >&2
    echo "$output" >&2
    exit 1
}
echo "$output" | grep -q 'ext_data_control_manager_v1' || {
    echo "ext_data_control_manager_v1 global missing from output:" >&2
    echo "$output" >&2
    exit 1
}
echo "$output" | grep -q 'zwlr_data_control_manager_v1' || {
    echo "zwlr_data_control_manager_v1 global missing from output:" >&2
    echo "$output" >&2
    exit 1
}
echo "$output" | grep -q 'wp_alpha_modifier_v1' || {
    echo "wp_alpha_modifier_v1 global missing from output:" >&2
    echo "$output" >&2
    exit 1
}
echo "$output" | grep -q 'wp_single_pixel_buffer_manager_v1' || {
    echo "wp_single_pixel_buffer_manager_v1 global missing from output:" >&2
    echo "$output" >&2
    exit 1
}
echo "$output" | grep -q 'wp_fractional_scale_manager_v1' || {
    echo "wp_fractional_scale_manager_v1 global missing from output:" >&2
    echo "$output" >&2
    exit 1
}
echo "$output" | grep -q 'zxdg_decoration_manager_v1' || {
    echo "zxdg_decoration_manager_v1 global missing from output:" >&2
    echo "$output" >&2
    exit 1
}
# 上游协议接线（src/protocol）：每个都对应一个真实客户端需求，
# 少一个就意味着某类程序在这台合成器上行为不对
for iface in zwlr_screencopy_manager_v1 \
             ext_image_copy_capture_manager_v1 \
             ext_output_image_capture_source_manager_v1 \
             ext_foreign_toplevel_image_capture_source_manager_v1 \
             ext_foreign_toplevel_list_v1 \
             zxdg_output_manager_v1 \
             xdg_wm_dialog_v1 \
             wp_viewporter \
             wp_presentation \
             zwp_primary_selection_device_manager_v1 \
             zwp_relative_pointer_manager_v1 \
             zwp_pointer_gestures_v1 \
             zwp_idle_inhibit_manager_v1 \
             ext_idle_notifier_v1 \
             zwp_keyboard_shortcuts_inhibit_manager_v1 \
             zwlr_output_power_manager_v1 \
             zwlr_output_manager_v1 \
             xdg_activation_v1 \
             xdg_toplevel_tag_manager_v1; do
    echo "$output" | grep -q "$iface" || {
        echo "$iface global missing from output:" >&2
        echo "$output" >&2
        exit 1
    }
done

echo "protocol smoke OK: all expected globals present"
