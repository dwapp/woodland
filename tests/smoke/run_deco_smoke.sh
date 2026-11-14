#!/usr/bin/env sh
# treeland-decoration-unstable-v1 冒烟：起 headless 合成器 + 真客户端，然后用
# wctl 断言 get_tree 里的 deco / ssd.margins。
#
# 断言的是**状态**而不是像素：标题栏隐藏后边距是不是只剩边框、边框宽度有没有
# 进边距、第二份 context 有没有被打成 already_used——这些用 jq 一句话就说清，
# 截图只能回答"看起来像不像"。
#
# 用法: sh run_deco_smoke.sh <woodland 二进制> <deco_probe> <wctl>
set -eu

. "$(dirname "$0")/lib.sh"
smoke_init decoration

BIN="${1:-./build/src/woodland}"
PROBE="${2:-./build/tests/deco_probe}"
WCTL="${3:-./build/src/wctl/wctl}"
[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 125; }
[ -x "$PROBE" ] || { echo "probe not built: $PROBE" >&2; exit 125; }
[ -x "$WCTL" ] || { echo "wctl not found: $WCTL" >&2; exit 125; }
command -v jq >/dev/null 2>&1 || { echo "jq not found, skip decoration smoke" >&2; exit 77; }

unset WAYLAND_DISPLAY
export XDG_RUNTIME_DIR="$(mktemp -d)"
# 断言的基准是内置默认外观（32 标题栏 + 1 边框），别让 ~/.config 里的
# 用户配置把它改掉
export XDG_CONFIG_HOME="$XDG_RUNTIME_DIR/config"
export HOME="$XDG_RUNTIME_DIR"
mkdir -p "$XDG_CONFIG_HOME"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1

SMOKE_LOG="$XDG_RUNTIME_DIR/woodland.log"
SMOKE_FLAGS="--self-check --trace"
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


# 圆角现在总是有渲染后端（scenefx 常开），能不能画出来取决于 [effects] enabled；
# 默认配置是开的，所以这里的期望值写死"画"。"关掉后不长角"由 smoke-bg-effect
# 的禁用场景覆盖。

view_count() {
    "$WCTL" -s "$SOCK" get_tree 2>/dev/null | jq -r '.count // -1'
}

# 等到视图数回到 0（上一个场景的窗口撤干净）
wait_view_count() {
    _want="$1"
    _i=0
    while [ "$_i" -lt 100 ]; do
        [ "$(view_count)" = "$_want" ] && return 0
        sleep 0.05; _i=$((_i+1))
    done
    return 1
}

# 起一个探针并等它就绪，然后把这一刻的 get_tree 抓进 $TREE
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

# expect <jq 表达式> <说明>：表达式在当前 $TREE 上求值
expect() {
    echo "$TREE" | jq -e "$1" >/dev/null || {
        echo "--- get_tree:" >&2
        echo "$TREE" | jq '.views[0] | {deco, ssd, outer, content}' >&2 || true
        smoke_fail "$2"
    }
}

echo "== 默认：没有任何 decoration context =="
scenario default --no-context
expect '.views[0].deco.managed == false' "no context yet, managed must be false"
expect '.views[0].deco.rendered == []' "nothing overridden, rendered must be empty"
# 内置默认：1px 边框 + 32px 标题栏
expect '.views[0].ssd.margins == [1, 33, 1, 1]' "default margins must be border+titlebar"
stop_probe

echo "== set_border：宽度进边距，颜色记录 =="
scenario border border 5 255 0 0 255
expect '.views[0].deco.managed == true' "context must mark the view as managed"
expect '.views[0].deco.border_width == 5' "border width must be 5"
expect '.views[0].deco.border_rgba == [255, 0, 0, 255]' "border color must be recorded"
expect '.views[0].deco.rendered == ["border"]' "border must be rendered"
expect '.views[0].ssd.margins == [5, 37, 5, 5]' "margins must follow the new border width"
stop_probe

echo "== set_titlebar_mode(hide)：边距只剩边框 =="
scenario hide titlebar 1
expect '.views[0].deco.titlebar == "hidden"' "titlebar must be hidden"
expect '.views[0].deco.rendered == ["titlebar"]' "titlebar must be rendered"
expect '.views[0].ssd.margins == [1, 1, 1, 1]' "hidden titlebar leaves only the border"
stop_probe

echo "== 半 CSD 半 SSD：隐藏标题栏 + 加粗边框 =="
scenario half border 8 0 0 0 255 titlebar 1
expect '.views[0].ssd.margins == [8, 8, 8, 8]' "no titlebar, 8px border all around"
expect '.views[0].deco.border_width == 8' "border width must be 8"
stop_probe

echo "== border 0 + hide：完全没有装饰 =="
scenario bare border 0 0 0 0 0 titlebar 1
expect '.views[0].ssd.margins == [0, 0, 0, 0]' "zero border + hidden titlebar means no decoration"
stop_probe

echo "== 圆角/阴影：圆角与阴影均有渲染后端 =="
scenario effect radius 12 shadow 40 0 10 0 0 0 102
expect '.views[0].deco.corner_radius == 12' "corner radius must be recorded"
expect '.views[0].deco.shadow_radius == 40' "shadow radius must be recorded"
expect '.views[0].deco.shadow_offset == [0, 10]' "shadow offset must be recorded"
if [ "$EFFECTS" = "true" ]; then
    expect '.views[0].deco.rendered == ["corner_radius", "shadow"]' "corner radius and shadow have render backends"
    # 圆角必须落在一整圈描边上：frame 外圆角 R+b，clipped_region 挖空内框且内圆角
    # R。旧实现是四条 1px 细条，半径会被细条尺寸 clamp 掉、角上只剩直角——钉住
    # 不会再退回去。R=12，b=1，内容 400x300，标题栏 32。
    ROUND_SCENE="$("$WCTL" -s "$SOCK" dump_scene)" || smoke_fail "dump_scene failed (rounded frame)"
    echo "$ROUND_SCENE" | jq -e '
        [.. | objects | select(.type == "rect" and .corners == [13,13,13,13])] as $f
        | ($f | length) == 1
          and ($f[0].clip.corners == [12,12,12,12])
          and ($f[0].clip.x == 1 and $f[0].clip.y == 1)
          and ($f[0].clip.w == 400 and $f[0].clip.h == 332)
          and ($f[0].w == 402 and $f[0].h == 334)
    ' >/dev/null || smoke_fail "radius must produce one stroked frame (outer R+b, inner R)"
    echo "$ROUND_SCENE" | jq -e '
        [.. | objects | select(.type == "rect" and (.w == 1 or .h == 1))] | length == 0
    ' >/dev/null || smoke_fail "no 1px border strips may remain outside the rounded frame"
    echo "$ROUND_SCENE" | jq -e '
        [.. | objects | select(.type == "shadow" and .blur_sigma == 40)] | length == 1
    ' >/dev/null || smoke_fail "shadow node must be created with blur_sigma 40"
else
    expect '.views[0].deco.rendered == []' "radius/shadow have no render backend"
fi
stop_probe

echo "== context 销毁：边框恢复默认，标题栏保持隐藏 =="
OUT_REVERT="$XDG_RUNTIME_DIR/probe.revert.log"
"$PROBE" border 6 255 0 0 255 titlebar 1 --drop-context --hold 3000 >"$OUT_REVERT" 2>&1 &
probe_pid=$!
i=0
while [ "$i" -lt 120 ]; do
    grep -q '^REVERTED' "$OUT_REVERT" 2>/dev/null && break
    kill -0 "$probe_pid" 2>/dev/null || smoke_fail "probe died before REVERTED"
    sleep 0.05; i=$((i+1))
done
grep -q '^REVERTED' "$OUT_REVERT" 2>/dev/null || smoke_fail "context was never dropped"
TREE="$("$WCTL" -s "$SOCK" get_tree)" || smoke_fail "get_tree failed (revert)"
expect '.views[0].deco.managed == false' "dropped context must clear managed"
expect '.views[0].deco.border_width == null' "border override must be reverted"
expect '.views[0].deco.titlebar == "hidden"' "titlebar override outlives the context"
expect '.views[0].ssd.margins == [1, 1, 1, 1]' "default border back, titlebar still hidden"
stop_probe

echo "== 已有 context 时再建一个：already_used =="
DUP_OUT="$XDG_RUNTIME_DIR/probe.dup.log"
rc=0
"$PROBE" dup >"$DUP_OUT" 2>&1 || rc=$?
[ "$rc" = 3 ] || {
    echo "--- probe output (dup):" >&2
    cat "$DUP_OUT" >&2 || true
    smoke_fail "duplicate context must be rejected with a protocol error (rc=$rc)"
}
grep -q '^PROTOCOL_ERROR 0 treeland_decoration_manager_v1' "$DUP_OUT" \
    || smoke_fail "expected already_used (error 0) on the manager, got: $(cat "$DUP_OUT")"
wait_view_count 0 || smoke_fail "dup probe window did not go away"

echo "== 决策轨迹与不变量 =="
"$WCTL" -s "$SOCK" get_trace | jq -e '
    [.entries[] | select(.what == "deco")] | length >= 5' >/dev/null \
    || smoke_fail "deco trace entries missing"
if [ "$EFFECTS" = "true" ]; then
    "$WCTL" -s "$SOCK" get_trace | jq -e '
        [.entries[] | select(.what == "deco" and (.detail | contains("corner_radius"))
                             and (.detail | contains("effects disabled")))] | length == 0' >/dev/null \
        || smoke_fail "corner radius must not claim to be disabled"
    "$WCTL" -s "$SOCK" get_trace | jq -e '
        [.entries[] | select(.what == "deco" and (.detail | contains("shadow"))
                             and (.detail | contains("no render backend")))] | length == 0' >/dev/null \
        || smoke_fail "shadow must not claim no render backend when effects are enabled"
else
    "$WCTL" -s "$SOCK" get_trace | jq -e '
        [.entries[] | select(.what == "deco" and (.detail | contains("shadow"))
                             and (.detail | contains("no render backend")))] | length >= 1' >/dev/null \
        || smoke_fail "trace must spell out that the shadow is not rendered"
fi
"$WCTL" -s "$SOCK" self_check | jq -e '.ok == true' >/dev/null \
    || smoke_fail "self_check failed after decoration changes"
echo "== 窗口阴影：热重载启用 shadow =="
if [ "$EFFECTS" = "true" ]; then
    mkdir -p "$XDG_CONFIG_HOME/woodland"
    cat >"$XDG_CONFIG_HOME/woodland/config.toml" <<'EOF'
[effects]
enabled = true
shadow = true
shadow_radius = 20
shadow_offset_y = 6
EOF
    "$WCTL" -s "$SOCK" reload_config >/dev/null || smoke_fail "reload_config failed"
    scenario shadow_on radius 8
    SHADOW_SCENE="$("$WCTL" -s "$SOCK" dump_scene)" || smoke_fail "dump_scene failed (shadow)"
    echo "$SHADOW_SCENE" | jq -e '
        [.. | objects | select(.type == "shadow" and .blur_sigma == 20)] | length == 1
    ' >/dev/null || smoke_fail "shadow node must exist with blur_sigma 20 when shadow=true"
    stop_probe
fi

echo "== 窄窗口：三个按钮放不下时整组收起 =="
# 120px 宽放不下"图标 + 标题 + 3x32 按钮"，按钮必须整组不画——不收的话最左那个
# 会压在应用图标上，两个都点不准。布局照常摆位（节点还在），关不关由
# ssd_apply_state 决定，所以这里查的是节点的 enabled。
scenario narrow --size 120 200
NARROW_SCENE="$("$WCTL" -s "$SOCK" dump_scene)" || smoke_fail "dump_scene failed (narrow window)"
# 标题文字是 70x32、应用图标 20x20，按 32x32 筛出来的正好是那三个符号；
# 数量先卡成 3，万一字体把标题量成 32 宽也会在这里暴露，而不是悄悄放过。
echo "$NARROW_SCENE" | jq -e '
    [.. | objects | select(.type == "buffer" and .y < 0 and .w == 32 and .h == 32)] as $b
    | ($b | length) == 3 and ([$b[] | select(.enabled)] | length) == 0
' >/dev/null || smoke_fail "buttons must be hidden when the titlebar cannot fit them"
echo "$NARROW_SCENE" | jq -e '
    [.. | objects | select(.type == "buffer" and .y < 0 and .enabled)] | length > 0
' >/dev/null || smoke_fail "icon/title must still be drawn in a narrow titlebar"
stop_probe

echo "== 矮标题栏：按钮符号不溢出标题栏、不被压扁 =="
# 按钮宽度来自 [decorations] button_width，高度跟标题栏走。两者不等时必须取
# 短边并居中：旧实现只摆位、dest 停在创建时的 32x32，在 20px 标题栏里就从
# y=-20 一直画到 +12，等于压在客户端内容上；按 (bwn, bh) 设 dest 又会把
# □ / × 压扁。这里读场景树实测，不靠肉眼。
mkdir -p "$XDG_CONFIG_HOME/woodland"
cat >"$XDG_CONFIG_HOME/woodland/config.toml" <<'EOF'
[decorations]
titlebar_height = 20
EOF
"$WCTL" -s "$SOCK" reload_config >/dev/null || smoke_fail "reload_config failed (titlebar_height = 20)"
scenario short_titlebar
SHORT_SCENE="$("$WCTL" -s "$SOCK" dump_scene)" || smoke_fail "dump_scene failed (short titlebar)"
# 标题栏在负坐标上：任何 buffer 的"顶端 + 高度"越过 0 就是画进了内容区
echo "$SHORT_SCENE" | jq -e '
    [.. | objects | select(.type == "buffer" and .y < 0 and (.y + .h) > 0)] | length == 0
' >/dev/null || smoke_fail "button glyphs must not spill out of a 20px titlebar"
# 右侧三个按钮格子（宽度仍是 32）里的符号必须是方形，且不高于标题栏
echo "$SHORT_SCENE" | jq -e '
    [.. | objects | select(.type == "buffer" and .y < 0 and .x >= 300)] as $g
    | ($g | length) == 3 and ([$g[] | select(.w != .h or .w > 20)] | length) == 0
' >/dev/null || smoke_fail "button glyphs must stay square and fit a 20px titlebar"
stop_probe

echo "decoration smoke passed"
