#!/usr/bin/env sh
set -eu

BIN="${1:-./build/src/woodland}"
[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 125; }

WLR_BACKENDS=headless WLR_RENDERER=pixman "$BIN" &
pid=$!

# 等待事件循环就绪
sleep 1
kill -TERM "$pid" 2>/dev/null || true

# 最多再等 5 秒退出
i=0
while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 50 ]; do
    sleep 0.1; i=$((i+1))
done

if kill -0 "$pid" 2>/dev/null; then
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    echo "woodland did not exit on SIGTERM" >&2
    exit 1
fi

wait "$pid"
