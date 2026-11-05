#!/usr/bin/env sh
# ext-background-effect-v1 冒烟：起 headless 合成器 + 真客户端设 blur region，
# 然后用 wctl 断言 get_tree 里的 bg_effect。
#
# 断言的是**协议状态**（区域双缓冲后变成什么、越界区域有没有被裁到 surface、
# 对象销毁后有没有撤掉），不是像素：模糊长什么样取决于 scenefx 与 GPU，
# 而"客户端要的那块区域合成器记住了没有"是确定的。
#
# 用法: sh run_bg_effect_smoke.sh <woodland 二进制> <deco_probe> <wctl>
set -eu

. "$(dirname "$0")/lib.sh"
smoke_init bg_effect

BIN="${1:-./build/src/woodland}"
PROBE="${2:-./build/tests/deco_probe}"
WCTL="${3:-./build/src/wctl/wctl}"
[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 125; }
[ -x "$PROBE" ] || { echo "probe not built: $PROBE" >&2; exit 125; }
[ -x "$WCTL" ] || { echo "wctl not found: $WCTL" >&2; exit 125; }
command -v jq >/dev/null 2>&1 || { echo "jq not found, skip bg-effect smoke" >&2; exit 77; }

unset WAYLAND_DISPLAY
export XDG_RUNTIME_DIR="$(mktemp -d)"
export XDG_CONFIG_HOME="$XDG_RUNTIME_DIR/config"
mkdir -p "$XDG_CONFIG_HOME"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1

SMOKE_LOG="$XDG_RUNTIME_DIR/woodland.log"
comp_pid=""
probe_pid=""
SOCK=""
TREE=""

smoke_cleanup() {
    if [ -n "$probe_pid" ]; then
        kill -KILL "$probe_pid" 2>/dev/null || true
        probe_pid=""
    fi
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

"$BIN" --self-check --trace >"$SMOKE_LOG" 2>&1 &
comp_pid=$!

i=0
while [ "$i" -lt 100 ]; do
    for f in "$XDG_RUNTIME_DIR"/woodland-*.sock; do
        [ -S "$f" ] && SOCK="$f"
    done
    [ -n "$SOCK" ] && break
    sleep 0.05; i=$((i+1))
done
[ -n "$SOCK" ] || smoke_fail "debug socket never appeared"

EFFECTS="$("$WCTL" -s "$SOCK" get_tree | jq -r '.effects')"

view_count() {
    "$WCTL" -s "$SOCK" get_tree 2>/dev/null | jq -r '.count // -1'
}

wait_view_count() {
    _want="$1"
    _i=0
    while [ "$_i" -lt 100 ]; do
        [ "$(view_count)" = "$_want" ] && return 0
        sleep 0.05; _i=$((_i+1))
    done
    return 1
}

scenario() {
    _name="$1"; shift
    _out="$XDG_RUNTIME_DIR/probe.$_name.log"
    "$PROBE" "$@" >"$_out" 2>&1 &
    probe_pid=$!
    _i=0
    while [ "$_i" -lt 120 ]; do
        if grep -q '^READY' "$_out" 2>/dev/null; then
            break
        fi
        if ! kill -0 "$probe_pid" 2>/dev/null; then
            echo "--- probe output ($_name):" >&2
            cat "$_out" >&2 || true
            smoke_fail "probe exited before READY ($_name)"
        fi
        sleep 0.05; _i=$((_i+1))
    done
    grep -q '^READY' "$_out" 2>/dev/null || smoke_fail "probe never became ready ($_name)"
    wait_view_count 1 || smoke_fail "view did not appear ($_name)"
    TREE="$("$WCTL" -s "$SOCK" get_tree)" || smoke_fail "get_tree failed ($_name)"
}

stop_probe() {
    if [ -n "$probe_pid" ]; then
        kill -TERM "$probe_pid" 2>/dev/null || true
        wait "$probe_pid" 2>/dev/null || true
        probe_pid=""
    fi
    wait_view_count 0 || smoke_fail "previous probe window did not go away"
}

expect() {
    echo "$TREE" | jq -e "$1" >/dev/null || {
        echo "--- get_tree:" >&2
        echo "$TREE" | jq '.views[0] | {bg_effect, deco, content}' >&2 || true
        smoke_fail "$2"
    }
}

echo "== 能力位：没编 effects 时不能对外宣称会模糊 =="
CAPS_OUT="$XDG_RUNTIME_DIR/probe.caps.log"
"$PROBE" --caps --no-context --hold 300 >"$CAPS_OUT" 2>&1 &
probe_pid=$!
i=0
while [ "$i" -lt 120 ]; do
    grep -q '^CAPS ' "$CAPS_OUT" 2>/dev/null && break
    kill -0 "$probe_pid" 2>/dev/null || smoke_fail "probe died before CAPS"
    sleep 0.05; i=$((i+1))
done
grep -q '^CAPS ' "$CAPS_OUT" 2>/dev/null || smoke_fail "no CAPS line from probe"
caps="$(sed -n 's/^CAPS //p' "$CAPS_OUT" | head -1)"
if [ "$EFFECTS" = "true" ]; then
    [ "$caps" = "1" ] || smoke_fail "effects build must advertise blur (caps=$caps)"
else
    [ "$caps" = "0" ] || smoke_fail "non-effects build must advertise no capability (caps=$caps)"
fi
kill -TERM "$probe_pid" 2>/dev/null || true
wait "$probe_pid" 2>/dev/null || true
probe_pid=""
wait_view_count 0 || smoke_fail "caps probe window did not go away"

echo "== 整窗模糊：区域原样记下 =="
scenario whole blur 0 0 400 300
expect '.views[0].bg_effect.blur == true' "blur must be on"
expect '.views[0].bg_effect.region == [0, 0, 400, 300]' "whole-surface region"

# 协议状态对不代表画得对。effects 构建里必须真的多出一个铺满内容的 blur 节点，
# 非 effects 构建里**不许**有——"没编就不会假装模糊"这条要能被断言。
SCENE="$("$WCTL" -s "$SOCK" dump_scene)" || smoke_fail "dump_scene failed"
blur_nodes="$(echo "$SCENE" | jq '[.. | objects | select(.type == "blur")] | length')"
if [ "$EFFECTS" = "true" ]; then
    [ "$blur_nodes" -ge 1 ] || smoke_fail "effects build must create a blur scene node"
    echo "$SCENE" | jq -e '[.. | objects | select(.type == "blur")
        | select(.w == 400 and .h == 300)] | length >= 1' >/dev/null \
        || smoke_fail "blur node must cover the whole content"
else
    [ "$blur_nodes" = "0" ] || smoke_fail "non-effects build must not create blur nodes"
fi
stop_probe

echo "== 局部区域：只糊指定的一块 =="
scenario partial blur 50 40 200 100
expect '.views[0].bg_effect.blur == true' "partial blur must be on"
expect '.views[0].bg_effect.region == [50, 40, 200, 100]' "region must be recorded as-is"
stop_probe

echo "== 越界区域：裁到 surface 尺寸 =="
scenario oversized blur 0 0 1000 1000
expect '.views[0].bg_effect.blur == true' "oversized blur must be on"
expect '.views[0].bg_effect.region == [0, 0, 400, 300]' "region must be clipped to the surface"
stop_probe

echo "== 没有 effect 对象：blur=false =="
scenario none --no-context
expect '.views[0].bg_effect.blur == false' "no effect object means no blur"
expect '.views[0].bg_effect.region == [0, 0, 0, 0]' "empty region"
stop_probe

echo "== 销毁 effect 对象：模糊撤掉 =="
DROP_OUT="$XDG_RUNTIME_DIR/probe.drop.log"
"$PROBE" --no-context blur 0 0 400 300 --drop-effect --hold 3000 >"$DROP_OUT" 2>&1 &
probe_pid=$!
i=0
while [ "$i" -lt 120 ]; do
    grep -q '^EFFECT_DROPPED' "$DROP_OUT" 2>/dev/null && break
    kill -0 "$probe_pid" 2>/dev/null || smoke_fail "probe died before EFFECT_DROPPED"
    sleep 0.05; i=$((i+1))
done
grep -q '^EFFECT_DROPPED' "$DROP_OUT" 2>/dev/null || smoke_fail "effect was never dropped"
TREE="$("$WCTL" -s "$SOCK" get_tree)" || smoke_fail "get_tree failed (drop)"
expect '.views[0].bg_effect.blur == false' "destroyed effect must not keep blurring"
stop_probe

echo "== 决策轨迹与不变量 =="
"$WCTL" -s "$SOCK" get_trace | jq -e '
    [.entries[] | select(.what == "bg_effect")] | length >= 3' >/dev/null \
    || smoke_fail "bg_effect trace entries missing"
"$WCTL" -s "$SOCK" self_check | jq -e '.ok == true' >/dev/null \
    || smoke_fail "self_check failed after background effect changes"

echo "bg-effect smoke passed"
