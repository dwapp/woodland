#!/usr/bin/env bash
# Check build and runtime dependencies for Woodland (especially optional smoke
# tools whose absence causes Meson tests to SKIP or silently degrade).
set -uo pipefail

JSON_MODE=0
STRICT_MODE=0

usage() {
    cat <<'EOF'
Usage: check-deps.sh [--json] [--strict] [-h|--help]

Check required build tools/libraries, optional protocol/build dependencies,
smoke-test runtime tools, and local GPU/DRM render nodes.

Options:
  --json     Print machine-readable JSON only to stdout
  --strict   Exit non-zero if any optional smoke/protocol dependency is missing
  -h, --help Show this help
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
    --json)
        JSON_MODE=1
        ;;
    --strict)
        STRICT_MODE=1
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        echo "check-deps.sh: unknown option '$1'" >&2
        usage >&2
        exit 2
        ;;
    esac
    shift
done

HAVE_PKG_CONFIG=0
command -v pkg-config >/dev/null 2>&1 && HAVE_PKG_CONFIG=1

# Store check records as tab-delimited lines in a temp file so we never lose
# entries across subshells and can render cleanly in bash or python3.
TMP_RECORDS="$(mktemp)"
trap 'rm -f "$TMP_RECORDS"' EXIT

record_check() {
    # $1=name $2=category(required|optional) $3=kind(tool|pkg|file|group)
    # $4=status(ok|miss) $5=detail $6=affects_csv $7=hint
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$1" "$2" "$3" "$4" "$5" "$6" "$7" >>"$TMP_RECORDS"
}

check_tool() {
    local name="$1" category="$2" affects="$3" hint="$4"
    local path=""
    if path="$(command -v "$name" 2>/dev/null)"; then
        record_check "$name" "$category" "tool" "ok" "$path" "$affects" "$hint"
    else
        record_check "$name" "$category" "tool" "miss" "not found in PATH" "$affects" "$hint"
    fi
}

check_pkg() {
    local name="$1" category="$2" affects="$3" hint="$4"
    if [ "$HAVE_PKG_CONFIG" -eq 0 ]; then
        record_check "$name" "$category" "pkg" "miss" "pkg-config unavailable" "$affects" "$hint"
        return
    fi
    if pkg-config --exists "$name" 2>/dev/null; then
        local ver
        ver="$(pkg-config --modversion "$name" 2>/dev/null || echo "found")"
        record_check "$name" "$category" "pkg" "ok" "$ver" "$affects" "$hint"
    else
        record_check "$name" "$category" "pkg" "miss" "not found by pkg-config" "$affects" "$hint"
    fi
}

check_file() {
    local name="$1" path="$2" category="$3" affects="$4" hint="$5"
    if [ -e "$path" ]; then
        record_check "$name" "$category" "file" "ok" "$path" "$affects" "$hint"
    else
        record_check "$name" "$category" "file" "miss" "missing $path" "$affects" "$hint"
    fi
}

# 1. Build tools (required)
check_tool "meson" "required" "build" "Arch: meson | Debian: meson"
check_tool "ninja" "required" "build" "Arch: ninja | Debian: ninja-build"
if cc_path="$(command -v cc 2>/dev/null || command -v gcc 2>/dev/null)"; then
    record_check "cc" "required" "tool" "ok" "$cc_path" "build" "Arch: gcc | Debian: gcc"
else
    record_check "cc" "required" "tool" "miss" "neither cc nor gcc found" "build" "Arch: gcc | Debian: gcc"
fi
check_tool "pkg-config" "required" "build" "Arch: pkgconf | Debian: pkg-config"
check_tool "wayland-scanner" "required" "build" "Arch: wayland | Debian: wayland-scanner"

# 2. Build libraries (required via pkg-config)
check_pkg "wayland-server" "required" "build" "Arch: wayland | Debian: libwayland-dev"
check_pkg "wayland-client" "required" "build" "Arch: wayland | Debian: libwayland-dev"
check_pkg "xkbcommon" "required" "build" "Arch: libxkbcommon | Debian: libxkbcommon-dev"
check_pkg "cairo" "required" "build" "Arch: cairo | Debian: libcairo2-dev"
check_pkg "pangocairo" "required" "build" "Arch: pango | Debian: libpango1.0-dev"
check_pkg "pixman-1" "required" "build" "Arch: pixman | Debian: libpixman-1-dev"
check_pkg "libdrm" "required" "build" "Arch: libdrm | Debian: libdrm-dev"
check_pkg "libinput" "required" "build" "Arch: libinput | Debian: libinput-dev"

# 3. Optional build / protocol deps
check_pkg "wayland-protocols" "optional" "protocol-probes,most-smoke-tests" \
    "Arch: wayland-protocols | Debian: wayland-protocols"
check_pkg "xcb" "optional" "xoverride,smoke-xwayland,smoke-xwindow-control(real-x11)" \
    "Arch: libxcb | Debian: libxcb1-dev"
check_pkg "librsvg-2.0" "optional" "svg-icon-rendering" \
    "Arch: librsvg | Debian: librsvg2-dev"
check_pkg "xwayland" "optional" "x11-runtime-support" \
    "Arch: xorg-xwayland | Debian: xwayland"
check_file "wlr-output-management-xml" \
    "/usr/share/wlr-protocols/unstable/wlr-output-management-unstable-v1.xml" \
    "optional" "smoke-output-mgmt-order" \
    "Arch: wlr-protocols | Debian: wlr-protocols"

# 4. Smoke runtime tools
check_tool "jq" "optional" \
    "smoke-ipc,smoke-debug-ctl,smoke-assert,smoke-deco,smoke-bg-effect,smoke-appearance,smoke-shortcut,smoke-shortcutd,smoke-ftl,smoke-security-context,smoke-dnd,smoke-multitask,smoke-switcher,smoke-compositor-action,smoke-xwayland,smoke-xwindow-control" \
    "Arch: jq | Debian: jq"
check_tool "wayland-info" "optional" "smoke-protocol" \
    "Arch: wayland-utils | Debian: wayland-utils"
check_tool "wl-copy" "optional" "smoke-protocol(primary-selection)" \
    "Arch: wl-clipboard | Debian: wl-clipboard"
check_tool "wl-paste" "optional" "smoke-protocol(primary-selection)" \
    "Arch: wl-clipboard | Debian: wl-clipboard"
check_tool "wlr-randr" "optional" "smoke-output-mgmt" \
    "Arch: wlr-randr | Debian: wlr-randr"
check_tool "grim" "optional" "smoke-capture,smoke-xwayland,failure-screenshots" \
    "Arch: grim | Debian: grim"
check_tool "lswt" "optional" "smoke-capture,smoke-xwayland" \
    "Arch: AUR lswt (https://git.sr.ht/~leon_plickat/lswt)"
check_tool "weston-simple-shm" "optional" "smoke-capture" \
    "Arch: weston | Debian: weston"
check_tool "file" "optional" "smoke-capture,smoke-xwayland" \
    "Arch: file | Debian: file"
check_tool "Xwayland" "optional" "smoke-xwayland,smoke-xwindow-control(real-x11)" \
    "Arch: xorg-xwayland | Debian: xwayland"

# xterm (with /dev/ptmx) OR xlogo satisfies smoke-xwayland's managed X11 app
if [ -e /dev/ptmx ] && xterm_path="$(command -v xterm 2>/dev/null)"; then
    record_check "xterm-or-xlogo" "optional" "group" "ok" "xterm ($xterm_path)" \
        "smoke-xwayland" "Arch: xterm or xorg-xlogo | Debian: xterm or x11-apps"
elif xlogo_path="$(command -v xlogo 2>/dev/null)"; then
    record_check "xterm-or-xlogo" "optional" "group" "ok" "xlogo ($xlogo_path)" \
        "smoke-xwayland" "Arch: xterm or xorg-xlogo | Debian: xterm or x11-apps"
else
    if command -v xterm >/dev/null 2>&1 && [ ! -e /dev/ptmx ]; then
        detail="xterm present but /dev/ptmx unavailable, and xlogo missing"
    else
        detail="neither xterm (with /dev/ptmx) nor xlogo found"
    fi
    record_check "xterm-or-xlogo" "optional" "group" "miss" "$detail" \
        "smoke-xwayland" "Arch: xterm or xorg-xlogo | Debian: xterm or x11-apps"
fi

check_tool "convert" "optional" "smoke-xwayland" \
    "Arch: imagemagick | Debian: imagemagick"
check_tool "identify" "optional" "smoke-xwayland" \
    "Arch: imagemagick | Debian: imagemagick"
check_tool "nc" "optional" "smoke-ipc(pipelined-transport)" \
    "Arch: openbsd-netcat | Debian: netcat-openbsd"
check_tool "python3" "optional" "smoke-ipc(stalled-client),tools/verify.sh" \
    "Arch: python | Debian: python3"

# 5. GPU / DRM render node capability
GPU_DEVICES=()
for dev in /dev/dri/renderD* /dev/udmabuf; do
    if [ -e "$dev" ]; then
        GPU_DEVICES+=("$dev")
    fi
done
GPU_AVAILABLE=0
if [ "${#GPU_DEVICES[@]}" -gt 0 ]; then
    GPU_AVAILABLE=1
fi

if [ "$JSON_MODE" -eq 1 ]; then
    if command -v python3 >/dev/null 2>&1; then
        python3 - "$TMP_RECORDS" "$GPU_AVAILABLE" "${GPU_DEVICES[@]+"${GPU_DEVICES[@]}"}" <<'PY'
import json
import sys

records_path = sys.argv[1]
gpu_available = sys.argv[2] == "1"
gpu_devices = sys.argv[3:]

checks = []
missing_required = []
missing_optional = []

with open(records_path, "r", encoding="utf-8") as fh:
    for line in fh:
        line = line.rstrip("\n")
        if not line:
            continue
        name, category, kind, status, detail, affects_csv, hint = line.split("\t")
        affects = [a for a in affects_csv.split(",") if a]
        checks.append({
            "name": name,
            "category": category,
            "kind": kind,
            "ok": status == "ok",
            "detail": detail,
            "affects": affects,
            "hint": hint,
        })
        if status != "ok":
            if category == "required":
                missing_required.append(name)
            else:
                missing_optional.append({
                    "name": name,
                    "kind": kind,
                    "detail": detail,
                    "affects": affects,
                    "hint": hint,
                })

ok = len(missing_required) == 0
strict_ok = ok and len(missing_optional) == 0
out = {
    "ok": ok,
    "strict_ok": strict_ok,
    "missing_required": missing_required,
    "missing_optional": missing_optional,
    "gpu": {
        "available": gpu_available,
        "devices": gpu_devices,
    },
    "checks": checks,
}
print(json.dumps(out, ensure_ascii=False, indent=2))
sys.exit(0 if ok else 1)
PY
        rc=$?
        if [ "$rc" -ne 0 ]; then
            exit 1
        fi
        if [ "$STRICT_MODE" -eq 1 ]; then
            if grep -q $'\tmiss\t' "$TMP_RECORDS"; then
                exit 1
            fi
        fi
        exit 0
    else
        echo '{"ok":false,"error":"python3 required for --json output"}'
        exit 1
    fi
fi

# Human-readable output
miss_req=0
miss_opt=0

echo "=== Required Build Dependencies ==="
while IFS=$'\t' read -r name cat kind status detail affects hint; do
    [ "$cat" = "required" ] || continue
    if [ "$status" = "ok" ]; then
        printf "  [OK]   %-24s %s\n" "$name" "$detail"
    else
        printf "  [MISS] %-24s %s  (%s)\n" "$name" "$detail" "$hint"
        miss_req=$((miss_req + 1))
    fi
done <"$TMP_RECORDS"

echo ""
echo "=== Optional Build / Smoke Dependencies ==="
while IFS=$'\t' read -r name cat kind status detail affects hint; do
    [ "$cat" = "optional" ] || continue
    if [ "$status" = "ok" ]; then
        printf "  [OK]   %-24s %s\n" "$name" "$detail"
    else
        printf "  [WARN] %-24s %s -> affects: %s  (%s)\n" "$name" "$detail" "$affects" "$hint"
        miss_opt=$((miss_opt + 1))
    fi
done <"$TMP_RECORDS"

echo ""
echo "=== GPU / DRM Allocator Capability (for -Deffects=enabled smoke) ==="
if [ "$GPU_AVAILABLE" -eq 1 ]; then
    echo "  [OK]   render nodes available: ${GPU_DEVICES[*]}"
else
    echo "  [WARN] no /dev/dri/renderD* or /dev/udmabuf found; -Deffects=enabled smoke tests cannot allocate buffers (use -Deffects=disabled)"
fi

echo ""
echo "Summary: required_missing=$miss_req, optional_missing=$miss_opt, gpu_available=$GPU_AVAILABLE"

if [ "$miss_req" -gt 0 ]; then
    exit 1
fi
if [ "$STRICT_MODE" -eq 1 ] && [ "$miss_opt" -gt 0 ]; then
    exit 1
fi
exit 0
