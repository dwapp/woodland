#!/usr/bin/env sh
# Alt+Tab 窗口切换器冒烟：起 headless 合成器，跑一个"三个窗口 + 虚拟键盘"的探针，
# 看面板状态与焦点是否真的跟着按键走（含最小化窗口进列表 / 提交时恢复）。
#
# 断言在探针里（get_focus / get_tree 的子串判断），脚本只做三件事：
#   1. 起停合成器并把现场落到 meson-logs/smoke/switcher/；
#   2. 判探针退出码（探针自己会打印每条 ok / FAIL）；
#   3. 补两条只有脚本才好查的：决策轨迹里成对出现 begin/commit、不变量自检干净。
#
# 用法: sh run_switcher_smoke.sh <woodland 二进制> <switcher_probe> <wctl>
set -eu

. "$(dirname "$0")/lib.sh"
smoke_init switcher

BIN="${1:-./build/src/woodland}"
PROBE="${2:-./build/tests/switcher_probe}"
WCTL="${3:-./build/src/wctl/wctl}"
[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 125; }
[ -x "$PROBE" ] || { echo "probe not built: $PROBE" >&2; exit 125; }
[ -x "$WCTL" ] || { echo "wctl not found: $WCTL" >&2; exit 125; }
command -v jq >/dev/null 2>&1 || { echo "jq not found, skip switcher smoke" >&2; exit 77; }

unset WAYLAND_DISPLAY
export XDG_RUNTIME_DIR="$(mktemp -d)"
# 面板样式（圆角/字体/配色）取自配置，别让 ~/.config 里的用户配置把用例搞成环境相关
export XDG_CONFIG_HOME="$XDG_RUNTIME_DIR/config"
export HOME="$XDG_RUNTIME_DIR"
mkdir -p "$XDG_CONFIG_HOME"
# WLR_RENDERER=pixman 只在 -Deffects=disabled 的构建里生效（effects 构建由
# fx_renderer_create 自己找节点），作用是把软件渲染钉死，面板颜色/圆角与 GPU 无关
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1

SMOKE_LOG="$XDG_RUNTIME_DIR/woodland.log"
comp_pid=""
SOCK=""

smoke_cleanup() {
    # 探针的输出是"它断言了什么、哪条没过"的唯一记录，跟着现场一起留
    if [ -n "${PROBE_LOG:-}" ] && [ -f "$PROBE_LOG" ] && [ -d "${SMOKE_DIR:-}" ]; then
        cp "$PROBE_LOG" "$SMOKE_DIR/probe.log" 2>/dev/null || :
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

# 探针自己连调试 socket：路径经环境变量传（合成器只对自己和子进程导出
# WOODLAND_IPC，脚本这边得显式给）
PROBE_LOG="$XDG_RUNTIME_DIR/probe.log"
rc=0
WOODLAND_IPC="$SOCK" "$PROBE" >"$PROBE_LOG" 2>&1 || rc=$?
if [ "$rc" -ne 0 ]; then
    echo "--- probe output:" >&2
    cat "$PROBE_LOG" >&2 || true
    smoke_fail "switch probe failed (rc=$rc)"
fi

# 四轮 Alt+Tab：三窗口，前三轮各 3 个候选（第三轮面板开着时最小化高亮那个），
# 第四轮开面板前先最小化一个非焦点窗口。
# 三窗口是故意的：两个窗口时正反切是同一件事，方向弄反了也看不出来。
# 轨迹是唯一能证明"最小化只改了标记、没摘掉那一行"的地方——面板计数不变，
# 变的是 min n=3 和提交时的 restore。
echo "== 决策轨迹：4 次 begin / 4 次 commit / 2 次 restore / 1 次 min / 0 次 drop =="
TRACE="$("$WCTL" -s "$SOCK" get_trace)" || smoke_fail "get_trace failed"

# detail 只取前几个字判断（begin 后面还带 n=/dir=/sel=）
trace_count() {
    echo "$TRACE" | jq --arg p "$1" \
        '[.entries[] | select(.what == "switcher" and (.detail | startswith($p)))] | length'
}
expect_trace() {
    got=$(trace_count "$1")
    if [ "$got" -ne "$2" ]; then
        echo "$TRACE" | jq '[.entries[] | select(.what == "switcher") | {obj, detail}]' >&2 || true
        smoke_fail "expected $2 switcher trace(s) matching '$1*', got $got"
    fi
}

expect_trace "begin n=3" 4
expect_trace "commit" 4
# 两次提交的目标都是最小化窗口：一次是面板开着时被最小化，一次是开面板前就最小化
expect_trace "restore" 2
# 第三轮在面板开着时把高亮项最小化：标记翻转一次，那一行必须留下。
# （"客户端自己取消最小化"验不了：仓库的 xdg-shell XML 是 unstable v6，
#  没有 unset_minimized，那条分支只有单测覆盖。）
expect_trace "min n=3" 1
# 一个都不该摘：最小化的候选要留在列表里（Alt+Tab 是它们唯一的回程票）
[ "$(trace_count "drop")" -eq 0 ] || smoke_fail "minimizing must not drop the row from the snapshot"

echo "== 监听器接线 =="
# 三个信号缺一个都会让某条生命周期路径失效（unmap / 最小化 / 锁屏），
# 而面板外观上完全看不出来，所以在这里点名核对一次。
"$WCTL" -s "$SOCK" get_listeners | jq -e '
    [.listeners[] | select(.name | startswith("switcher.")) | .name] | sort
    == ["switcher.changed", "switcher.lock", "switcher.unmapped"]
' >/dev/null || smoke_fail "switcher listeners are not all wired"

echo "== 不变量自检 =="
"$WCTL" -s "$SOCK" self_check | jq -e '.ok == true' >/dev/null \
    || smoke_fail "self_check failed after switching windows"

echo "== 面板收起后不留节点 =="
"$WCTL" -s "$SOCK" get_tree | jq -e '.window_switcher.active == false' >/dev/null \
    || smoke_fail "switcher must be inactive once Alt is released"

echo "switcher smoke passed"
