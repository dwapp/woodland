#!/usr/bin/env sh
# 拖拽（DnD）冒烟：合成器必须对 wl_data_device.start_drag 有回应。
#
# 断言两件事：
#   1. invalid serial 的 start_drag 要以 wl_data_source.cancelled 结束
#      （不回应的话客户端会一直挂着——这正是补 DnD 之前的行为）；
#   2. 这次拒绝要出现在决策轨迹里（标签 dnd），且事后 dnd 状态干净
#      （active=false、没有残留图标）——图标树忘了关就是屏幕上留个影子。
#
# 真拖拽（按下 → 拖 → 投放）没法 headless 驱动：serial 必须来自真实的
# pointer button press。那条路径走真机手测清单，见设计文档 §8.3。
#
# 用法: sh run_dnd_smoke.sh <woodland 二进制> <dnd_probe 二进制> <wctl 二进制>
set -eu

. "$(dirname "$0")/lib.sh"
smoke_init dnd

BIN="${1:-./build/src/woodland}"
PROBE="${2:-./build/tests/dnd_probe}"
WCTL="${3:-./build/src/wctl/wctl}"
[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 125; }
[ -x "$PROBE" ] || { echo "probe not found: $PROBE" >&2; exit 125; }
[ -x "$WCTL" ] || { echo "wctl not found: $WCTL" >&2; exit 125; }

command -v jq >/dev/null 2>&1 || {
    echo "jq not found, skip dnd smoke" >&2
    exit 77
}

unset WAYLAND_DISPLAY
export XDG_RUNTIME_DIR="$(mktemp -d)"
# WLR_RENDERER=pixman 只在 -Deffects=disabled 的构建里生效（effects 构建由
# fx_renderer_create 自己找渲染节点）：ASan 构建靠它避开 Mesa/EGL 的退出泄漏，
# 否则"退出码必须 0"这条断言会被 LeakSanitizer 染红（run_headless.sh 同款）。
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman

SMOKE_LOG="$XDG_RUNTIME_DIR/woodland.log"
comp_pid=""

smoke_cleanup() {
    if [ -n "$comp_pid" ]; then
        kill -TERM "$comp_pid" 2>/dev/null || true
        i=0
        while kill -0 "$comp_pid" 2>/dev/null && [ "$i" -lt 50 ]; do
            sleep 0.1; i=$((i+1))
        done
        kill -KILL "$comp_pid" 2>/dev/null || true
        wait "$comp_pid" 2>/dev/null || true
    fi
    rm -rf "$XDG_RUNTIME_DIR"
}

SMOKE_FLAGS="--self-check --trace"
"$BIN" --self-check --trace >"$SMOKE_LOG" 2>&1 &
comp_pid=$!

# 等 wayland socket 与调试 socket 都就绪
SOCK=""
WAYLAND_SOCK=""
i=0
while [ "$i" -lt 100 ]; do
    for f in "$XDG_RUNTIME_DIR"/woodland-*.sock; do
        [ -S "$f" ] && SOCK="$f"
    done
    for f in "$XDG_RUNTIME_DIR"/wayland-*; do
        [ -S "$f" ] && WAYLAND_SOCK="${f##*/}"
    done
    [ -n "$SOCK" ] && [ -n "$WAYLAND_SOCK" ] && break
    sleep 0.05; i=$((i+1))
done
[ -n "$SOCK" ] || smoke_fail "debug socket never appeared"
[ -n "$WAYLAND_SOCK" ] || smoke_fail "wayland socket never appeared"

echo "== start_drag 必须被回应 =="
if ! WAYLAND_DISPLAY="$WAYLAND_SOCK" "$PROBE" >"$XDG_RUNTIME_DIR/probe.log" 2>&1; then
    cat "$XDG_RUNTIME_DIR/probe.log" >&2 || :
    smoke_fail "dnd probe failed"
fi
cat "$XDG_RUNTIME_DIR/probe.log" >&2 || :

echo "== 拒绝的决策要进轨迹 =="
TRACE="$("$WCTL" -s "$SOCK" get_trace)" || smoke_fail "get_trace failed"
echo "$TRACE" | jq -e '[.entries[] | select(.what == "dnd")] | length >= 1' >/dev/null \
    || smoke_fail "no dnd entry in trace: $TRACE"
echo "$TRACE" | jq -e '[.entries[] | select(.what == "dnd" and (.detail | startswith("reject")))] | length >= 1' >/dev/null \
    || smoke_fail "dnd request should have been rejected with an invalid serial"

echo "== 拒绝之后状态必须干净 =="
TREE="$("$WCTL" -s "$SOCK" get_tree)" || smoke_fail "get_tree failed"
echo "$TREE" | jq -e '.dnd.active == false and .dnd.dropped == false' >/dev/null \
    || smoke_fail "dnd should not stay active after a rejected drag: $TREE"
echo "$TREE" | jq -e '.dnd.icon.visible == false' >/dev/null \
    || smoke_fail "rejected drag left a visible icon node"
CHECK="$("$WCTL" -s "$SOCK" self_check)" || smoke_fail "self_check reported failures"
echo "$CHECK" | jq -e '.ok == true and .failures == 0' >/dev/null \
    || smoke_fail "self_check not clean after rejected drag: $CHECK"

# 正常退出：SIGTERM 后退出码必须是 0（不留半拆状态）
kill -TERM "$comp_pid" 2>/dev/null || true
i=0
while kill -0 "$comp_pid" 2>/dev/null && [ "$i" -lt 50 ]; do
    sleep 0.1; i=$((i+1))
done
if kill -0 "$comp_pid" 2>/dev/null; then
    kill -KILL "$comp_pid" 2>/dev/null || true
    smoke_fail "compositor did not exit on SIGTERM"
fi
set +e
wait "$comp_pid"
rc=$?
set -e
comp_pid=""
[ "$rc" -eq 0 ] || smoke_fail "compositor exited with $rc"

# 退出路径上不许有 self-check 报警
if grep -q "self-check" "$SMOKE_LOG"; then
    grep "self-check" "$SMOKE_LOG" >&2
    smoke_fail "self-check logged a failure"
fi

echo "dnd smoke OK: start_drag answered, rejected drag left no state"
