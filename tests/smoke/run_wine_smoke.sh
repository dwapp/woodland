#!/usr/bin/env sh
# treeland-wine-window-management-unstable-v1 & treeland-wine-window-state-unstable-v1 冒烟：
#   bind / window_id / configure_position / configure_stacking / set_position / set_z_order /
#   state_changed / attention hint / unminimize / activate
#
# 用法: sh run_wine_smoke.sh <woodland 二进制> <wine_probe> <wctl>
set -eu

. "$(dirname "$0")/lib.sh"
smoke_init wine-protocols

BIN="${1:-./build/src/woodland}"
PROBE="${2:-./build/tests/wine_probe}"
WCTL="${3:-}"
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
SMOKE_FLAGS="--trace --debug-ctl"
SMOKE_LOG="$XDG_RUNTIME_DIR/log"
"$BIN" --trace --debug-ctl >"$SMOKE_LOG" 2>&1 &
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
[ -n "$socket_name" ] || smoke_fail "wayland socket did not appear"
export WAYLAND_DISPLAY="$socket_name"
smoke_wait_socket || smoke_fail "debug socket did not appear"

wctl_cmd() {
    "$WCTL" -s "$SOCK" -t 1000 "$1"
}

# 1. Test bind
"$PROBE" --bind || smoke_fail "probe --bind failed"
wm_avail="$(wctl_cmd get_tree | jq -r '.wine_window_management.available // false')"
ws_avail="$(wctl_cmd get_tree | jq -r '.wine_window_state.available // false')"
[ "$wm_avail" = "true" ] || smoke_fail "wine_window_management is not available in get_tree"
[ "$ws_avail" = "true" ] || smoke_fail "wine_window_state is not available in get_tree"

# 2. Test Window Management
"$PROBE" --wm || smoke_fail "probe --wm failed"
pos_reqs="$(wctl_cmd get_tree | jq -r '.wine_window_management.position_requests // 0')"
z_reqs="$(wctl_cmd get_tree | jq -r '.wine_window_management.z_order_requests // 0')"
[ "$pos_reqs" -ge 1 ] || smoke_fail "position_requests not recorded"
[ "$z_reqs" -ge 1 ] || smoke_fail "z_order_requests not recorded"

# 3. Test Window State
"$PROBE" --ws || smoke_fail "probe --ws failed"
att_set="$(wctl_cmd get_tree | jq -r '.wine_window_state.attention_set_count // 0')"
att_clr="$(wctl_cmd get_tree | jq -r '.wine_window_state.attention_clear_count // 0')"
[ "$att_set" -ge 1 ] || smoke_fail "attention_set_count not recorded"
[ "$att_clr" -ge 1 ] || smoke_fail "attention_clear_count not recorded"

# 4. Test HWND_INSERT_AFTER
"$PROBE" --insert-after || smoke_fail "probe --insert-after failed"

exit 0
