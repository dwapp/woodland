#!/usr/bin/env sh
# treeland-dde-shell-unstable-v2 冒烟：
#   bind / lifecycle / duplicate_error / 窗口角色切换与 layer 同步 / skip_flags / 键盘焦点拒绝
#
# 用法: sh run_dde_shell_smoke.sh <woodland 二进制> <dde_shell_probe> <wctl>
set -eu

. "$(dirname "$0")/lib.sh"
smoke_init dde-shell

BIN="${1:-./build/src/woodland}"
PROBE="${2:-./build/tests/dde_shell_probe}"
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
avail="$(wctl_cmd get_tree | jq -r '.dde_shell.available // false')"
[ "$avail" = "true" ] || smoke_fail "dde_shell is not available in get_tree"

# 2. Test lifecycle
"$PROBE" --lifecycle || smoke_fail "probe --lifecycle failed"

# 3. Test duplicate error
"$PROBE" --duplicate-error || smoke_fail "probe --duplicate-error failed"
dup_errors="$(wctl_cmd get_tree | jq -r '.dde_shell.duplicate_error_count // 0')"
[ "$dup_errors" -ge 1 ] || smoke_fail "duplicate error count was not recorded"

# 4. Test mapped window with ALWAYS_ON_TOP (role 2), position (50, 60), skip 0x5, focus 0
"$PROBE" --mapped 2 50 60 5 0 3 &
probe_pid=$!

# Wait for window to map
i=0
mapped=0
while [ "$i" -lt 30 ]; do
    found="$(wctl_cmd get_tree | jq '[.views[] | select(.title=="dde_shell_window")] | length')"
    if [ "$found" -ge 1 ]; then
        mapped=1
        break
    fi
    sleep 0.1
    i=$((i+1))
done
[ "$mapped" -eq 1 ] || smoke_fail "dde_shell window failed to map"

tree="$(wctl_cmd get_tree)"
view_id="$(echo "$tree" | jq -r '.views[] | select(.title=="dde_shell_window") | .id')"
is_aot="$(echo "$tree" | jq -r '.views[] | select(.title=="dde_shell_window") | .always_on_top')"
layer="$(echo "$tree" | jq -r '.views[] | select(.title=="dde_shell_window") | .layer')"
role="$(echo "$tree" | jq -r '.views[] | select(.title=="dde_shell_window") | .dde_shell.role')"
skip="$(echo "$tree" | jq -r '.views[] | select(.title=="dde_shell_window") | .dde_shell.skip_flags')"
focusable="$(echo "$tree" | jq -r '.views[] | select(.title=="dde_shell_window") | .focusable')"

[ "$is_aot" = "true" ] || smoke_fail "expected always_on_top=true, got $is_aot"
[ "$layer" = "1" ] || smoke_fail "expected layer=1, got $layer"
[ "$role" = "always_on_top" ] || smoke_fail "expected role=always_on_top, got $role"
[ "$skip" = "5" ] || smoke_fail "expected skip_flags=5, got $skip"
[ "$focusable" = "false" ] || smoke_fail "expected focusable=false, got $focusable"

# 5. User unpin via wctl
wctl_cmd "pin $view_id 0" || smoke_fail "wctl pin 0 failed"
tree2="$(wctl_cmd get_tree)"
is_aot2="$(echo "$tree2" | jq -r --arg id "$view_id" '.views[] | select(.id==($id|tonumber)) | .always_on_top')"
layer2="$(echo "$tree2" | jq -r --arg id "$view_id" '.views[] | select(.id==($id|tonumber)) | .layer')"
role2="$(echo "$tree2" | jq -r --arg id "$view_id" '.views[] | select(.id==($id|tonumber)) | .dde_shell.role')"

[ "$is_aot2" = "false" ] || smoke_fail "expected always_on_top=false after unpin, got $is_aot2"
[ "$layer2" = "0" ] || smoke_fail "expected layer=0 after unpin, got $layer2"
[ "$role2" = "normal" ] || smoke_fail "expected role=normal after unpin, got $role2"

wait "$probe_pid" 2>/dev/null || true
exit 0
