#!/usr/bin/env sh
# 调试接口冒烟：起一个 headless 合成器 + 一个真实客户端，然后用 wctl
# 断言 get_tree / get_focus / self_check / dump_scene 的结论。
#
# 这个脚本本身就是"AI 调试面"的用法示例：不再数像素，直接断言状态。
#
# 用法: sh run_ipc_smoke.sh <woodland 二进制> <wctl 二进制> [客户端命令]
set -eu

# 公共壳：失败时把日志 + 状态快照 + 决策轨迹落到 meson-logs/smoke/ipc/
. "$(dirname "$0")/lib.sh"
smoke_init ipc

BIN="${1:-./build/src/woodland}"
WCTL="${2:-./build/src/wctl/wctl}"
CLIENT="${3:-weston-simple-shm}"
[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 125; }
[ -x "$WCTL" ] || { echo "wctl not found: $WCTL" >&2; exit 125; }

command -v jq >/dev/null 2>&1 || {
    echo "jq not found, skip ipc smoke" >&2
    exit 77
}
if [ ! -x "$CLIENT" ] && ! command -v "$CLIENT" >/dev/null 2>&1; then
    echo "$CLIENT not found, skip ipc smoke" >&2
    exit 77
fi

unset WAYLAND_DISPLAY
export XDG_RUNTIME_DIR="$(mktemp -d)"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1

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
"$BIN" --self-check --trace -s "$CLIENT" >"$SMOKE_LOG" 2>&1 &
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
[ -n "$SOCK" ] || smoke_fail "debug socket never appeared"

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
    || smoke_fail "ping"

echo "== get_tree =="
TREE="$("$WCTL" -s "$SOCK" get_tree)" || smoke_fail "get_tree exited non-zero"
echo "$TREE" | jq -e '.ok == true' >/dev/null || smoke_fail "get_tree ok=false"
echo "$TREE" | jq -e '.count == 1' >/dev/null || smoke_fail "expected exactly 1 view"
echo "$TREE" | jq -e '.views[0] | .mapped and .focused and .activated' >/dev/null \
    || smoke_fail "the only view should be mapped, focused and activated"
echo "$TREE" | jq -e '.views[0].z == 0' >/dev/null || smoke_fail "z order: expected 0"
echo "$TREE" | jq -e '.views[0].workspace == .workspace.current' >/dev/null \
    || smoke_fail "view workspace should be the active one"
echo "$TREE" | jq -e '.views[0] | (.outer[2] > 0) and (.outer[3] > 0)' >/dev/null \
    || smoke_fail "outer box is empty"
echo "$TREE" | jq -e '.views[0].ssd.present and .views[0].ssd.active' >/dev/null \
    || smoke_fail "SSD should exist and be active for the focused view"
echo "$TREE" | jq -e '.views[0].ssd.margins[1] == 27' >/dev/null \
    || smoke_fail "expected a 27px titlebar margin"
# 内容框必须落在外框之内（标题栏 27px 不能盖住内容）
echo "$TREE" | jq -e '.views[0] | (.content[1] == .outer[1] + 27)' >/dev/null \
    || smoke_fail "content should start one titlebar below the outer box"
echo "$TREE" | jq -e '.outputs[0].workarea[2] == 1280 and .outputs[0].workarea[3] == 720' >/dev/null \
    || smoke_fail "unexpected headless workarea"
echo "$TREE" | jq -e '.placement_policy == "cascade"' >/dev/null \
    || smoke_fail "default placement policy should be cascade"
echo "$TREE" | jq -e '.snap_preview.active == false' >/dev/null \
    || smoke_fail "snap_preview should be inactive initially"
# 没拖动就不该有 edges / box / output：字段缺失是契约的一部分（消费端不用
# 判"激活了但 box 还是 0"），也避免把上一轮的残值当当前状态读
echo "$TREE" | jq -e '.snap_preview | (has("edges") or has("box") or has("output")) | not' >/dev/null \
    || smoke_fail "inactive snap_preview should carry only 'active'"

echo "== get_focus =="
"$WCTL" -s "$SOCK" get_focus | jq -e '.ok and .view == 1 and .recorded_view == 1' >/dev/null \
    || smoke_fail "get_focus did not report view 1"

echo "== self_check =="
CHECK="$("$WCTL" -s "$SOCK" self_check)" || smoke_fail "self_check reported failures"
echo "$CHECK" | jq -e '.ok == true and .failures == 0' >/dev/null \
    || smoke_fail "self_check not clean: $CHECK"
echo "$CHECK" | jq -e '[.checks[].name] | index("activated_unique") != null' >/dev/null \
    || smoke_fail "activated_unique check missing"

echo "== dump_scene =="
"$WCTL" -s "$SOCK" dump_scene | jq -e '.ok and .tree.type == "tree" and .nodes > 0' >/dev/null \
    || smoke_fail "dump_scene"

echo "== get_trace =="
# 决策轨迹：不只是"有内容"，还要顺序对、对象对、没绕圈。
# 一个窗口从创建到有焦点，因果顺序固定是 map → place → focus。
TRACE="$("$WCTL" -s "$SOCK" get_trace)" || smoke_fail "get_trace exited non-zero"
echo "$TRACE" | jq -e '.ok and .enabled == true' >/dev/null \
    || smoke_fail "trace should be enabled (started with --trace)"
echo "$TRACE" | jq -e '.capacity > 0 and .dropped == 0' >/dev/null \
    || smoke_fail "unexpected ring state: $TRACE"
echo "$TRACE" | jq -e '.count == .total' >/dev/null \
    || smoke_fail "count should equal total while the ring is not full"
# seq 必须从 0 开始且严格递增——顺序错了，轨迹就只能当噪声
echo "$TRACE" | jq -e '[.entries[].seq] == ([.entries[].seq] | sort) and .entries[0].seq == 0' >/dev/null \
    || smoke_fail "trace entries are not in seq order"
echo "$TRACE" | jq -e '[.entries[].what] | index("map") != null' >/dev/null \
    || smoke_fail "no map event in the trace"
echo "$TRACE" | jq -e '[.entries[].what] | index("place") != null' >/dev/null \
    || smoke_fail "no place decision in the trace"
echo "$TRACE" | jq -e '[.entries[].what] | index("focus") != null' >/dev/null \
    || smoke_fail "no focus event in the trace"
echo "$TRACE" | jq -e '[.entries[].what] | index("output_add") != null' >/dev/null \
    || smoke_fail "no output_add event in the trace"
# 因果顺序：map 必须早于 place，place 早于 focus
echo "$TRACE" | jq -e '
    ([.entries[] | select(.what=="map")][0].seq) as $m
  | ([.entries[] | select(.what=="place")][0].seq) as $p
  | ([.entries[] | select(.what=="focus")][0].seq) as $f
  | $m < $p and $p < $f' >/dev/null \
    || smoke_fail "trace order is wrong: map/place/focus must be sequential"
# place 的 detail 要写出依据（策略名），否则这条记录等于没用
echo "$TRACE" | jq -e '[.entries[] | select(.what=="place")][0].detail | startswith("policy=")' >/dev/null \
    || smoke_fail "place detail should name the policy"
echo "$TRACE" | jq -e '[.entries[] | select(.what=="focus")][0].obj == 1' >/dev/null \
    || smoke_fail "focus event should carry the view id"

echo "== get_listeners =="
# 监听器账本：注册顺序就是调用顺序（AGENTS.md 坑 2）。这里断言的正是
# 那个坑的现场：cursor.motion 必须排在 input.cursor_motion 前面——
# 反过来的话协议层会用旧焦点发相对位移。以前这条契约只活在注释里。
LST="$("$WCTL" -s "$SOCK" get_listeners)" || smoke_fail "get_listeners exited non-zero"
echo "$LST" | jq -e '.ok and .overflow == 0 and .count > 0' >/dev/null \
    || smoke_fail "listener ledger should be non-empty with no overflow: $LST"
echo "$LST" | jq -e '[.listeners[].seq] == ([.listeners[].seq] | sort)' >/dev/null \
    || smoke_fail "listener dump must be sorted by registration seq"
echo "$LST" | jq -e '
    ([.listeners[] | select(.name=="cursor.motion")][0].seq) as $c
  | ([.listeners[] | select(.name=="input.motion")][0].seq) as $i
  | $c < $i' >/dev/null \
    || smoke_fail "cursor.motion must be registered before input.motion (stale-focus pitfall)"
# emit 计数在跑：server.new_output 用 connect_verbose 注册（thunk 自动计数），
# 启动建输出时必发一次——这条断言证明计账链路是活的，也证明坑 2 的
# 补广播监听器真的执行过
echo "$LST" | jq -e '[.listeners[] | select(.name=="server.new_output")][0].emits >= 1' >/dev/null \
    || smoke_fail "server.new_output should have counted at least one emit (verbose registration)"
# 手动计数的示例在 server_cursor_motion：headless 没有输入事件，emits 为 0 属正常
echo "$LST" | jq -e '[.listeners[] | .name | length > 0] | all' >/dev/null \
    || smoke_fail "every listener entry needs a name"

echo "== help =="
# help 是自描述的：字段名+类型都在响应里，调用方不用读 commands.c。
# 断言形状而不是内容细节：每个命令都要有 desc，每个字段都要有 type。
HELP="$("$WCTL" -s "$SOCK" help)" || smoke_fail "help exited non-zero"
echo "$HELP" | jq -e '.ok and (.commands | length >= 7)' >/dev/null \
    || smoke_fail "help should list every command"
echo "$HELP" | jq -e '[.commands[] | has("desc") and (.fields|length > 0)] | all' >/dev/null \
    || smoke_fail "every help entry needs a desc and at least one field"
echo "$HELP" | jq -e '[.commands[].fields[] | has("type")] | all' >/dev/null \
    || smoke_fail "every help field needs a type"

echo "== unknown command =="
if "$WCTL" -s "$SOCK" bogus >/dev/null 2>&1; then
    smoke_fail "unknown command should exit non-zero"
fi

# nc 只用于下面两条「一个连接多条命令」的传输测试，缺了不影响其余断言，
# 所以不放进上面的必需列表。之前直接调用它，没有 nc 的环境里管道失败被
# 2>/dev/null 吞掉，表现成「0 条回应」的假失败（Arch 的 base 就没有 nc）。
if command -v nc >/dev/null 2>&1; then
    echo "== multiple commands on one connection (nc) =="
    LINES="$(printf 'get_focus\nhelp\n' | nc -U -q1 "$SOCK" 2>/dev/null | wc -l)"
    [ "$LINES" -ge 2 ] || smoke_fail "expected 2 replies over one connection, got $LINES"

    # 一次灌进多条命令：服务端"发不完就先不解析下一条、等 WRITABLE 再续"
    # 这条状态机如果写错，表现就是回了前几条然后卡死
    echo "== pipelined commands =="
    PIPED="$(awk 'BEGIN{for(i=0;i<20;i++)print "ping"}' | nc -U -q1 "$SOCK" 2>/dev/null | wc -l)"
    [ "$PIPED" -ge 20 ] || smoke_fail "expected 20 pipelined replies, got $PIPED"
else
    echo "== nc not found, skipping the pipelined transport checks =="
fi

echo "== wctl options =="
"$WCTL" --help >/dev/null 2>&1 || smoke_fail "--help should exit 0"
"$WCTL" -s "$SOCK" --timeout 2000 ping | jq -e '.ok' >/dev/null \
    || smoke_fail "--timeout long option rejected"

# 命令超长必须在 write 之前拒绝：snprintf 截断时返回的是"本来要写多少"，
# 直接拿去 write 会从栈上读出去
echo "== oversized command =="
LONG="$(awk 'BEGIN{for(i=0;i<200;i++)printf "a"}')"
if "$WCTL" -s "$SOCK" "$LONG" 2>&1 | grep -q "command too long"; then
    :
else
    smoke_fail "oversized command was not rejected before write"
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
        || smoke_fail "compositor stopped answering while a client refused to read"
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
        smoke_fail "wctl should fail against a silent server"
    fi
    elapsed=$(( $(date +%s) - started ))
    [ "$elapsed" -le 3 ] || smoke_fail "read timeout ignored (waited ${elapsed}s)"
    kill "$slow_srv" 2>/dev/null || true
    wait "$slow_srv" 2>/dev/null || true
fi

echo "== no self-check failures in the log =="
if grep -q "self-check" "$SMOKE_LOG"; then
    grep "self-check" "$SMOKE_LOG" >&2
    smoke_fail "self-check logged a failure"
fi

# 正常退出后 socket 必须被清掉，否则下次 bind 会被残留文件挡住（虽然会被 unlink）
kill -TERM "$comp_pid" 2>/dev/null || true
i=0
while kill -0 "$comp_pid" 2>/dev/null && [ "$i" -lt 50 ]; do
    sleep 0.1; i=$((i+1))
done
comp_pid=""
[ -e "$SOCK" ] && smoke_fail "socket left behind: $SOCK"

echo "ipc smoke OK"
