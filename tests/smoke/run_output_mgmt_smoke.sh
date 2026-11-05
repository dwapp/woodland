#!/usr/bin/env sh
# output-management 冒烟：test 不许有副作用、apply 必须生效、多 head 必须可见。
#
# 用法: sh run_output_mgmt_smoke.sh <woodland 二进制路径>
#
# 用 wlr-randr 当客户端（它就是 wlr-output-management 的消费者）：
#   wlr-randr              列出所有 head
#   wlr-randr --dryrun ... 发 test 请求
#   wlr-randr ...          发 apply 请求
set -eu

. "$(dirname "$0")/lib.sh"
smoke_init output-mgmt

BIN="${1:-./build/src/woodland}"
[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 125; }

command -v wlr-randr >/dev/null 2>&1 || {
    echo "wlr-randr not found, skip" >&2
    exit 77
}

unset WAYLAND_DISPLAY
export XDG_RUNTIME_DIR="$(mktemp -d)"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2

SMOKE_LOG="$XDG_RUNTIME_DIR/log"
"$BIN" >"$SMOKE_LOG" 2>&1 &
pid=$!
smoke_cleanup() {
    kill -TERM "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    rm -rf "$XDG_RUNTIME_DIR"
}

i=0
while [ "$(ls "$XDG_RUNTIME_DIR/" 2>/dev/null | grep -c '^wayland-')" -eq 0 ] \
        && [ "$i" -lt 50 ]; do
    sleep 0.1; i=$((i+1))
done
socket_name="$(ls "$XDG_RUNTIME_DIR/" 2>/dev/null | grep '^wayland-' | head -1)"
if [ -z "$socket_name" ]; then
    echo "wayland socket did not appear" >&2
    exit 1
fi
export WAYLAND_DISPLAY="$socket_name"

# 比通用版多一条：失败时把 wlr-randr 看到的东西也打出来
fail() {
    echo "--- wlr-randr 输出:" >&2
    wlr-randr >&2 2>&1 || :
    smoke_fail "$1"
}

# 取某个 head 的 Position 行
position_of() {
    wlr-randr 2>/dev/null | awk -v head="$1" '
        $0 ~ "^" head " " { inside = 1; next }
        inside && /^[A-Za-z]/ { inside = 0 }
        inside && /Position:/ { print $2; exit }
    '
}

enabled_of() {
    wlr-randr 2>/dev/null | awk -v head="$1" '
        $0 ~ "^" head " " { inside = 1; next }
        inside && /^[A-Za-z]/ { inside = 0 }
        inside && /Enabled:/ { print $2; exit }
    '
}

# 1. 两个 head 都要出现。少了任何一个，说明广播没带上新输出——
#    headless 输出没有 preferred mode 时尤其容易复现。
heads="$(wlr-randr 2>/dev/null | grep -c '^HEADLESS-')"
[ "$heads" -eq 2 ] || fail "期望 2 个 head，实际 $heads"
echo "ok: 2 个 head 都可见"

# 2. test 请求不许改动布局
before="$(position_of HEADLESS-1)"
[ -n "$before" ] || fail "拿不到 HEADLESS-1 的位置"
wlr-randr --dryrun --output HEADLESS-1 --pos 500,500 >/dev/null 2>&1 || true
sleep 0.3
after="$(position_of HEADLESS-1)"
[ "$before" = "$after" ] \
    || fail "dryrun 改动了布局：$before -> $after"
echo "ok: dryrun(test) 零副作用，位置仍是 $before"

# 3. apply 必须真的生效
wlr-randr --output HEADLESS-1 --pos 640,0 >/dev/null 2>&1 || true
sleep 0.3
moved="$(position_of HEADLESS-1)"
[ "$moved" = "640,0" ] || fail "apply 未生效：期望 640,0，实际 ${moved:-<空>}"
echo "ok: apply 生效，位置 640,0"

# 4. 关掉一个输出：head 必须报告 disabled，且另一个不受影响
wlr-randr --output HEADLESS-2 --off >/dev/null 2>&1 || true
sleep 0.3
[ "$(enabled_of HEADLESS-2)" = "no" ] || fail "HEADLESS-2 未报告 disabled"
[ "$(enabled_of HEADLESS-1)" = "yes" ] || fail "HEADLESS-1 被连带关掉了"
[ "$(position_of HEADLESS-1)" = "640,0" ] || fail "关闭 HEADLESS-2 影响了 HEADLESS-1 的位置"
echo "ok: HEADLESS-2 disabled，HEADLESS-1 未受影响"

echo "output-management smoke passed"
