#!/usr/bin/env sh
# ext-background-effect-v1 冒烟：起 headless 合成器 + 真客户端设 blur region，
# 然后用 wctl 断言 get_tree 里的 bg_effect。
#
# 断言的是**协议状态**（区域双缓冲后变成什么、越界区域有没有被裁到 surface、
# 对象销毁后有没有撤掉），不是像素：模糊长什么样取决于 scenefx 与 GPU，
# 而"客户端要的那块区域合成器记住了没有"是确定的。
#
# 跑两遍：默认配置（[effects] enabled=true，模糊真画）与显式关掉（capability=0、
# 一个 blur 节点都没有、协议状态照记）。编译期没有 effects 开关，这个配置面
# 就是唯一的"关掉特效"入口，必须每次都验。
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
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1

SMOKE_LOG=""
comp_pid=""
probe_pid=""
SOCK=""
TREE=""

stop_compositor() {
    if [ -n "$comp_pid" ]; then
        kill -TERM "$comp_pid" 2>/dev/null || true
        i=0
        while kill -0 "$comp_pid" 2>/dev/null && [ "$i" -lt 50 ]; do
            sleep 0.1; i=$((i+1))
        done
        kill -KILL "$comp_pid" 2>/dev/null || true
        wait "$comp_pid" 2>/dev/null || true
        comp_pid=""
    fi
    SOCK=""
}

# 特效是运行期开关，所以同一套场景要能按配置跑两遍：默认（开）与显式关掉。
# 配置写进临时 XDG_CONFIG_HOME，~/.config 里的用户配置不参与。
start_compositor() {
    mkdir -p "$XDG_CONFIG_HOME/woodland"
    cat >"$XDG_CONFIG_HOME/woodland/config.toml" <<EOF
[effects]
enabled = $1
blur = true
corner_radius = 12
EOF
    SMOKE_LOG="$XDG_RUNTIME_DIR/compositor.$2.log"
    "$BIN" --self-check --trace >"$SMOKE_LOG" 2>&1 &
    comp_pid=$!

    SOCK=""
    i=0
    while [ "$i" -lt 100 ]; do
        for f in "$XDG_RUNTIME_DIR"/woodland-*.sock; do
            [ -S "$f" ] && SOCK="$f"
        done
        [ -n "$SOCK" ] && break
        sleep 0.05; i=$((i+1))
    done
    [ -n "$SOCK" ] || smoke_fail "debug socket never appeared ($2)"
}

smoke_cleanup() {
    if [ -n "$probe_pid" ]; then
        kill -KILL "$probe_pid" 2>/dev/null || true
        probe_pid=""
    fi
    stop_compositor
    rm -rf "$XDG_RUNTIME_DIR"
}

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

check_caps() {
    _want="$1"
    _why="$2"
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
    [ "$caps" = "$_want" ] || smoke_fail "$_why (expected caps=$_want, got $caps)"
    kill -TERM "$probe_pid" 2>/dev/null || true
    wait "$probe_pid" 2>/dev/null || true
    probe_pid=""
    wait_view_count 0 || smoke_fail "caps probe window did not go away"
}

echo "== 能力位：如实报支持模糊 =="
start_compositor true default
EFFECTS="$("$WCTL" -s "$SOCK" get_tree | jq -r '.effects')"

if [ "$EFFECTS" = "true" ]; then
    check_caps 1 "effects default to on, so blur must be advertised"
else
    check_caps 0 "non-effects build must not advertise blur"
fi

echo "== 整窗模糊：区域原样记下 =="
scenario whole blur 0 0 400 300
expect '.views[0].bg_effect.blur == true' "blur must be on"
expect '.views[0].bg_effect.region == [0, 0, 400, 300]' "whole-surface region"

# 协议状态对不代表画得对。开着特效时必须真的多出一个铺满内容的 blur 节点，
# 关掉时**不许**有——"关了就不假装模糊"这条要能被断言。
SCENE="$("$WCTL" -s "$SOCK" dump_scene)" || smoke_fail "dump_scene failed"
blur_nodes="$(echo "$SCENE" | jq '[.. | objects | select(.type == "blur")] | length')"
if [ "$EFFECTS" = "true" ]; then
    [ "$blur_nodes" -ge 1 ] || smoke_fail "effects on: must create a blur scene node"
    echo "$SCENE" | jq -e '[.. | objects | select(.type == "blur")
        | select(.w == 400 and .h == 300)] | length >= 1' >/dev/null \
        || smoke_fail "blur node must cover the whole content"
    # 有标题栏时内容只圆下角（corner_radius=12），blur 节点得跟内容一个角，
    # 否则圆角窗口的模糊会在下沿漏出直角。
    echo "$SCENE" | jq -e '[.. | objects | select(.type == "blur")
        | select(.corners == [0, 0, 12, 12])] | length >= 1' >/dev/null \
        || smoke_fail "blur node must follow the content corners"
else
    [ "$blur_nodes" = "0" ] || smoke_fail "non-effects build must not create blur nodes"
fi
stop_probe

echo "== 局部区域：只糊指定的一块 =="
scenario partial blur 50 40 200 100
expect '.views[0].bg_effect.blur == true' "partial blur must be on"
expect '.views[0].bg_effect.region == [50, 40, 200, 100]' "region must be recorded as-is"
if [ "$EFFECTS" = "true" ]; then
    PART_SCENE="$("$WCTL" -s "$SOCK" dump_scene)" || smoke_fail "dump_scene failed"
    echo "$PART_SCENE" | jq -e '[.. | objects | select(.type == "blur")
        | select(.w == 200 and .h == 100 and .x == 50 and .y == 40)] | length == 1' >/dev/null \
        || smoke_fail "partial blur node must be positioned at 50,40 with size 200x100"
    echo "$PART_SCENE" | jq -e '[.. | objects | select(.type == "optimized_blur")] | length >= 1' >/dev/null \
        || smoke_fail "optimized blur node must exist"
fi
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

echo "== layer surface：面板/Dock 也能声明模糊 =="
# 这条路径没有 view 对象：合成器得按 surface 认出 layer surface，把 blur 节点
# 挂到它自己的场景树里。用 dump_scene 断言"到底建没建"，因为 get_tree 只列顶层窗口。
LAYER_OUT="$XDG_RUNTIME_DIR/probe.layer.log"
"$PROBE" --layer blur 0 0 400 300 --hold 3000 >"$LAYER_OUT" 2>&1 &
probe_pid=$!
i=0
while [ "$i" -lt 120 ]; do
    grep -q '^READY' "$LAYER_OUT" 2>/dev/null && break
    if ! kill -0 "$probe_pid" 2>/dev/null; then
        echo "--- layer probe output:" >&2
        cat "$LAYER_OUT" >&2 || true
        smoke_fail "layer probe exited before READY"
    fi
    sleep 0.05; i=$((i+1))
done
grep -q '^READY' "$LAYER_OUT" 2>/dev/null || smoke_fail "layer probe never became ready"
[ "$(view_count)" = "0" ] || smoke_fail "layer probe must not create a toplevel view"
LAYER_SCENE="$("$WCTL" -s "$SOCK" dump_scene)" || smoke_fail "dump_scene failed (layer)"
layer_blur="$(echo "$LAYER_SCENE" | jq '[.. | objects | select(.type == "blur")] | length')"
if [ "$EFFECTS" = "true" ]; then
    [ "$layer_blur" = "1" ] || smoke_fail "layer blur must create exactly one blur node (got $layer_blur)"
    echo "$LAYER_SCENE" | jq -e '[.. | objects | select(.type == "blur")
        | select(.w == 400 and .h == 300 and .corners == [0, 0, 0, 0])] | length == 1' >/dev/null \
        || smoke_fail "layer blur node must cover the surface with square corners"
else
    [ "$layer_blur" = "0" ] || smoke_fail "non-effects build must not create blur nodes"
fi
kill -TERM "$probe_pid" 2>/dev/null || true
wait "$probe_pid" 2>/dev/null || true
probe_pid=""
# surface 没了，blur 节点必须跟着走（挂在 layer 的场景树上，不该留下悬空节点）
i=0
layer_blur="?"
while [ "$i" -lt 100 ]; do
    layer_blur="$("$WCTL" -s "$SOCK" dump_scene 2>/dev/null \
        | jq '[.. | objects | select(.type == "blur")] | length')" || layer_blur="?"
    [ "$layer_blur" = "0" ] && break
    sleep 0.05; i=$((i+1))
done
[ "$layer_blur" = "0" ] || smoke_fail "layer blur node must go away with its surface"

echo "== 决策轨迹与不变量 =="
"$WCTL" -s "$SOCK" get_trace | jq -e '
    [.entries[] | select(.what == "bg_effect")] | length >= 3' >/dev/null \
    || smoke_fail "bg_effect trace entries missing"
"$WCTL" -s "$SOCK" self_check | jq -e '.ok == true' >/dev/null \
    || smoke_fail "self_check failed after background effect changes"

# ---------------------------------------------------------------------------
# 第二遍：同一份配置面，只把 [effects] enabled 关掉。
# 这才是"运行期禁用"的验收点：协议全局还在、客户端设区域不报错、状态照记，
# 但 capability 归零、场景树里一个 blur 节点都没有。
# ---------------------------------------------------------------------------
if [ "$EFFECTS" = "true" ]; then
    echo "== 关掉特效：能力位归零、不建节点、状态照记 =="
    stop_compositor
    start_compositor false disabled

    "$WCTL" -s "$SOCK" get_tree | jq -e '.effects == false' >/dev/null \
        || smoke_fail "get_tree must report effects off"
    "$WCTL" -s "$SOCK" get_tree | jq -e '.effects_blur == false' >/dev/null \
        || smoke_fail "get_tree must report blur off"
    check_caps 0 "effects disabled must not advertise blur"

    # 客户端照样能设区域（协议对象还在），合成器也照样记下来——只是不画。
    scenario whole-off blur 0 0 400 300
    expect '.views[0].bg_effect.blur == true' "region must still be recorded when effects are off"
    expect '.views[0].bg_effect.region == [0, 0, 400, 300]' "region must still be recorded verbatim"
    OFF_SCENE="$("$WCTL" -s "$SOCK" dump_scene)" || smoke_fail "dump_scene failed (disabled)"
    off_nodes="$(echo "$OFF_SCENE" | jq '[.. | objects | select(.type == "blur")] | length')"
    [ "$off_nodes" = "0" ] || smoke_fail "effects off must not create blur nodes (got $off_nodes)"
    stop_probe
fi

echo "== surface 销毁后设 blur region：surface_destroyed 协议错误 =="
INERT_OUT="$XDG_RUNTIME_DIR/probe.inert.log"
rc=0
"$PROBE" inert-error >"$INERT_OUT" 2>&1 || rc=$?
[ "$rc" = 3 ] || {
    echo "--- probe output (inert-error):" >&2
    cat "$INERT_OUT" >&2 || true
    smoke_fail "inert-error probe expected rc=3, got $rc"
}
grep -q 'PROTOCOL_ERROR 0 ext_background_effect_surface_v1' "$INERT_OUT" \
    || smoke_fail "expected PROTOCOL_ERROR 0 (surface_destroyed), got: $(cat "$INERT_OUT")"

echo "bg-effect smoke passed"
