#!/usr/bin/env sh
# 多任务视图冒烟：使用真实 Wayland 客户端（simple_ssd）验证离屏缩略图渲染路径
# （wlr_renderer_begin_buffer_pass + mv_render_scene_node，覆盖 fx_renderer / pixman）、
# 场景树状态、工作区切换以及 self_check 不变量。
#
# 用法: sh run_multitask_smoke.sh <woodland> <simple_ssd> <action_probe> <wctl>
set -eu

. "$(dirname "$0")/lib.sh"
smoke_init multitask

BIN="${1:-./build/src/woodland}"
CLIENT="${2:-./build/tests/simple_ssd}"
PROBE="${3:-./build/tests/action_probe}"
WCTL="${4:-./build/src/wctl/wctl}"

[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 125; }
[ -x "$CLIENT" ] || { echo "client not found: $CLIENT" >&2; exit 125; }
[ -x "$PROBE" ] || { echo "action_probe not found: $PROBE" >&2; exit 125; }
[ -x "$WCTL" ] || { echo "wctl not found: $WCTL" >&2; exit 125; }
command -v jq >/dev/null 2>&1 || { echo "jq not found, skip multitask smoke" >&2; exit 77; }

unset WAYLAND_DISPLAY
export XDG_RUNTIME_DIR="$(mktemp -d)"
export XDG_CONFIG_HOME="$XDG_RUNTIME_DIR/config"
export XDG_DATA_HOME="$XDG_RUNTIME_DIR/data"
mkdir -p "$XDG_CONFIG_HOME/woodland" "$XDG_DATA_HOME"
CFG="$XDG_CONFIG_HOME/woodland/config.toml"
cat >"$CFG" <<'EOF'
[effects]
enabled = true
blur = true
EOF

export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1
SMOKE_LOG="$XDG_RUNTIME_DIR/woodland.log"

smoke_cleanup() {
    smoke_stop_compositor
    rm -rf "$XDG_RUNTIME_DIR"
}

smoke_start_compositor --self-check --trace -c "$CFG" -s "$CLIENT"

wctl_cmd() {
    "$WCTL" -s "$SOCK" -t 1000 "$1"
}

# 等真实客户端窗口完成 map 与首帧提交
"$WCTL" -s "$SOCK" -t 5000 wait view_mapped >/dev/null || smoke_fail "client never mapped"
TREE="$(wctl_cmd get_tree)" || smoke_fail "get_tree failed after client map"
printf '%s' "$TREE" | jq -e '.count == 1 and .multitask_view.active == false' >/dev/null \
    || smoke_fail "unexpected initial state: $TREE"

echo "== 打开多任务视图（含真实客户表面离屏缩略图与帧提交）=="
"$PROBE" --trigger 16 || smoke_fail "open_multitask_view trigger failed"
"$WCTL" -s "$SOCK" -t 2000 wait multitask active=1 card_count=1 thumb_count=1 >/dev/null \
    || smoke_fail "multitask_view did not activate"
# 经 pixel 同步触发一次真实离屏缩略图渲染与帧提交
"$WCTL" -s "$SOCK" pixel 100 100 >/dev/null || smoke_fail "pixel render failed during multitask_view"
kill -0 "$SMOKE_COMP_PID" 2>/dev/null || smoke_fail "compositor crashed while rendering multitask_view"
TREE="$(wctl_cmd get_tree)" || smoke_fail "get_tree failed after open_multitask_view"
printf '%s' "$TREE" | jq -e \
    '.multitask_view.active == true
     and .multitask_view.workspace == "1"
     and .multitask_view.card_count == 1
     and .multitask_view.thumb_count == 1
     and .multitask_view.dropped_cards == 0
     and .multitask_view.dropped_workspaces == 0
     and .multitask_view.selected_index == 0
     and .multitask_view.selected_view == 1' >/dev/null \
    || smoke_fail "multitask_view did not render live thumbnail for mapped client: $TREE"

# 校验场景树健康且无不变量破坏
SCENE="$(wctl_cmd dump_scene)" || smoke_fail "dump_scene failed while multitask_view active"
printf '%s' "$SCENE" | jq -e '.ok == true and .nodes > 0' >/dev/null \
    || smoke_fail "invalid scene graph while multitask_view active: $SCENE"

CHECK="$(wctl_cmd self_check)" || smoke_fail "self_check failed while multitask_view active"
printf '%s' "$CHECK" | jq -e '.ok == true and .failures == 0' >/dev/null \
    || smoke_fail "self_check broken while multitask_view active: $CHECK"

echo "== 关闭多任务视图 =="
"$PROBE" --trigger 17 || smoke_fail "close_multitask_view trigger failed"
TREE="$(wctl_cmd get_tree)" || smoke_fail "get_tree failed after close_multitask_view"
printf '%s' "$TREE" | jq -e '.multitask_view.active == false' >/dev/null \
    || smoke_fail "multitask_view did not close: $TREE"

echo "== 关掉运行期特效再打开（降级路径 + 实时缩略图）=="
cat >"$CFG" <<'EOF'
[effects]
enabled = false
EOF
wctl_cmd reload_config >/dev/null || smoke_fail "reload_config failed"
"$PROBE" --trigger 16 || smoke_fail "open_multitask_view (effects off) failed"
"$WCTL" -s "$SOCK" -t 2000 wait multitask active=1 card_count=1 thumb_count=1 >/dev/null \
    || smoke_fail "multitask_view (effects off) did not activate"
"$WCTL" -s "$SOCK" pixel 100 100 >/dev/null || smoke_fail "pixel render failed (effects off)"
kill -0 "$SMOKE_COMP_PID" 2>/dev/null || smoke_fail "compositor crashed with effects disabled"
TREE="$(wctl_cmd get_tree)" || smoke_fail "get_tree failed (effects off)"
printf '%s' "$TREE" | jq -e \
    '.effects == false
     and .multitask_view.active == true
     and .multitask_view.card_count == 1
     and .multitask_view.thumb_count == 1' >/dev/null \
    || smoke_fail "multitask_view degraded path failed: $TREE"
"$PROBE" --trigger 17 || smoke_fail "close_multitask_view (effects off) failed"

echo "== 切到空工作区再 toggle =="
"$PROBE" --trigger 2 || smoke_fail "workspace_2 trigger failed"
"$PROBE" --trigger 18 || smoke_fail "toggle_multitask_view(open) failed"
TREE="$(wctl_cmd get_tree)" || smoke_fail "get_tree failed on empty workspace"
printf '%s' "$TREE" | jq -e \
    '.multitask_view.active == true
     and .multitask_view.workspace == "2"
     and .multitask_view.card_count == 0
     and .multitask_view.thumb_count == 0' >/dev/null \
    || smoke_fail "unexpected empty workspace multitask state: $TREE"

"$PROBE" --trigger 18 || smoke_fail "toggle_multitask_view(close) failed"
TREE="$(wctl_cmd get_tree)" || smoke_fail "get_tree failed after toggle close"
printf '%s' "$TREE" | jq -e '.multitask_view.active == false' >/dev/null \
    || smoke_fail "multitask_view did not toggle close: $TREE"

CHECK="$(wctl_cmd self_check)" || smoke_fail "final self_check failed"
printf '%s' "$CHECK" | jq -e '.ok == true and .failures == 0' >/dev/null \
    || smoke_fail "final self_check broken: $CHECK"

if grep -q 'self-check .* failed' "$SMOKE_LOG"; then
    smoke_fail "compositor logged a self-check failure"
fi

echo "multitask smoke passed"
