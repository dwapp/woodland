#!/usr/bin/env sh
set -eu

. "$(dirname "$0")/lib.sh"
smoke_init headless

BIN="${1:-./build/src/woodland}"
[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 125; }

# 日志落到文件而不是终端：退出码之外的东西（启动告警、崩溃栈）才是证据
SMOKE_LOG="$(mktemp)"
WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 "$BIN" \
    >"$SMOKE_LOG" 2>&1 &
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
    cat "$SMOKE_LOG" >&2 || :
    exit 1
fi

wait "$pid"
