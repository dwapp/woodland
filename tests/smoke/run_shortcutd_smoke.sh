#!/usr/bin/env sh
# woodland-shortcutd 冒烟：
#   1. 配置解析：--print-default-config 的输出能被自己回读（不连合成器）
#   2. 非 DDE 会话：合成器自动把它拉起来，绑定条数与配置一致
#   3. 第二个实例撞上 occupied：必须非零退出，而不是静默占位失败
#   4. DDE 会话（XDG_CURRENT_DESKTOP=Deepin）：合成器不启动它，快捷键留给桌面
#   4b. 只有 DESKTOP_SESSION=deepin：同样按 DDE 处理（会话名有几个来源）
#   5. 手动跑一个实例 + SIGHUP 重载：旧绑定要撤掉、新配置要生效、进程正常退出
#   6. 合成器退出：客户端以 0 退出（会话结束不是错误）
#
# 用法: sh run_shortcutd_smoke.sh <woodland> <woodland-shortcutd> <wctl>
set -eu

. "$(dirname "$0")/lib.sh"
smoke_init shortcutd

BIN="${1:-./build/src/woodland}"
SD="${2:-./build/src/shortcutd/woodland-shortcutd}"
WCTL="${3:-./build/src/wctl/wctl}"
[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 125; }
[ -x "$SD" ] || { echo "shortcutd not built: $SD" >&2; exit 125; }
if [ -z "$WCTL" ] || [ ! -x "$WCTL" ]; then
    echo "wctl not found: $WCTL" >&2
    exit 125
fi
command -v jq >/dev/null 2>&1 || { echo "jq is required" >&2; exit 125; }

BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"
SD="$(cd "$(dirname "$SD")" && pwd)/$(basename "$SD")"
WCTL="$(cd "$(dirname "$WCTL")" && pwd)/$(basename "$WCTL")"

unset WAYLAND_DISPLAY
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1

comp_pid=""
sd_pid=""
run_dir=""
LOG_DIR="$(mktemp -d)"
comp_count=0
cleanup_all() {
    if [ -n "$sd_pid" ]; then
        kill -TERM "$sd_pid" 2>/dev/null || true
        wait "$sd_pid" 2>/dev/null || true
        sd_pid=""
    fi
    if [ -n "$comp_pid" ]; then
        kill -TERM "$comp_pid" 2>/dev/null || true
        wait "$comp_pid" 2>/dev/null || true
        comp_pid=""
    fi
    [ -n "$run_dir" ] && rm -rf "$run_dir"
    run_dir=""
}
smoke_cleanup() {
    cleanup_all
    rm -rf "$LOG_DIR"
}

# 起一个干净的 headless 合成器；$1 = XDG_CURRENT_DESKTOP 取值，其余参数透传给合成器
start_compositor() {
    _desktop="$1"
    shift
    cleanup_all
    run_dir="$(mktemp -d)"
    comp_count=$((comp_count + 1))
    export XDG_RUNTIME_DIR="$run_dir"
    export XDG_CONFIG_HOME="$run_dir/config"
    export XDG_DATA_HOME="$run_dir/data"
    # 会话名还有另外两个来源（见 util/env.h:woodland_session_is_dde）。默认清掉，
    # 让本脚本的 "$1" 是唯一变量；要专测那两个变量时用 SD_SESSION_DESKTOP 传入。
    unset XDG_SESSION_DESKTOP DESKTOP_SESSION
    if [ -n "${SD_SESSION_DESKTOP:-}" ]; then
        export DESKTOP_SESSION="$SD_SESSION_DESKTOP"
    fi
    mkdir -p "$XDG_CONFIG_HOME/woodland" "$XDG_DATA_HOME"
    cat >"$XDG_CONFIG_HOME/woodland/shortcuts.toml" <<'EOF'
[general]
enabled = true

[[shortcut]]
key = "Ctrl+Alt+T"
command = "true"

[[shortcut]]
key = "Meta+Left"
action = "tile_left"

[[shortcut]]
key = "XF86AudioMute"
command = "true"
flags = "press|repeat"
EOF

    SMOKE_LOG="$LOG_DIR/compositor-$comp_count.log"
    export WOODLAND_SHORTCUTD="$SD"
    XDG_CURRENT_DESKTOP="$_desktop" "$BIN" --trace "$@" >"$SMOKE_LOG" 2>&1 &
    comp_pid=$!

    SOCK=""
    i=0
    while [ "$(ls "$run_dir" 2>/dev/null | grep -c '^wayland-')" -eq 0 ] \
            && [ "$i" -lt 100 ]; do
        sleep 0.05; i=$((i+1))
    done
    socket_name="$(ls "$run_dir" 2>/dev/null | grep '^wayland-' | head -1)"
    [ -n "$socket_name" ] || smoke_fail "wayland socket did not appear"
    export WAYLAND_DISPLAY="$socket_name"
    smoke_wait_socket 100 || smoke_fail "debug socket did not appear"
}

wctl_cmd() {
    "$WCTL" -s "$SOCK" -t 1000 "$1"
}

# 轮询到 jq 表达式成立为止（自动启动的客户端是异步连上来的）
wait_for() {
    _expr="$1"
    _tries="${2:-100}"
    _i=0
    while [ "$_i" -lt "$_tries" ]; do
        _out="$(wctl_cmd get_tree 2>/dev/null)" || _out=""
        if [ -n "$_out" ] && printf '%s' "$_out" | jq -e "$_expr" >/dev/null 2>&1; then
            printf '%s' "$_out"
            return 0
        fi
        sleep 0.05
        _i=$((_i + 1))
    done
    printf '%s' "${_out:-}"
    return 1
}

# ---- 1. 配置解析：导出的示例必须能被自己回读 ----
sample="$(mktemp)"
"$SD" --print-default-config >"$sample" || smoke_fail "--print-default-config failed"
[ -s "$sample" ] || smoke_fail "--print-default-config produced nothing"
"$SD" -c "$sample" --dry-run >/dev/null \
    || smoke_fail "the exported sample config was not parseable"
rm -f "$sample"

# ---- 2. 非 DDE 会话：自动启动 + 三条绑定 ----
start_compositor "woodland:wlroots"
_tree="$(wait_for '.shortcut_manager.acquired == true and .shortcut_manager.binding_count == 3')" \
    || smoke_fail "shortcutd was not autostarted (state: ${_tree:-none})"

printf '%s' "$_tree" | jq -e \
    '[.shortcut_manager.bindings[].key] | sort
     == ["Ctrl+Alt+T", "Meta+Left", "XF86AudioMute"]' >/dev/null \
    || smoke_fail "unexpected bindings: $(printf '%s' "$_tree" | jq -c '.shortcut_manager.bindings')"

# 配置里的 command 类绑定必须是 notify，action 类必须带上协议里的 action 号
printf '%s' "$_tree" | jq -e \
    '.shortcut_manager.bindings[]
     | select(.key == "Meta+Left")
     | .action == 32 and .flags == 1' >/dev/null \
    || smoke_fail "Meta+Left was not registered as tile_left(32)/press: $_tree"

printf '%s' "$_tree" | jq -e \
    '.shortcut_manager.bindings[]
     | select(.key == "XF86AudioMute")
     | .action == 0 and .flags == 5' >/dev/null \
    || smoke_fail "XF86AudioMute flags/action wrong: $_tree"

wctl_cmd get_trace | grep -q 'shortcut client: started' \
    || smoke_fail "trace did not record the shortcut client launch"

# ---- 3. 第二个实例：occupied，必须非零退出 ----
set +e
"$SD" -c "$XDG_CONFIG_HOME/woodland/shortcuts.toml" >"$run_dir/second.log" 2>&1
second_rc=$?
set -e
[ "$second_rc" -ne 0 ] \
    || smoke_fail "a second shortcutd should fail (occupied), but it exited 0"
grep -q 'occupied' "$run_dir/second.log" \
    || smoke_fail "second instance did not report occupied: $(cat "$run_dir/second.log")"

# ---- 4. DDE 会话：不自动启动 ----
start_compositor "Deepin"
sleep 0.8
_tree="$(wctl_cmd get_tree)" || smoke_fail "get_tree failed in the DDE session"
printf '%s' "$_tree" | jq -e \
    '.shortcut_manager.acquired == false and .shortcut_manager.binding_count == 0' >/dev/null \
    || smoke_fail "shortcutd must not start in a DDE session: $_tree"

wctl_cmd get_trace | grep -q 'shortcut client: skipped' \
    || smoke_fail "trace did not record why the shortcut client was skipped"

grep -q 'DDE session' "$SMOKE_LOG" \
    || smoke_fail "compositor log did not mention the DDE session"

# 4b. 只有 DESKTOP_SESSION=deepin（XDG_CURRENT_DESKTOP 不是 DDE）：同样不启动。
# 显示管理器的会话不一定把 XDG_CURRENT_DESKTOP 设全，漏判的代价是 dde-daemon
# 拿不到独占、整套 DDE 快捷键失效，所以三个变量都要看。
SD_SESSION_DESKTOP=deepin start_compositor "woodland:wlroots"
sleep 0.8
_tree="$(wctl_cmd get_tree)" || smoke_fail "get_tree failed in the legacy-name session"
printf '%s' "$_tree" | jq -e \
    '.shortcut_manager.acquired == false and .shortcut_manager.binding_count == 0' >/dev/null \
    || smoke_fail "DESKTOP_SESSION=deepin must also be treated as DDE: $_tree"
grep -q 'DESKTOP_SESSION=deepin' "$SMOKE_LOG" \
    || smoke_fail "compositor log did not name the deciding session variable"
unset SD_SESSION_DESKTOP

# ---- 5. 手动实例 + SIGHUP 重载 ----
# 这一段自己管 wsd 的生命周期：合成器用 --no-shortcut-client 关掉自动启动，
# 否则它先占了独占，手动这个就只能拿到 occupied。
start_compositor "KDE" --no-shortcut-client
_cfg="$XDG_CONFIG_HOME/woodland/shortcuts.toml"
"$SD" -v -c "$_cfg" >"$run_dir/sd.log" 2>&1 &
sd_pid=$!
_tree="$(wait_for '.shortcut_manager.acquired == true and .shortcut_manager.binding_count == 3')" \
    || smoke_fail "manually started shortcutd did not register: ${_tree:-none}"

cat >"$_cfg" <<'EOF'
[general]
enabled = true

[[shortcut]]
key = "Meta+Q"
action = "close_window"

[[shortcut]]
key = "Ctrl+Alt+K"
command = "true"
EOF
kill -HUP "$sd_pid"
_tree="$(wait_for '.shortcut_manager.binding_count == 2')" \
    || smoke_fail "SIGHUP did not reload the bindings: ${_tree:-none}"
printf '%s' "$_tree" | jq -e \
    '[.shortcut_manager.bindings[].key] | sort == ["Ctrl+Alt+K", "Meta+Q"]' >/dev/null \
    || smoke_fail "after reload the old bindings are still there: $_tree"

kill -TERM "$sd_pid"
set +e
wait "$sd_pid"
sd_rc=$?
set -e
sd_pid=""
[ "$sd_rc" -eq 0 ] || smoke_fail "shortcutd should exit 0 on SIGTERM, got $sd_rc"

# 退出后独占要放开，否则下一个会话永远拿不到 manager
_tree="$(wait_for '.shortcut_manager.acquired == false')" \
    || smoke_fail "manager was not released after shortcutd exited: ${_tree:-none}"

# ---- 6. 合成器退出：要当"会话结束"退出 0，不是报连接错误 ----
# 合成器关 socket 时 libwayland 把 EOF 报成 EPIPE。以前这条路径返回 1，
# 于是每次正常结束会话都会在日志里留一行"连接断开"，按退出码判断健康状态的
# 东西（systemd、启动脚本）会把正常收尾当崩溃。
start_compositor "KDE" --no-shortcut-client
"$SD" -c "$XDG_CONFIG_HOME/woodland/shortcuts.toml" >"$run_dir/sd-exit.log" 2>&1 &
sd_pid=$!
wait_for '.shortcut_manager.acquired == true' >/dev/null \
    || smoke_fail "shortcutd did not acquire before the compositor shutdown"
kill -TERM "$comp_pid"
set +e
wait "$comp_pid"
comp_pid=""
wait "$sd_pid"
sd_rc=$?
set -e
sd_pid=""
[ "$sd_rc" -eq 0 ] \
    || smoke_fail "shortcutd should exit 0 when the compositor goes away, got $sd_rc: $(cat "$run_dir/sd-exit.log")"

echo "shortcutd smoke: ok"
