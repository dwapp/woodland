#!/usr/bin/env sh
# 冒烟测试的公共外壳：失败时把现场落盘，而不是只吐一行断言。
#
# 为什么需要：断言失败的那一行（"expected 2 replies, got 0"）只能说明结果，
# 说明不了原因。上一次 nc 假失败就是被这行误导的。有现场之后，排查的人
# （或 AI）拿到的是合成器日志 + 那一刻的完整状态 + 决策轨迹。
#
# 用法：
#   . "$(dirname "$0")/lib.sh"     # 或显式路径
#   smoke_init ipc                 # 装 trap、确定落盘目录
#   smoke_cleanup() { ... }        # 可选：自己的收尾，dump 之后才调用
#
# 脚本自己设置、lib 只读的变量（有就收集，没有就跳过）：
#   BIN          合成器二进制
#   WCTL         wctl 二进制（有它才能收集 JSON 状态）
#   SOCK         调试 socket 路径
#   SMOKE_LOG    合成器日志文件
#   SMOKE_FLAGS  启动参数（写进 meta.txt，方便复现）
#
# 落盘位置：$MESON_BUILD_ROOT/meson-logs/smoke/<名字>/（不在 meson 下跑时
# 退化成 ./meson-logs/smoke/<名字>/）。meson 的 testlog.txt 就在旁边，
# 一次失败的产物集中在一处，不用去 /tmp 里翻已经被删掉的目录。

SMOKE_DUMPED=0
SMOKE_EXITING=0

# 脚本可以覆盖它；默认什么也不做
smoke_cleanup() {
    :
}

smoke_init() {
    SMOKE_NAME="${1:-smoke}"
    # 冒烟测试只针对合成器本身：默认不拉起 woodland-shortcutd。它会 acquire
    # 全局快捷键管理器（协议只允许一个持有者）并把默认键位注册进去，
    # "binding_count == 0"、occupied、last_outcome 这些断言全被它带偏。
    # 专测自动启动的 run_shortcutd_smoke.sh 会自己覆盖这个变量。
    : "${WOODLAND_SHORTCUTD:=off}"
    export WOODLAND_SHORTCUTD
    if [ -n "${MESON_BUILD_ROOT:-}" ]; then
        SMOKE_DIR="$MESON_BUILD_ROOT/meson-logs/smoke/$SMOKE_NAME"
    else
        SMOKE_DIR="$PWD/meson-logs/smoke/$SMOKE_NAME"
    fi
    # 上一轮的产物必须清掉，否则会把旧现场当成这次的
    rm -rf "$SMOKE_DIR"
    trap '_smoke_exit $?' EXIT
    trap '_smoke_exit 130' INT
    trap '_smoke_exit 143' TERM
}

_smoke_exit() {
    _rc="${1:-0}"
    # INT/TERM 之后还会触发 EXIT，只处理一次
    [ "$SMOKE_EXITING" = 1 ] && return 0
    SMOKE_EXITING=1
    if [ "$_rc" -ne 0 ]; then
        smoke_dump "$_rc"
    fi
    smoke_cleanup
    exit "$_rc"
}

# 从 ipc socket 名反推 wayland display（woodland-wayland-0.sock → wayland-0），
# grim 需要它；拿不到就算了，截图是加分项不是必需项
_smoke_wayland_display() {
    [ -n "${SOCK:-}" ] || return 1
    _base="${SOCK##*/}"
    _base="${_base#woodland-}"
    _base="${_base%.sock}"
    [ -n "$_base" ] || return 1
    printf '%s' "$_base"
}

smoke_dump() {
    _rc="${1:-?}"
    [ "$SMOKE_DUMPED" = 1 ] && return 0
    SMOKE_DUMPED=1
    mkdir -p "$SMOKE_DIR" 2>/dev/null || return 0

    {
        echo "test: $SMOKE_NAME"
        echo "exit: $_rc"
        echo "when: $(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || echo '-')"
        echo "kernel: $(uname -srm 2>/dev/null || echo '-')"
        echo "bin: ${BIN:-'-'}"
        echo "flags: ${SMOKE_FLAGS:-'-'}"
        echo "wctl: ${WCTL:-'-'}"
        echo "socket: ${SOCK:-'-'}"
        echo "log: ${SMOKE_LOG:-'-'}"
    } >"$SMOKE_DIR/meta.txt" 2>/dev/null

    if [ -n "${SMOKE_LOG:-}" ] && [ -f "$SMOKE_LOG" ]; then
        cp "$SMOKE_LOG" "$SMOKE_DIR/compositor.log" 2>/dev/null || :
    fi

    # 状态快照：只读命令，对已经"半坏"的合成器也安全（失败就留个 .err）
    if [ -n "${WCTL:-}" ] && [ -x "${WCTL:-}" ] && [ -n "${SOCK:-}" ] \
            && [ -S "$SOCK" ]; then
        for _cmd in get_tree get_focus self_check get_trace get_listeners \
            dump_scene; do
            "$WCTL" -s "$SOCK" -t 1000 "$_cmd" \
                >"$SMOKE_DIR/$_cmd.json" 2>"$SMOKE_DIR/$_cmd.err" || :
        done
    fi

    if command -v grim >/dev/null 2>&1; then
        if _display="$(_smoke_wayland_display)"; then
            WAYLAND_DISPLAY="$_display" grim "$SMOKE_DIR/screenshot.png" \
                2>"$SMOKE_DIR/screenshot.err" || :
        fi
    fi

    echo "--- 现场已保存到 $SMOKE_DIR" >&2
}

# 断言失败的统一出口：一行结论 + 日志尾巴 + 现场目录
smoke_fail() {
    echo "FAIL: $*" >&2
    if [ -n "${SMOKE_LOG:-}" ] && [ -f "$SMOKE_LOG" ]; then
        echo "--- compositor log (tail) ---" >&2
        tail -40 "$SMOKE_LOG" >&2 || :
    fi
    smoke_dump 1
    exit 1
}

# 等调试 socket 出现；成功时设置 SOCK。找不到就 smoke_fail。
# 脚本可以自己实现（各脚本的超时不同），这不是强制的。
smoke_wait_socket() {
    _tries="${1:-100}"
    _dir="${XDG_RUNTIME_DIR:-}"
    _i=0
    while [ "$_i" -lt "$_tries" ]; do
        for _f in "$_dir"/woodland-*.sock; do
            [ -S "$_f" ] && SOCK="$_f"
        done
        [ -n "${SOCK:-}" ] && return 0
        sleep 0.05
        _i=$((_i + 1))
    done
    return 1
}
