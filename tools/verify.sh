#!/usr/bin/env bash
# One-command build + test verification entrypoint with machine-readable JSON
# summary and prominent SKIP warnings.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
NO_EFFECTS_DIR="$REPO_ROOT/build-noeffects"
MODE="full"          # full | single | ci | quick
SUITE=""
JSON_MODE=0
JSON_OUT=""
FAIL_ON_SKIP=0

usage() {
    cat <<'EOF'
Usage: verify.sh [options]

Run build + tests and emit a structured summary with prominent SKIP warnings.
Default mode validates both the effects-enabled build (`build`) and the
`-Deffects=disabled` CI-equivalent build (`build-noeffects`).

Options:
  --quick                 Build primary dir and run `--suite quick` only
  --suite <name>          Run only the given Meson test suite
  --single                Test primary effects-enabled build (`build`) only
  --ci                    Test `-Deffects=disabled` build (`build-noeffects`) only
  --build-dir <dir>       Effects build directory (default: ./build)
  --no-effects-dir <dir>  No-effects build directory (default: ./build-noeffects)
  --json                  Print ONLY JSON summary to stdout (logs go to stderr)
  --json-out <path>       Write JSON summary to file (default: <dir>/meson-logs/verify-summary.json)
  --fail-on-skip          Exit 2 if any test was skipped (even if 0 failed)
  -h, --help              Show this help
EOF
}

resolve_dir() {
    case "$1" in
    /*) printf '%s' "$1" ;;
    *) printf '%s/%s' "$REPO_ROOT" "$1" ;;
    esac
}

while [ $# -gt 0 ]; do
    case "$1" in
    --quick)
        MODE="quick"
        SUITE="quick"
        ;;
    --suite)
        shift
        [ $# -gt 0 ] || { echo "verify.sh: --suite requires an argument" >&2; exit 2; }
        SUITE="${1#woodland:}"
        ;;
    --single)
        MODE="single"
        ;;
    --ci)
        MODE="ci"
        ;;
    --build-dir)
        shift
        [ $# -gt 0 ] || { echo "verify.sh: --build-dir requires an argument" >&2; exit 2; }
        BUILD_DIR="$(resolve_dir "$1")"
        ;;
    --no-effects-dir)
        shift
        [ $# -gt 0 ] || { echo "verify.sh: --no-effects-dir requires an argument" >&2; exit 2; }
        NO_EFFECTS_DIR="$(resolve_dir "$1")"
        ;;
    --json)
        JSON_MODE=1
        ;;
    --json-out)
        shift
        [ $# -gt 0 ] || { echo "verify.sh: --json-out requires an argument" >&2; exit 2; }
        JSON_OUT="$(resolve_dir "$1")"
        ;;
    --fail-on-skip)
        FAIL_ON_SKIP=1
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        echo "verify.sh: unknown option '$1'" >&2
        usage >&2
        exit 2
        ;;
    esac
    shift
done

command -v meson >/dev/null 2>&1 || { echo "verify.sh: meson not found" >&2; exit 125; }
command -v python3 >/dev/null 2>&1 || { echo "verify.sh: python3 not found" >&2; exit 125; }

if [ -z "$JSON_OUT" ]; then
    if [ "$MODE" = "ci" ]; then
        JSON_OUT="$NO_EFFECTS_DIR/meson-logs/verify-summary.json"
    else
        JSON_OUT="$BUILD_DIR/meson-logs/verify-summary.json"
    fi
fi

log_msg() {
    if [ "$JSON_MODE" -eq 1 ]; then
        printf '%s\n' "$*" >&2
    else
        printf '%s\n' "$*"
    fi
}

run_cmd() {
    if [ "$JSON_MODE" -eq 1 ]; then
        "$@" >&2
    else
        "$@"
    fi
}

has_gpu_allocator() {
    for dev in /dev/dri/renderD* /dev/udmabuf; do
        if [ -e "$dev" ]; then
            return 0
        fi
    done
    return 1
}

ensure_build_configured() {
    local dir="$1"
    local effects_val="$2"
    if [ ! -f "$dir/build.ninja" ]; then
        log_msg "[verify] meson setup $dir -Dtest=enabled -Deffects=$effects_val"
        run_cmd meson setup "$dir" "$REPO_ROOT" -Dtest=enabled -Deffects="$effects_val"
        return $?
    fi
    local mismatch
    mismatch="$(meson introspect --buildoptions "$dir" 2>/dev/null | python3 -c '
import json, sys
want_eff = sys.argv[1]
opts = {o["name"]: o["value"] for o in json.load(sys.stdin)}
bad = []
if opts.get("test") != "enabled":
    bad.append("test=" + str(opts.get("test")))
if opts.get("effects") != want_eff:
    bad.append("effects=" + str(opts.get("effects")))
print(",".join(bad))
' "$effects_val" 2>/dev/null || echo "introspect_failed")"
    if [ -n "$mismatch" ]; then
        log_msg "[verify] reconfiguring $dir ($mismatch -> test=enabled,effects=$effects_val)"
        run_cmd meson configure "$dir" -Dtest=enabled -Deffects="$effects_val"
        return $?
    fi
    return 0
}

TMP_META="$(mktemp)"
trap 'rm -f "$TMP_META"' EXIT

run_build_variant() {
    local name="$1"
    local dir="$2"
    local effects_val="$3"

    local setup_rc=0
    ensure_build_configured "$dir" "$effects_val" || setup_rc=$?

    local compile_rc=0
    local compile_t0 compile_t1 compile_dur
    compile_t0="$(python3 -c 'import time; print(time.monotonic())')"
    if [ "$setup_rc" -ne 0 ]; then
        compile_rc="$setup_rc"
    else
        log_msg "[verify] compiling $name ($dir)..."
        run_cmd meson compile -C "$dir" || compile_rc=$?
    fi
    compile_t1="$(python3 -c 'import time; print(time.monotonic())')"
    compile_dur="$(python3 -c "print(f'{float($compile_t1)-float($compile_t0):.3f}')")"

    rm -f "$dir"/meson-logs/testlog-verify-*.json \
          "$dir"/meson-logs/testlog-verify-*.txt \
          "$dir"/meson-logs/testlog-verify-*.junit.xml 2>/dev/null || true

    local log_files=()
    local smoke_skip_reason=""
    local test_rc=0

    if [ "$compile_rc" -eq 0 ]; then
        if [ -n "$SUITE" ]; then
            local run_suite=1
            local effective_suite="$SUITE"
            local lb="testlog-verify-suite"
            if [ "$effects_val" = "enabled" ] && ! has_gpu_allocator; then
                case "$SUITE" in
                smoke|needs-gpu|xwayland)
                    smoke_skip_reason="no /dev/dri/renderD* or /dev/udmabuf: -Deffects=enabled smoke cannot allocate buffers"
                    log_msg "[verify] WARNING: $smoke_skip_reason (skipping $SUITE on $name)"
                    run_suite=0
                    ;;
                quick)
                    smoke_skip_reason="no /dev/dri/renderD* or /dev/udmabuf: skipping smoke tests in quick suite on effects build"
                    log_msg "[verify] WARNING: $smoke_skip_reason"
                    effective_suite="unit"
                    lb="testlog-verify-unit"
                    ;;
                esac
            fi
            if [ "$run_suite" -eq 1 ]; then
                log_msg "[verify] running suite '$effective_suite' on $name..."
                local rc=0
                run_cmd meson test -C "$dir" --suite "woodland:$effective_suite" --print-errorlogs --logbase "$lb" || rc=$?
                [ "$rc" -ne 0 ] && test_rc="$rc"
                [ -f "$dir/meson-logs/$lb.json" ] && log_files+=("$dir/meson-logs/$lb.json")
            fi
        else
            log_msg "[verify] running unit tests on $name..."
            local rc_u=0
            run_cmd meson test -C "$dir" --suite "woodland:unit" --print-errorlogs --logbase testlog-verify-unit || rc_u=$?
            [ "$rc_u" -ne 0 ] && test_rc="$rc_u"
            [ -f "$dir/meson-logs/testlog-verify-unit.json" ] && log_files+=("$dir/meson-logs/testlog-verify-unit.json")

            if [ "$effects_val" = "enabled" ] && ! has_gpu_allocator; then
                smoke_skip_reason="no /dev/dri/renderD* or /dev/udmabuf: -Deffects=enabled smoke cannot allocate buffers"
                log_msg "[verify] WARNING: $smoke_skip_reason (skipping smoke on $name)"
            else
                log_msg "[verify] running smoke tests on $name..."
                local rc_s=0
                run_cmd meson test -C "$dir" --suite "woodland:smoke" --print-errorlogs --logbase testlog-verify-smoke || rc_s=$?
                [ "$rc_s" -ne 0 ] && test_rc="$rc_s"
                [ -f "$dir/meson-logs/testlog-verify-smoke.json" ] && log_files+=("$dir/meson-logs/testlog-verify-smoke.json")
            fi
        fi
    fi

    local logs_csv=""
    if [ "${#log_files[@]}" -gt 0 ]; then
        logs_csv="$(IFS='|'; echo "${log_files[*]}")"
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$name" "$dir" "$effects_val" "$compile_rc" "$compile_dur" \
        "$test_rc" "$smoke_skip_reason" "$logs_csv" >>"$TMP_META"
}

case "$MODE" in
full)
    run_build_variant "effects" "$BUILD_DIR" "enabled"
    run_build_variant "no-effects" "$NO_EFFECTS_DIR" "disabled"
    ;;
single|quick)
    run_build_variant "effects" "$BUILD_DIR" "enabled"
    ;;
ci)
    run_build_variant "no-effects" "$NO_EFFECTS_DIR" "disabled"
    ;;
esac

mkdir -p "$(dirname "$JSON_OUT")"

python3 - "$TMP_META" "$MODE" "$SUITE" "$JSON_OUT" "$JSON_MODE" "$FAIL_ON_SKIP" <<'PY'
import json
import os
import re
import sys

meta_path, mode, suite, json_out, json_mode_str, fail_on_skip_str = sys.argv[1:7]
json_mode = json_mode_str == "1"
fail_on_skip = fail_on_skip_str == "1"

DUMP_RE = re.compile(r"^---\s*现场已保存到\s+(.+?)\s*$")


def extract_details(stdout_text, stderr_text, result):
    lines = []
    dump_dir = None
    for src in (stderr_text or "", stdout_text or ""):
        for raw in src.splitlines():
            s = raw.strip()
            if not s:
                continue
            m = DUMP_RE.match(s)
            if m:
                dump_dir = m.group(1)
                continue
            lines.append(s)

    if not lines:
        return "", dump_dir

    if result == "SKIP":
        for s in lines:
            if "skip" in s.lower():
                return s, dump_dir
        return lines[0], dump_dir

    for s in lines:
        if (
            s.startswith("FAIL:")
            or "Assertion" in s
            or "assertion" in s
            or "ERROR: AddressSanitizer" in s
            or "runtime error:" in s
            or "SUMMARY:" in s
        ):
            return s, dump_dir
    return lines[-1], dump_dir


builds = []
warnings = []
total_passed = 0
total_failed = 0
total_skipped = 0
total_duration = 0.0
any_compile_fail = False
any_runner_fail = False

with open(meta_path, "r", encoding="utf-8") as fh:
    for line in fh:
        line = line.rstrip("\n")
        if not line:
            continue
        name, bdir, eff_val, comp_rc_s, comp_dur_s, test_rc_s, smoke_skip, logs_csv = line.split("\t")
        comp_rc = int(comp_rc_s)
        comp_dur = float(comp_dur_s)
        test_rc = int(test_rc_s)
        comp_ok = comp_rc == 0
        if not comp_ok:
            any_compile_fail = True
        if test_rc != 0:
            any_runner_fail = True

        passed = []
        failed = []
        skipped = []
        test_dur = 0.0

        for log_path in [p for p in logs_csv.split("|") if p]:
            if not os.path.exists(log_path):
                continue
            with open(log_path, "r", encoding="utf-8") as lf:
                for raw in lf:
                    raw = raw.strip()
                    if not raw:
                        continue
                    rec = json.loads(raw)
                    tname = rec.get("name", "unknown")
                    res = rec.get("result", "FAIL")
                    dur = float(rec.get("duration") or 0.0)
                    rc = rec.get("returncode")
                    test_dur += dur
                    reason, dump_dir = extract_details(
                        rec.get("stdout", ""), rec.get("stderr", ""), res
                    )
                    if res in ("OK", "EXPECTEDFAIL"):
                        passed.append(tname)
                    elif res == "SKIP":
                        skipped.append({
                            "name": tname,
                            "returncode": rc,
                            "duration_s": round(dur, 3),
                            "reason": reason or "skipped (exit 77)",
                        })
                        warnings.append({
                            "build": name,
                            "type": "test_skipped",
                            "test": tname,
                            "reason": reason or "skipped (exit 77)",
                        })
                    else:
                        entry = {
                            "name": tname,
                            "result": res,
                            "returncode": rc,
                            "duration_s": round(dur, 3),
                            "reason": reason or res,
                        }
                        if dump_dir:
                            entry["dump_dir"] = dump_dir
                        failed.append(entry)

        if smoke_skip:
            warnings.append({
                "build": name,
                "type": "smoke_suite_skipped",
                "reason": smoke_skip,
            })

        total_passed += len(passed)
        total_failed += len(failed)
        total_skipped += len(skipped)
        build_dur = comp_dur + test_dur
        total_duration += build_dur

        builds.append({
            "name": name,
            "dir": bdir,
            "effects": eff_val == "enabled",
            "compile": {
                "ok": comp_ok,
                "rc": comp_rc,
                "duration_s": round(comp_dur, 3),
            },
            "test_runner_rc": test_rc,
            "smoke_skipped_reason": smoke_skip or None,
            "counts": {
                "passed": len(passed),
                "failed": len(failed),
                "skipped": len(skipped),
                "duration_s": round(build_dur, 3),
            },
            "passed": passed,
            "failed": failed,
            "skipped": skipped,
        })

ok = (not any_compile_fail) and (total_failed == 0) and (not any_runner_fail)
has_warnings = len(warnings) > 0

summary = {
    "ok": ok,
    "has_warnings": has_warnings,
    "mode": mode,
    "suite": suite or None,
    "total": {
        "passed": total_passed,
        "failed": total_failed,
        "skipped": total_skipped,
        "duration_s": round(total_duration, 3),
    },
    "builds": builds,
    "warnings": warnings,
}

with open(json_out, "w", encoding="utf-8") as jf:
    json.dump(summary, jf, ensure_ascii=False, indent=2)
    jf.write("\n")

if json_mode:
    print(json.dumps(summary, ensure_ascii=False, indent=2))
else:
    print("")
    print("=== Verify Summary ===")
    for b in builds:
        c = b["counts"]
        comp_str = "OK" if b["compile"]["ok"] else f"FAIL(rc={b['compile']['rc']})"
        print(
            f"  [{b['name']}] compile={comp_str} "
            f"passed={c['passed']} failed={c['failed']} skipped={c['skipped']} "
            f"({c['duration_s']:.2f}s)"
        )
        for f in b["failed"]:
            dump = f" dump={f['dump_dir']}" if f.get("dump_dir") else ""
            print(f"    FAIL {f['name']} ({f['result']}): {f['reason']}{dump}")
    print(
        f"Total: passed={total_passed} failed={total_failed} "
        f"skipped={total_skipped} ({total_duration:.2f}s)"
    )
    if warnings:
        print("", file=sys.stderr)
        print(
            f"WARNING: {total_skipped} test(s) SKIPPED / {len(warnings)} warning(s) "
            f"(SKIP passes Meson by default!):",
            file=sys.stderr,
        )
        for w in warnings:
            if w["type"] == "test_skipped":
                print(
                    f"  [SKIP] [{w['build']}] {w['test']}: {w['reason']}",
                    file=sys.stderr,
                )
            else:
                print(
                    f"  [WARN] [{w['build']}] {w['reason']}",
                    file=sys.stderr,
                )
        print("  Tip: run ./tools/check-deps.sh to inspect missing runtime dependencies.", file=sys.stderr)
    print(f"Summary JSON written to {json_out}")

if not ok:
    sys.exit(1)
if fail_on_skip and (total_skipped > 0 or len(warnings) > 0):
    sys.exit(2)
sys.exit(0)
PY
