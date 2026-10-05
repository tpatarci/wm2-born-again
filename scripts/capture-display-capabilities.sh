#!/usr/bin/env bash
set -euo pipefail
#
# wm2-born-again display capability capture (D-05, XDIS-05, TEST-08).
#
#   bash scripts/capture-display-capabilities.sh <display> <label>
#
# Captures, in ONE pass, everything the signoff bundle needs to know about one
# X server: the extension matrix, the geometry, the RANDR state, the fontconfig
# resolutions the window manager actually asks for, the root EWMH properties the
# behaviour checklist names, and the full window tree -- plus the commit hash,
# branch, tool versions and timestamp that make the capture attributable to a
# tree ("Release Evidence Required", first two bullets).
#
# Output lands under a per-LABEL directory beneath the phase evidence directory,
# so one target can be diffed against another:
#
#   .planning/phases/08-.../evidence/<label>/capabilities.txt   human-readable summary
#   .planning/phases/08-.../evidence/<label>/root-properties.txt  xprop -root dumps
#   .planning/phases/08-.../evidence/<label>/window-tree.txt      xwininfo -root -tree
#
# Shell conventions follow scripts/preflight.sh and scripts/gates/build-all.sh:
# `set -euo pipefail` on line 2, every failure line prefixed `wm2: ` on stderr to
# match the binary's own convention.
#
# THREE DELIBERATE DESIGN POINTS:
#
#   1. A MISSING EXTENSION IS A RESULT, NOT A FAILURE. This script's entire
#      purpose is to characterise servers that lack RANDR, RENDER or Shape --
#      that is what XDIS-04 and XDIS-05 are about. Every probe therefore records
#      "absent" and continues; only an unusable display or an unwritable output
#      directory aborts the run.
#
#   2. NO SERVER FLAGS ARE SUGGESTED OR APPLIED. The script only ever READS from
#      a display someone else started. In particular it never proposes the X
#      access-control-disabling flag (threat T-8-AC); how the session was started
#      is the operator's business and stays out of this file so it cannot be
#      copied out of here into a deployment recipe.
#
#   3. THE TRANSCRIPT CONTAINS WINDOW TITLES. `xwininfo -root -tree` and
#      `xprop -root` dump whatever is on the display at capture time, and this
#      output is COMMITTED (threat T-8-REMOTE). Capture on a session started for
#      the run, and read the files before committing them.

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)

# As scripts/gates/build-all.sh does. The operator will typically run this from
# inside a remote-desktop session with an arbitrary working directory, and the
# provenance block below asks git about THIS repository, not about wherever the
# shell happened to be.
cd "$REPO_ROOT"

PHASE_DIR="$REPO_ROOT/.planning/phases/08-xrandr-vnc-compatibility-focus-rules"
EVIDENCE_ROOT="${WM2_EVIDENCE_ROOT:-$PHASE_DIR/evidence}"

usage() {
    echo "usage: capture-display-capabilities.sh <display> <label>"
    echo "  <display>  an X display name, e.g. :99 or :2"
    echo "  <label>    a directory name for this target, e.g. tigervnc, xrdp, x2go, local-xephyr"
    echo ""
    echo "Environment:"
    echo "  WM2_EVIDENCE_ROOT   override the output root (default: the phase evidence directory)"
}

case "${1:-}" in
    -h|--help) usage; exit 0 ;;
esac

if [ $# -ne 2 ]; then
    echo "wm2: capture: expected exactly 2 arguments (display and label), got $#" >&2
    usage >&2
    exit 2
fi

DISPLAY_NAME="$1"
LABEL="$2"

# The label becomes a directory name and is used unquoted in paths below, so it
# is constrained rather than trusted.
if ! printf '%s' "$LABEL" | grep -qE '^[A-Za-z0-9][A-Za-z0-9._-]*$'; then
    echo "wm2: capture: label must match [A-Za-z0-9][A-Za-z0-9._-]* (got '$LABEL')" >&2
    exit 2
fi

OUT_DIR="$EVIDENCE_ROOT/$LABEL"
CAPS="$OUT_DIR/capabilities.txt"
PROPS="$OUT_DIR/root-properties.txt"
TREE="$OUT_DIR/window-tree.txt"

mkdir -p "$OUT_DIR"

# ---------------------------------------------------------------------------
# Helpers
#
# `probe` runs a command against the target display and never lets a non-zero
# exit abort the capture (design point 1). It records the exit status so a
# reader can tell "the tool said no" from "the tool was not installed".
# ---------------------------------------------------------------------------

probe() {
    DISPLAY="$DISPLAY_NAME" "$@" 2>&1 || echo "  (command exited $? -- recorded, not fatal)"
}

have() { command -v "$1" >/dev/null 2>&1; }

section() {
    echo ""
    echo "== $* =="
}

# ---------------------------------------------------------------------------
# 0. The display must be usable at all. This is the ONE fatal precondition.
# ---------------------------------------------------------------------------

if ! have xdpyinfo; then
    echo "wm2: capture: xdpyinfo not found on PATH (install x11-utils)" >&2
    exit 1
fi

if ! DISPLAY="$DISPLAY_NAME" xdpyinfo >/dev/null 2>&1; then
    echo "wm2: capture: cannot open display '$DISPLAY_NAME'" >&2
    exit 1
fi

DPYINFO=$(DISPLAY="$DISPLAY_NAME" xdpyinfo 2>&1)

# ---------------------------------------------------------------------------
# The capabilities summary. Everything below writes to stdout, redirected once.
# ---------------------------------------------------------------------------

{
    echo "wm2-born-again display capability capture"
    echo "========================================="
    echo ""

    # -- provenance (Release Evidence Required, bullets 1 and 2) -------------
    section "provenance"
    echo "  label:        $LABEL"
    echo "  display:      $DISPLAY_NAME"
    echo "  timestamp:    $(date -u +'%Y-%m-%dT%H:%M:%SZ') (UTC)"
    # NO HOSTNAME. This output is COMMITTED to a public repository, and
    # `uname -n` on a workstation is that machine's name -- a host identifier,
    # which is a standing prohibition here since 08.5 and is restated in plan
    # 09-09's own prohibitions. It emitted one until that plan's evidence
    # capture found it. The kernel and distro lines stay: they are what makes a
    # capture attributable to a PLATFORM, which is the question this block
    # exists to answer, and neither names the machine.
    echo "  kernel:       $(uname -sr)"
    if [ -r /etc/os-release ]; then
        # shellcheck disable=SC1091
        echo "  distro:       $(. /etc/os-release && printf '%s' "${PRETTY_NAME:-unknown}")"
    else
        echo "  distro:       unknown (/etc/os-release not readable)"
    fi
    echo "  commit:       $(git rev-parse HEAD 2>/dev/null || echo 'unknown')"
    echo "  commit-short: $(git rev-parse --short HEAD 2>/dev/null || echo 'unknown')"
    echo "  branch:       $(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo 'unknown')"
    echo "  tree-state:   $(test -z "$(git status --porcelain 2>/dev/null)" && echo clean || echo DIRTY)"

    section "tool versions"
    for tool in cmake c++ pkg-config xdpyinfo xprop xwininfo xrandr fc-match; do
        if have "$tool"; then
            case "$tool" in
                cmake)      echo "  cmake         $(cmake --version | head -1)" ;;
                c++)        echo "  c++           $(c++ --version | head -1)" ;;
                pkg-config) echo "  pkg-config    $(pkg-config --version)" ;;
                fc-match)   echo "  fc-match      $(fc-match --version 2>&1 | head -1)" ;;
                *)          echo "  $(printf '%-13s' "$tool")$(command -v "$tool")" ;;
            esac
        else
            echo "  $(printf '%-13s' "$tool")NOT INSTALLED"
        fi
    done

    section "X library versions (pkg-config)"
    for mod in x11 xext xft fontconfig xrandr xrender; do
        if pkg-config --exists "$mod" 2>/dev/null; then
            echo "  $(printf '%-12s' "$mod") $(pkg-config --modversion "$mod")"
        else
            echo "  $(printf '%-12s' "$mod") NOT AVAILABLE"
        fi
    done

    # -- the XDIS-05 extension matrix ---------------------------------------
    #
    # THESE THREE LINES ARE THE MATRIX. One line each, in a fixed
    # "EXTENSION <name>: present|ABSENT" shape, so one target diffs cleanly
    # against another and a reader does not have to interpret a raw list.
    section "extension matrix (XDIS-05)"

    EXT_LIST=$(printf '%s\n' "$DPYINFO" \
        | sed -n '/number of extensions:/,/^default screen number/p' \
        | sed '1d;$d' \
        | sed 's/^ *//' | grep -v '^$' || true)

    ext_state() {
        # Matched case-insensitively on a whole line: the SHAPE extension is
        # advertised as "SHAPE", RANDR as "RANDR", RENDER as "RENDER", but
        # server implementations have historically varied in case.
        if printf '%s\n' "$EXT_LIST" | grep -qix "$1"; then
            echo "present"
        else
            echo "ABSENT"
        fi
    }

    echo "  EXTENSION SHAPE:  $(ext_state SHAPE)"
    echo "  EXTENSION RANDR:  $(ext_state RANDR)"
    echo "  EXTENSION RENDER: $(ext_state RENDER)"
    echo ""
    echo "  What each absence costs this window manager:"
    echo "    SHAPE absent  -> rectangular frame fallback; no Shape requests are issued"
    echo "                     (src/Border.cpp combineShape funnel, plan 08-03)"
    echo "    RANDR absent  -> no resolution-change reflow; geometry is read once at startup"
    echo "                     (src/Manager.cpp, plan 08-05)"
    echo "    RENDER absent -> libXft falls back to core X11 glyphs; the sideways tab still"
    echo "                     renders rotated (proved in plan 08-06)"

    section "full extension list ($(printf '%s\n' "$EXT_LIST" | grep -c . || true) advertised)"
    printf '%s\n' "$EXT_LIST" | sed 's/^/  /'

    # -- geometry -----------------------------------------------------------
    section "geometry, resolution and root depth"
    printf '%s\n' "$DPYINFO" | grep -E 'name of display|version number|vendor string|vendor release number|dimensions:|resolution:|depth of root window|number of screens|default screen number' | sed 's/^ */  /'
    echo ""
    echo "  --- per-screen block ---"
    printf '%s\n' "$DPYINFO" | sed -n '/^screen #/,/^  number of visuals/p' | sed 's/^/  /'

    # -- RANDR --------------------------------------------------------------
    #
    # Tolerated and RECORDED when missing, per the plan: a server without RANDR
    # is a supported target, not a broken capture.
    section "RANDR state"
    if [ "$(ext_state RANDR)" = "ABSENT" ]; then
        echo "  RANDR is ABSENT from this server's extension list."
        echo "  Recording the absence rather than aborting -- this is a supported"
        echo "  configuration (XDIS-02 no-RANDR fallback, WM2_FORCE_NO_RANDR lever)."
        echo ""
        echo "  xrandr output anyway, for the record:"
        if have xrandr; then
            probe xrandr --query | sed 's/^/    /'
        else
            echo "    xrandr not installed"
        fi
    elif have xrandr; then
        echo "  --- xrandr --query ---"
        probe xrandr --query | sed 's/^/  /'
        echo ""
        echo "  --- xrandr --listmonitors ---"
        probe xrandr --listmonitors | sed 's/^/  /'
    else
        echo "  RANDR is present on the server but xrandr(1) is not installed here."
    fi

    # -- fontconfig ---------------------------------------------------------
    #
    # The two shipped defaults the WM asks for first, plus the generic rung each
    # of its ladders falls back to. A remote target that resolves none of these
    # has no window titles at all, which is the failure mode the checklist's Xft
    # line is about. The two generic rungs keep their point size on purpose:
    # decision 09-01 froze those literals, because a fallback the user can also
    # break is not a fallback.
    section "fontconfig resolution (the patterns the WM actually requests)"
    if have fc-match; then
        echo "  menu font (the menu-font default in include/Config.h):"
        echo "    'DejaVu Sans:pixelsize=13'"
        echo "      -> $(fc-match --format='%{family} :: %{file}' 'DejaVu Sans:pixelsize=13' 2>&1 || echo 'UNRESOLVED')"
        echo "  tab font, rotated (the tab-font default in include/Config.h):"
        echo "    'DejaVu Sans:bold:pixelsize=13'"
        echo "      -> $(fc-match --format='%{family} :: %{file}' 'DejaVu Sans:bold:pixelsize=13' 2>&1 || echo 'UNRESOLVED')"
        echo "  generic fallback rungs (src/Manager.cpp:628, src/Border.cpp:190):"
        echo "    'sans-serif:size=12'"
        echo "      -> $(fc-match --format='%{family} :: %{file}' 'sans-serif:size=12' 2>&1 || echo 'UNRESOLVED')"
        echo "    'sans-serif:bold:size=12'"
        echo "      -> $(fc-match --format='%{family} :: %{file}' 'sans-serif:bold:size=12' 2>&1 || echo 'UNRESOLVED')"
    else
        echo "  fc-match NOT INSTALLED -- fontconfig resolution could not be captured."
    fi

    # -- root EWMH ----------------------------------------------------------
    section "root EWMH properties (summary; full dump in root-properties.txt)"
    if have xprop; then
        probe xprop -root _NET_SUPPORTING_WM_CHECK _NET_SUPPORTED _NET_NUMBER_OF_DESKTOPS \
            _NET_CURRENT_DESKTOP _NET_WORKAREA _NET_CLIENT_LIST _NET_ACTIVE_WINDOW \
            | sed 's/^/  /'
    else
        echo "  xprop NOT INSTALLED -- EWMH properties could not be captured."
    fi

    section "window tree (summary; full dump in window-tree.txt)"
    if have xwininfo; then
        probe xwininfo -root -children | sed 's/^/  /'
    else
        echo "  xwininfo NOT INSTALLED -- the window tree could not be captured."
    fi

    echo ""
    echo "== end of capture =="
} > "$CAPS"

# ---------------------------------------------------------------------------
# The two dumps that get their own files, so a reviewer can diff one target
# against another without the provenance header changing every line.
# ---------------------------------------------------------------------------

{
    echo "# root properties -- $LABEL -- $DISPLAY_NAME -- $(date -u +'%Y-%m-%dT%H:%M:%SZ')"
    echo "# commit $(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo ""
    echo "## the EWMH properties the behaviour checklist names"
    if have xprop; then
        probe xprop -root _NET_SUPPORTING_WM_CHECK _NET_SUPPORTED _NET_NUMBER_OF_DESKTOPS \
            _NET_CURRENT_DESKTOP _NET_WORKAREA _NET_CLIENT_LIST _NET_ACTIVE_WINDOW
        echo ""
        echo "## every property on root"
        probe xprop -root
    else
        echo "xprop NOT INSTALLED"
    fi
} > "$PROPS"

{
    echo "# window tree -- $LABEL -- $DISPLAY_NAME -- $(date -u +'%Y-%m-%dT%H:%M:%SZ')"
    echo "# commit $(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo ""
    if have xwininfo; then
        probe xwininfo -root -tree
    else
        echo "xwininfo NOT INSTALLED"
    fi
} > "$TREE"

# ---------------------------------------------------------------------------
# Result
# ---------------------------------------------------------------------------

for f in "$CAPS" "$PROPS" "$TREE"; do
    if [ ! -s "$f" ]; then
        echo "wm2: capture: produced an EMPTY file: $f" >&2
        exit 1
    fi
done

echo "capture OK: $LABEL ($DISPLAY_NAME)"
echo "  $CAPS"
echo "  $PROPS"
echo "  $TREE"
grep -E '^  EXTENSION ' "$CAPS" | sed 's/^ */  /'
exit 0
