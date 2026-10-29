#!/usr/bin/env sh
# 调试接口冒烟：起一个 headless 合成器 + 一个真实客户端，然后用 wctl
# 断言 get_tree / get_focus / self_check / dump_scene 的结论。
#
# 这个脚本本身就是"AI 调试面"的用法示例：不再数像素，直接断言状态。
#
# 用法: sh run_ipc_smoke.sh <woodland 二进制> <wctl 二进制>
set -eu

BIN="${1:-./build/src/woodland}"
WCTL="${2:-./build/src/wctl/wctl}"
[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 125; }
[ -x "$WCTL" ] || { echo "wctl not found: $WCTL" >&2; exit 125; }

for tool in jq weston-simple-shm; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "$tool not found, skip ipc smoke" >&2
        exit 77
    }
done

unset WAYLAND_DISPLAY
export XDG_RUNTIME_DIR="$(mktemp -d)"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1

LOG="$XDG_RUNTIME_DIR/woodland.log"
comp_pid=""

cleanup() {
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
trap cleanup EXIT INT TERM

fail() {
    echo "FAIL: $*" >&2
    echo "--- compositor log ---" >&2
    tail -30 "$LOG" >&2 || true
    exit 1
}

"$BIN" --self-check -s "weston-simple-shm" >"$LOG" 2>&1 &
comp_pid=$!

# 等调试 socket 出现
SOCK=""
i=0
while [ "$i" -lt 100 ]; do
    for f in "$XDG_RUNTIME_DIR"/woodland-*.sock; do
        [ -S "$f" ] && SOCK="$f"
    done
    [ -n "$SOCK" ] && break
    sleep 0.05; i=$((i+1))
done
[ -n "$SOCK" ] || fail "debug socket never appeared"

# 等窗口 map 上来（get_tree 里出现 1 个视图）
i=0
while [ "$i" -lt 100 ]; do
    if "$WCTL" -s "$SOCK" get_tree 2>/dev/null | jq -e '.count >= 1' >/dev/null 2>&1; then
        break
    fi
    sleep 0.05; i=$((i+1))
done

echo "== ping =="
"$WCTL" -s "$SOCK" ping | jq -e '.ok == true and .pid > 0' >/dev/null \
    || fail "ping"

echo "== get_tree =="
TREE="$("$WCTL" -s "$SOCK" get_tree)" || fail "get_tree exited non-zero"
echo "$TREE" | jq -e '.ok == true' >/dev/null || fail "get_tree ok=false"
echo "$TREE" | jq -e '.count == 1' >/dev/null || fail "expected exactly 1 view"
echo "$TREE" | jq -e '.views[0] | .mapped and .focused and .activated' >/dev/null \
    || fail "the only view should be mapped, focused and activated"
echo "$TREE" | jq -e '.views[0].z == 0' >/dev/null || fail "z order: expected 0"
echo "$TREE" | jq -e '.views[0].workspace == .workspace.current' >/dev/null \
    || fail "view workspace should be the active one"
echo "$TREE" | jq -e '.views[0] | (.outer[2] > 0) and (.outer[3] > 0)' >/dev/null \
    || fail "outer box is empty"
echo "$TREE" | jq -e '.views[0].ssd.present and .views[0].ssd.active' >/dev/null \
    || fail "SSD should exist and be active for the focused view"
echo "$TREE" | jq -e '.views[0].ssd.margins[1] == 27' >/dev/null \
    || fail "expected a 27px titlebar margin"
# 内容框必须落在外框之内（标题栏 27px 不能盖住内容）
echo "$TREE" | jq -e '.views[0] | (.content[1] == .outer[1] + 27)' >/dev/null \
    || fail "content should start one titlebar below the outer box"
echo "$TREE" | jq -e '.outputs[0].workarea[2] == 1280 and .outputs[0].workarea[3] == 720' >/dev/null \
    || fail "unexpected headless workarea"
echo "$TREE" | jq -e '.placement_policy == "cascade"' >/dev/null \
    || fail "default placement policy should be cascade"

echo "== get_focus =="
"$WCTL" -s "$SOCK" get_focus | jq -e '.ok and .view == 1 and .recorded_view == 1' >/dev/null \
    || fail "get_focus did not report view 1"

echo "== self_check =="
CHECK="$("$WCTL" -s "$SOCK" self_check)" || fail "self_check reported failures"
echo "$CHECK" | jq -e '.ok == true and .failures == 0' >/dev/null \
    || fail "self_check not clean: $CHECK"
echo "$CHECK" | jq -e '[.checks[].name] | index("activated_unique") != null' >/dev/null \
    || fail "activated_unique check missing"

echo "== dump_scene =="
"$WCTL" -s "$SOCK" dump_scene | jq -e '.ok and .tree.type == "tree" and .nodes > 0' >/dev/null \
    || fail "dump_scene"

echo "== unknown command =="
if "$WCTL" -s "$SOCK" bogus >/dev/null 2>&1; then
    fail "unknown command should exit non-zero"
fi

echo "== multiple commands on one connection (nc) =="
LINES="$(printf 'get_focus\nhelp\n' | nc -U -q1 "$SOCK" 2>/dev/null | wc -l)"
[ "$LINES" -ge 2 ] || fail "expected 2 replies over one connection, got $LINES"

# 一次灌进多条命令：服务端"发不完就先不解析下一条、等 WRITABLE 再续"
# 这条状态机如果写错，表现就是回了前几条然后卡死
echo "== pipelined commands =="
PIPED="$(awk 'BEGIN{for(i=0;i<20;i++)print "ping"}' | nc -U -q1 "$SOCK" 2>/dev/null | wc -l)"
[ "$PIPED" -ge 20 ] || fail "expected 20 pipelined replies, got $PIPED"

echo "== wctl options =="
"$WCTL" --help >/dev/null 2>&1 || fail "--help should exit 0"
"$WCTL" -s "$SOCK" --timeout 2000 ping | jq -e '.ok' >/dev/null \
    || fail "--timeout long option rejected"

# 命令超长必须在 write 之前拒绝：snprintf 截断时返回的是"本来要写多少"，
# 直接拿去 write 会从栈上读出去
echo "== oversized command =="
LONG="$(awk 'BEGIN{for(i=0;i<200;i++)printf "a"}')"
if "$WCTL" -s "$SOCK" "$LONG" 2>&1 | grep -q "command too long"; then
    :
else
    fail "oversized command was not rejected before write"
fi

if command -v python3 >/dev/null 2>&1; then
    # 连上就不读的客户端不能把合成器冻住：写不动时排队等 WRITABLE，
    # 事件循环必须照常转，别的连接还能拿到回应。
    echo "== stalled client must not freeze the compositor =="
    python3 - "$SOCK" <<'PY' &
import socket, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
s.sendall(b"dump_scene\n" * 2000)   # 灌满后既不读也不关
time.sleep(3)
s.close()
PY
    stalled=$!
    sleep 0.3
    "$WCTL" -s "$SOCK" -t 1000 ping | jq -e '.ok' >/dev/null \
        || fail "compositor stopped answering while a client refused to read"
    kill "$stalled" 2>/dev/null || true
    wait "$stalled" 2>/dev/null || true

    # -t 要能约束 read：对端连上就不说话时不能一直等
    echo "== -t bounds the read, not just the connect =="
    SLOW="$XDG_RUNTIME_DIR/slow.sock"
    python3 - "$SLOW" <<'PY' &
import socket, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(sys.argv[1])
s.listen(1)
c, _ = s.accept()
time.sleep(5)          # 连上，一个字也不回
c.close()
PY
    slow_srv=$!
    i=0
    while [ ! -S "$SLOW" ] && [ "$i" -lt 100 ]; do
        sleep 0.05; i=$((i+1))
    done
    started=$(date +%s)
    if "$WCTL" -s "$SLOW" -t 400 ping >/dev/null 2>&1; then
        fail "wctl should fail against a silent server"
    fi
    elapsed=$(( $(date +%s) - started ))
    [ "$elapsed" -le 3 ] || fail "read timeout ignored (waited ${elapsed}s)"
    kill "$slow_srv" 2>/dev/null || true
    wait "$slow_srv" 2>/dev/null || true
fi

echo "== no self-check failures in the log =="
if grep -q "self-check" "$LOG"; then
    grep "self-check" "$LOG" >&2
    fail "self-check logged a failure"
fi

# 正常退出后 socket 必须被清掉，否则下次 bind 会被残留文件挡住（虽然会被 unlink）
kill -TERM "$comp_pid" 2>/dev/null || true
i=0
while kill -0 "$comp_pid" 2>/dev/null && [ "$i" -lt 50 ]; do
    sleep 0.1; i=$((i+1))
done
comp_pid=""
[ -e "$SOCK" ] && fail "socket left behind: $SOCK"

echo "ipc smoke OK"
