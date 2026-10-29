#!/usr/bin/env sh
# idle-inhibit 聚合冒烟：最后一个抑制器销毁后 idle 必须恢复。
#
# 用法: sh run_idle_smoke.sh <woodland 二进制路径> <idle_probe 客户端路径>
set -eu

BIN="${1:-./build/src/woodland}"
PROBE="${2:-./build/tests/idle_probe}"
[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 125; }
[ -x "$PROBE" ] || { echo "probe not built: $PROBE" >&2; exit 125; }

unset WAYLAND_DISPLAY
export XDG_RUNTIME_DIR="$(mktemp -d)"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1

"$BIN" >"$XDG_RUNTIME_DIR/log" 2>&1 &
pid=$!
cleanup() {
    kill -TERM "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    rm -rf "$XDG_RUNTIME_DIR"
}
trap cleanup EXIT INT TERM

i=0
while [ "$(ls "$XDG_RUNTIME_DIR/" 2>/dev/null | grep -c '^wayland-')" -eq 0 ] \
        && [ "$i" -lt 50 ]; do
    sleep 0.1; i=$((i+1))
done
socket_name="$(ls "$XDG_RUNTIME_DIR/" 2>/dev/null | grep '^wayland-' | head -1)"
if [ -z "$socket_name" ]; then
    echo "wayland socket did not appear" >&2
    exit 1
fi
export WAYLAND_DISPLAY="$socket_name"

set +e
"$PROBE"
rc=$?
set -e
if [ "$rc" -ne 0 ]; then
    echo "--- 合成器日志:" >&2
    cat "$XDG_RUNTIME_DIR/log" >&2 || true
    exit "$rc"
fi

echo "idle-inhibit smoke passed"
