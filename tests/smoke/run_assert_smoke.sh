#!/usr/bin/env sh
# 结构化无头屏幕断言冒烟：验证 wctl pixel / screenshot --region / wait。
#
# 用法: sh run_assert_smoke.sh <woodland> <wctl> <simple_ssd>
set -eu

. "$(dirname "$0")/lib.sh"
smoke_init assert

BIN="${1:-./build/src/woodland}"
WCTL="${2:-./build/src/wctl/wctl}"
CLIENT="${3:-./build/tests/simple_ssd}"
[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 125; }
[ -x "$WCTL" ] || { echo "wctl not found: $WCTL" >&2; exit 125; }
[ -x "$CLIENT" ] || { echo "client not found: $CLIENT" >&2; exit 125; }
command -v jq >/dev/null 2>&1 || { echo "jq not found, skip assert smoke" >&2; exit 77; }

unset WAYLAND_DISPLAY
export XDG_RUNTIME_DIR="$(mktemp -d)"
export XDG_CONFIG_HOME="$XDG_RUNTIME_DIR/config"
export HOME="$XDG_RUNTIME_DIR"
mkdir -p "$XDG_CONFIG_HOME"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1

SMOKE_LOG="$XDG_RUNTIME_DIR/woodland.log"
client_pid=""

smoke_cleanup() {
    smoke_stop_pid "$client_pid" 10
    client_pid=""
    smoke_stop_compositor
    rm -rf "$XDG_RUNTIME_DIR"
}

smoke_start_compositor --debug-ctl --self-check --trace

"$CLIENT" >"$XDG_RUNTIME_DIR/client.log" 2>&1 &
client_pid=$!

echo "== 1. wait view_mapped (with quoted title) =="
MAP_RES="$("$WCTL" -s "$SOCK" -t 5000 wait view_mapped "title=simple-ssd" app_id=org.woodland.simple-ssd)" \
    || smoke_fail "wait view_mapped failed: ${MAP_RES:-}"
echo "$MAP_RES" | jq -e '.ok == true and .matched == true and .view.id == 1' >/dev/null \
    || smoke_fail "unexpected wait view_mapped result: $MAP_RES"

echo "== 1b. wait trace (end-to-end over IPC) =="
WTRACE="$("$WCTL" -s "$SOCK" -t 2000 wait trace what=map obj=1)" \
    || smoke_fail "wait trace failed: ${WTRACE:-}"
echo "$WTRACE" | jq -e '.ok == true and .matched == true and .what == "map" and .obj == 1' >/dev/null \
    || smoke_fail "unexpected wait trace result: $WTRACE"

echo "== 1c. partially off-output screenshot clipping =="
SC_CLIP="$("$WCTL" -s "$SOCK" screenshot --region -10,-10,200,200)" \
    || smoke_fail "clipped screenshot failed: ${SC_CLIP:-}"
echo "$SC_CLIP" | jq -e '.ok == true and .region == [0, 0, 190, 190]' >/dev/null \
    || smoke_fail "unexpected clipped screenshot region: $SC_CLIP"

echo "== 2. read geometry & palette =="
TREE="$("$WCTL" -s "$SOCK" get_tree)" || smoke_fail "get_tree failed"
outer_x="$(echo "$TREE" | jq -r '.views[0].outer[0]')"
outer_y="$(echo "$TREE" | jq -r '.views[0].outer[1]')"
outer_w="$(echo "$TREE" | jq -r '.views[0].outer[2]')"
content_x="$(echo "$TREE" | jq -r '.views[0].content[0]')"
content_y="$(echo "$TREE" | jq -r '.views[0].content[1]')"

echo "== 3. pixel on SSD titlebar =="
PX_TB="$("$WCTL" -s "$SOCK" pixel "$((outer_x + 17))" "$((outer_y + 3))")" \
    || smoke_fail "pixel on titlebar failed: ${PX_TB:-}"
echo "$PX_TB" | jq -e \
    '.ok == true
     and .source.kind == "ssd.titlebar"
     and .source.expected_source == "ssd.palette.titlebar_active"
     and .source.matches_expected == true
     and .source.channel_delta <= 2' >/dev/null \
    || smoke_fail "titlebar pixel assertion failed: $PX_TB"

echo "== 4. pixel inside client content =="
PX_VIEW="$("$WCTL" -s "$SOCK" pixel "$((content_x + 125))" "$((content_y + 125))")" \
    || smoke_fail "pixel on client failed: ${PX_VIEW:-}"
echo "$PX_VIEW" | jq -e \
    '.ok == true
     and .hex == "#333333"
     and .source.kind == "view"
     and .source.title == "simple-ssd"
     and .source.expected_source == "client.surface"' >/dev/null \
    || smoke_fail "client pixel assertion failed: $PX_VIEW"

echo "== 5. pixel on compositor background =="
PX_BG="$("$WCTL" -s "$SOCK" pixel 1200 700)" \
    || smoke_fail "pixel on background failed: ${PX_BG:-}"
echo "$PX_BG" | jq -e \
    '.ok == true
     and .hex == "#000000"
     and .source.kind == "background"
     and .source.expected_hex == "#000000"
     and .source.matches_expected == true' >/dev/null \
    || smoke_fail "background pixel assertion failed: $PX_BG"

echo "== 6. screenshot titlebar strip (auto-expected from palette) =="
SC_TB="$("$WCTL" -s "$SOCK" screenshot --region "$((outer_x + 1)),$((outer_y + 1)),$((outer_w - 2)),32")" \
    || smoke_fail "screenshot titlebar failed: ${SC_TB:-}"
echo "$SC_TB" | jq -e \
    '.ok == true
     and .match.expected_source == "ssd.palette.titlebar_active"
     and .match.ratio_milli >= 500
     and .dominant[0].count > 0' >/dev/null \
    || smoke_fail "titlebar screenshot assertion failed: $SC_TB"

echo "== 7. screenshot client content (--expect #333333) =="
SC_VIEW="$("$WCTL" -s "$SOCK" screenshot --region "${content_x},${content_y},250,250" --expect "#333333")" \
    || smoke_fail "screenshot client failed: ${SC_VIEW:-}"
echo "$SC_VIEW" | jq -e \
    '.ok == true
     and .match.expected_hex == "#333333"
     and .match.ratio_milli >= 950' >/dev/null \
    || smoke_fail "client screenshot assertion failed: $SC_VIEW"

echo "== 7b. screenshot --output and --dump-ppm =="
SC_OUT="$("$WCTL" -s "$SOCK" screenshot --output HEADLESS-1)" \
    || smoke_fail "screenshot --output HEADLESS-1 failed: ${SC_OUT:-}"
echo "$SC_OUT" | jq -e '.ok == true and .output == "HEADLESS-1"' >/dev/null \
    || smoke_fail "unexpected screenshot --output result: $SC_OUT"
rc=0
SC_BAD="$("$WCTL" -s "$SOCK" screenshot --output NO-SUCH-OUTPUT 2>&1)" || rc=$?
[ "$rc" -ne 0 ] && echo "$SC_BAD" | jq -e '.ok == false' >/dev/null \
    || smoke_fail "screenshot --output NO-SUCH-OUTPUT should fail: $SC_BAD"

PPM_PATH="$XDG_RUNTIME_DIR/crop.ppm"
SC_PPM="$("$WCTL" -s "$SOCK" screenshot --region "${content_x},${content_y},64,64" --dump-ppm "$PPM_PATH")" \
    || smoke_fail "screenshot --dump-ppm failed: ${SC_PPM:-}"
echo "$SC_PPM" | jq -e --arg p "$PPM_PATH" '.ok == true and .dumped_ppm == $p' >/dev/null \
    || smoke_fail "unexpected --dump-ppm result: $SC_PPM"
[ -s "$PPM_PATH" ] || smoke_fail "dumped ppm file missing: $PPM_PATH"
head -n 3 "$PPM_PATH" | tr '\n' ' ' | grep -q '^P6 64 64 255 ' \
    || smoke_fail "unexpected PPM header in $PPM_PATH"
ppm_size="$(wc -c < "$PPM_PATH")"
hdr_size="$(head -n 3 "$PPM_PATH" | wc -c)"
[ "$((ppm_size - hdr_size))" -eq $((64 * 64 * 3)) ] \
    || smoke_fail "unexpected PPM payload size: $ppm_size - $hdr_size"

echo "== 8. move + wait pixel =="
"$WCTL" -s "$SOCK" move 1 400 200 | jq -e '.ok == true' >/dev/null \
    || smoke_fail "move failed"
WPX="$("$WCTL" -s "$SOCK" -t 2000 wait pixel x=500 y=300 hex=#333333)" \
    || smoke_fail "wait pixel after move failed: ${WPX:-}"
echo "$WPX" | jq -e '.ok == true and .matched == true' >/dev/null \
    || smoke_fail "unexpected wait pixel result: $WPX"

echo "== 9-10. workspace switch + pixel =="
"$WCTL" -s "$SOCK" workspace 2 | jq -e '.ok == true' >/dev/null \
    || smoke_fail "workspace 2 failed"
"$WCTL" -s "$SOCK" -t 2000 wait workspace name=2 >/dev/null \
    || smoke_fail "wait workspace 2 failed"
PX_WS2="$("$WCTL" -s "$SOCK" pixel 500 300)" || smoke_fail "pixel on ws2 failed"
echo "$PX_WS2" | jq -e '.ok == true and .source.kind == "background" and .hex == "#000000"' >/dev/null \
    || smoke_fail "ws2 should show background at 500,300: $PX_WS2"

"$WCTL" -s "$SOCK" workspace 1 | jq -e '.ok == true' >/dev/null \
    || smoke_fail "workspace 1 failed"
"$WCTL" -s "$SOCK" -t 2000 wait workspace name=1 >/dev/null \
    || smoke_fail "wait workspace 1 failed"

echo "== 11. wait timeout =="
rc=0
TOUT="$("$WCTL" -s "$SOCK" -t 300 wait view_mapped title=does-not-exist 2>&1)" || rc=$?
[ "$rc" -ne 0 ] || smoke_fail "timeout wait unexpectedly succeeded: $TOUT"
echo "$TOUT" | jq -e \
    '.ok == false and .cmd == "wait" and .condition == "view_mapped" and .matched == false' >/dev/null \
    || smoke_fail "unexpected timeout response: $TOUT"

echo "== 12. out-of-bounds pixel =="
rc=0
OOB1="$("$WCTL" -s "$SOCK" pixel -5 0 2>&1)" || rc=$?
[ "$rc" -ne 0 ] && echo "$OOB1" | jq -e '.ok == false' >/dev/null \
    || smoke_fail "pixel -5 0 should fail: $OOB1"
rc=0
OOB2="$("$WCTL" -s "$SOCK" pixel 9999 9999 2>&1)" || rc=$?
[ "$rc" -ne 0 ] && echo "$OOB2" | jq -e '.ok == false' >/dev/null \
    || smoke_fail "pixel 9999 9999 should fail: $OOB2"

echo "== 13. kill client + wait view_unmapped + wait view_count =="
kill -TERM "$client_pid" 2>/dev/null || true
wait "$client_pid" 2>/dev/null || true
client_pid=""
"$WCTL" -s "$SOCK" -t 3000 wait view_unmapped title=simple-ssd >/dev/null \
    || smoke_fail "wait view_unmapped failed"
"$WCTL" -s "$SOCK" -t 2000 wait view_count count=0 >/dev/null \
    || smoke_fail "wait view_count count=0 failed"

echo "== 14. self_check & listener ledger =="
"$WCTL" -s "$SOCK" self_check | jq -e '.ok == true and .failures == 0' >/dev/null \
    || smoke_fail "self_check failed"
"$WCTL" -s "$SOCK" get_listeners | jq -e '.ok == true and .overflow == 0' >/dev/null \
    || smoke_fail "get_listeners overflow"

echo "assert smoke OK"
