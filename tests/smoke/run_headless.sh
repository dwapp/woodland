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

# ---------------------------------------------------------------------------
# 起不来的那条路也得体面：退出码不许 >= 128（信号）。
#
# WLR_RENDERER_FORCE_SOFTWARE=1 让 EGL 不依赖 DRM 节点也能起来，但 buffer 分配
# 走不通：effects 构建的 fx renderer 只声明 DMABUF 能力位，唯一能用的 gbm
# 分配器要有 DRM render node。于是这个进程必然在 allocator 那里失败。这条测的
# 不是"能起来"，是"起不来时干净退出 1 而不是段错误"—— 曾经 finish 会无条件遍历
# 还没初始化的 workspace/output 链表（头是 {0}，wl_list_for_each_safe 第一次就
# 解引用 NULL），栈落在 woodland_workspace_finish。
#
# 无 effects 构建里同一条命令是能起来的（pixman/gles2 有 shm 路径），那样会被
# timeout 收成 124 —— 两种结果都合法，都落在"不是信号"里。
#
# 消毒器构建下这条判不了：LSan 会因为 mesa 在退出时不释放的 DRI 缓存 abort()
# （rc=134），和"我们崩了"在退出码上无法区分。跳过，别给假警报——ASan 构建下
# 真正的内存错误由 ASan 自己打栈（halt_on_error=1）。
# ---------------------------------------------------------------------------
if [ -n "${ASAN_OPTIONS:-}" ]; then
    echo "  (sanitizer build: exit code is polluted by LSan, skipping)"
else
    FAIL_LOG="$(mktemp)"
    fail_rc=0
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER_FORCE_SOFTWARE=1 \
        timeout 3 "$BIN" >"$FAIL_LOG" 2>&1 || fail_rc=$?
    if [ "$fail_rc" -ge 128 ]; then
        echo "--- startup-failure log:" >&2
        tail -5 "$FAIL_LOG" >&2 || :
        smoke_fail "woodland died by signal on a failed startup (rc=$fail_rc)"
    fi
    rm -f "$FAIL_LOG"
fi
