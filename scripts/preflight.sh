#!/usr/bin/env bash
set -euo pipefail
#
# wm2-born-again environment preflight (D-01, D-02, TEST-07).
#
# Verifies every declared build and test dependency resolves on THIS host, right
# now. It makes no claim about other hosts, nor about versions drifting later.
#
# Runs two ways, with identical checks:
#   - standalone:      bash scripts/preflight.sh
#   - ctest fixture:   registered as a FIXTURES_SETUP test so the suite fails
#                      with a named cause before any behavioral assertion runs
#                      (D-02). Deliberately NOT a CMake configure-time check --
#                      that would block plain binary builds on machines that
#                      never run tests.
#
# All checks are accumulated rather than aborting on the first failure, so an
# operator sees the full picture in one run.
#
# Shell conventions established here (this is the project's first shell script):
# `set -euo pipefail`, and every failure line prefixed `wm2: ` on stderr to match
# the binary's own convention (src/Manager.cpp). The fail-fast options are set on
# line 2, immediately after the shebang, so nothing can run before them.

QUIET=0
for arg in "$@"; do
    case "$arg" in
        --quiet) QUIET=1 ;;
        -h|--help)
            echo "usage: preflight.sh [--quiet]"
            echo "  --quiet   suppress informational version lines; still print failures"
            exit 0
            ;;
        *)
            echo "wm2: preflight: unknown argument: $arg" >&2
            exit 2
            ;;
    esac
done

FAILURES=0

fail() {
    echo "wm2: $*" >&2
    FAILURES=$((FAILURES + 1))
}

info() {
    if [ "$QUIET" -eq 0 ]; then
        echo "$*"
    fi
}

section() {
    if [ "$QUIET" -eq 0 ]; then
        echo ""
        echo "== $* =="
    fi
}

# ---------------------------------------------------------------------------
# 1. pkg-config modules
# ---------------------------------------------------------------------------

section "pkg-config modules"

# xrender is checked explicitly rather than relied on transitively through Xft,
# because XDIS-04's RENDER-less fallback work needs it named in its own right.
for mod in x11 xext xft fontconfig xrandr xrender; do
    if pkg-config --exists "$mod" 2>/dev/null; then
        info "  $(printf '%-12s' "$mod") $(pkg-config --modversion "$mod")"
    else
        fail "pkg-config module missing: $mod (install the matching -dev package)"
    fi
done

# OPTIONAL modules (D-18). A missing one is reported with its CONSEQUENCE and
# never with fail(): an optional dependency that fails the preflight is not
# optional, and this script's whole job is to be the named cause a builder can
# act on -- which includes telling somebody why they got no settings window
# without telling them their environment is broken.
#
# Same `info` line shape as the required loop above when the module is present,
# so a reader does not have to learn a second format to read one list.
for mod in gtk+-3.0; do
    if pkg-config --exists "$mod" 2>/dev/null; then
        info "  $(printf '%-12s' "$mod") $(pkg-config --modversion "$mod")  (optional: builds wm2-config)"
    else
        info "  $(printf '%-12s' "$mod") not found  (optional: without it wm2-config, the settings window, is not built; the window manager and wm2-ctl are unaffected)"
    fi
done

# ---------------------------------------------------------------------------
# 2. Toolchain versions (recorded for signoff evidence) + the CMake floor
# ---------------------------------------------------------------------------

section "toolchain"

if command -v cmake >/dev/null 2>&1; then
    CMAKE_VERSION_LINE=$(cmake --version | head -1)
    info "  $CMAKE_VERSION_LINE"

    CMAKE_VER=$(printf '%s' "$CMAKE_VERSION_LINE" | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1)
    CMAKE_MAJOR=${CMAKE_VER%%.*}
    CMAKE_REST=${CMAKE_VER#*.}
    CMAKE_MINOR=${CMAKE_REST%%.*}

    # Assert, do not merely record. This is the named cause that replaces the
    # opaque configure-time string(JSON) error a 3.16-3.18 builder would hit.
    if [ "$CMAKE_MAJOR" -lt 3 ] || { [ "$CMAKE_MAJOR" -eq 3 ] && [ "$CMAKE_MINOR" -lt 20 ]; }; then
        fail "cmake >= 3.20 required (ADD_TAGS_AS_LABELS needs string(JSON), --no-tests=error needs ctest 3.20); found ${CMAKE_MAJOR}.${CMAKE_MINOR}"
    fi
else
    fail "cmake not found on PATH"
fi

if command -v c++ >/dev/null 2>&1; then
    info "  $(c++ --version | head -1)"
else
    fail "c++ not found on PATH"
fi

if command -v pkg-config >/dev/null 2>&1; then
    info "  pkg-config $(pkg-config --version)"
else
    fail "pkg-config not found on PATH"
fi

if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    info "  distro: $(. /etc/os-release && printf '%s' "${PRETTY_NAME:-unknown}")"
else
    info "  distro: unknown (/etc/os-release not readable)"
fi

# ---------------------------------------------------------------------------
# 3. X11 tooling on PATH
# ---------------------------------------------------------------------------

section "X11 tooling"

for tool in Xvfb Xephyr xprop xwininfo xdotool xdpyinfo xrandr; do
    if command -v "$tool" >/dev/null 2>&1; then
        info "  $(printf '%-10s' "$tool") $(command -v "$tool")"
    else
        fail "X11 tool missing from PATH: $tool"
    fi
done

# ---------------------------------------------------------------------------
# 4. At least one simple X client
#
# The checklist explicitly forbids relying on xterm unless it is a declared
# dependency, so xterm is reported as informational only and can never satisfy
# this check on its own.
# ---------------------------------------------------------------------------

section "simple X client"

SIMPLE_CLIENT=""
for client in xclock xmessage; do
    if command -v "$client" >/dev/null 2>&1; then
        SIMPLE_CLIENT="$client"
        info "  $(printf '%-10s' "$client") $(command -v "$client")"
    fi
done

if [ -z "$SIMPLE_CLIENT" ]; then
    fail "no simple X client found (need one of: xclock, xmessage); install x11-apps"
fi

if command -v xterm >/dev/null 2>&1; then
    info "  xterm      $(command -v xterm)  (informational: does not satisfy this check)"
else
    info "  xterm      not installed (informational: not required; tests use xclock)"
fi

# ---------------------------------------------------------------------------
# 5. fontconfig fallback chains
#
# The exact two patterns the WM uses for its menu and rotated tab fonts.
# ---------------------------------------------------------------------------

section "fontconfig fallback chains"

if command -v fc-match >/dev/null 2>&1; then
    # The third pattern is the generic rung of the tab-font ladder in
    # src/Border.cpp (XDIS-04). It is checked here rather than asserted in a
    # comment, because that rung is what stands between a font-poor target and
    # a window manager with no labels at all.
    # The first two are the shipped menu-font and tab-font defaults from
    # include/Config.h, sized in pixels since quick task 261004-vp6.
    for pattern in "DejaVu Sans:pixelsize=13" \
                   "DejaVu Sans:bold:pixelsize=13" \
                   "sans-serif:bold:size=12"; do
        # fc-match exits 0 even when it resolves nothing useful, so require a
        # non-empty result naming a real font file.
        if MATCH=$(fc-match "$pattern" 2>/dev/null) && [ -n "$MATCH" ]; then
            MATCH_FILE=$(fc-match --format=%{file} "$pattern" 2>/dev/null || true)
            if [ -n "$MATCH_FILE" ] && [ -r "$MATCH_FILE" ]; then
                info "  '$pattern' -> $MATCH_FILE"
            else
                fail "fontconfig pattern resolved to no readable font file: '$pattern'"
            fi
        else
            fail "fontconfig could not resolve pattern: '$pattern'"
        fi
    done
else
    fail "fc-match not found on PATH (install fontconfig)"
fi

# ---------------------------------------------------------------------------
# Result
# ---------------------------------------------------------------------------

if [ "$FAILURES" -ne 0 ]; then
    echo "wm2: preflight FAILED with $FAILURES problem(s); see the wm2: lines above" >&2
    exit 1
fi

info ""
info "preflight OK"
exit 0
