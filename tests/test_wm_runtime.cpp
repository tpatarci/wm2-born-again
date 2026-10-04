// Config-to-runtime, terminating-error-path and stress tests against the REAL
// compiled wm2-born-again binary (plan 08-13, TEST-05).
//
// This file closes the last three "Missing Automated Coverage To Add" entries in
// COMPILED_CODE_BEHAVIOR_CHECKLIST.md:
//
//   item 5   config settings reaching observable runtime behaviour -> [wm_config_runtime]
//   item 6   the four terminating X11 error paths                  -> [wm_errors]
//   item 13  100-window create/map/unmap/destroy churn under ASan   -> [wm_stress]
//
// plus a [wm_harness] case for the fixture's own stale-report cleanup
// (deferred item 14), which is test infrastructure rather than WM behaviour and
// is tagged separately for that reason.
//
// Tags are registered as ctest LABELS via ADD_TAGS_AS_LABELS (D-33), so each
// group is selectable with `ctest -L '^wm_config_runtime$' --no-tests=error`.
//
// The rules inherited from 08-04 .. 08-12, none of them optional:
//
//   CONFIG IS OBSERVED THROUGH THE RUNNING BINARY, NEVER THROUGH THE PARSER.
//   That is the entire point of this group. tests/test_config.cpp already proves
//   the parser assigns the right member; a setting that parses and then reaches
//   nothing passes every one of those cases. That is exactly the defect plan
//   08-07 found in the focus booleans, which had been dead config since the
//   Config struct was written.
//
//   OUTCOMES ARE OBSERVED THROUGH THE SERVER. Frame geometry through
//   XGetGeometry + XTranslateCoordinates; colours through a server-side
//   XGetImage over the root window; spawned programs through the class hint of
//   the window that appears; the delete path through the WM_DELETE_WINDOW
//   message the client actually receives.
//
//   NON-EVENTS ARE PROVEN BY SETTLING FIRST (deferred item 9). The WM does not
//   flush its X output until its event loop wakes, so a single read after a
//   single nudge reliably observes the PREVIOUS state and a negative assertion
//   passes for the wrong reason. Every "this did NOT happen" read here runs
//   settleWm() first -- fifteen SPACED nudges. The spacing is as load-bearing as
//   the count; a tight loop of the same length still read stale values.
//
//   NO sleep()-BASED SYNCHRONISATION. Every wait is a deadline-bounded poll
//   whose exit condition is a real observation. The one deliberate wall-clock
//   interval in this file is the long press in the destroy-window-delay case,
//   where the elapsed time IS the thing under test; it is spelled as a deadline
//   loop (holdFor) so it still never blocks the driver's connection.
//
//   THE WM SURVIVES X PROTOCOL ERRORS. WindowManager::errorHandler() logs and
//   returns 0, so no assertion about windows, properties or geometry can notice
//   one. 08-11 added the stderr assertion below and found a RenderBadPicture on
//   every managed window close that 243 tests had walked past.
//
// Deferred item 7 is accounted for rather than worked around: the WM adopts its
// own menu, submenu and EWMH check windows as clients, so _NET_CLIENT_LIST
// always carries entries this file did not create. List assertions here filter
// to the windows the case owns.
//
// The programs this file launches are chosen from what scripts/preflight.sh
// verifies, never from the shipped default command -- the checklist warns
// explicitly that the shipped default may not be installed on a target host.

#include <catch2/catch_test_macros.hpp>

#include "support/WmFixture.h"
#include "support/XTestDriver.h"
#include "support/PixelVerdict.h"

#include "x11wrap.h"
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include <X11/Xutil.h>

// dirent.h and fcntl.h are included for this file's OWN uses -- opendir(),
// readdir(), struct dirent, and the O_WRONLY passed to open(). They arrived
// transitively until now, which compiles but is not something this translation
// unit is entitled to rely on: a header it does not control dropping an
// include it never promised would break the build for reasons nothing here
// names. Same reasoning for <sstream> and <stdexcept> below.
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace wm2test;

namespace {

// The Xvfb geometry WmFixture starts every display with.
constexpr int kScreenW = 1024;
constexpr int kScreenH = 768;

// A parking spot for the pointer clear of every window this file maps. Set
// BEFORE anything is mapped: the pointer starts at the screen centre, and a
// window mapped underneath it would generate an EnterNotify that races the case.
constexpr int kParkX = 5;
constexpr int kParkY = 5;

// The shipped default frame thickness (include/Config.h). Named rather than
// spelled 7 at each use so the expectation reads as "the default" where that is
// what is meant.
constexpr int kDefaultFrameThickness = 7;

// A guaranteed-present X client. scripts/preflight.sh fails the whole suite if
// neither this nor its alternate is installed, so depending on it here cannot
// produce a mysterious failure -- the preflight fixture names the cause first.
const char* const kProbeProgram = "xclock";
const char* const kProbeClass   = "XClock";

// How long to wait for a program the WM spawned to appear as a managed window.
//
// Generous on purpose. A deadline here exists to stop a broken case hanging the
// suite forever, NOT to assert that spawning is fast: the chain is fork, fork,
// exec, an X connection, a map request, and the WM framing it -- and under the
// sanitizer, with the whole 283-case suite running, every link of that is
// slower. MEASURED: this case took 16.4 s against a 15 s deadline in one
// full-suite ASan run while passing in 0.3 s on its own.
constexpr int kSpawnObserveMs = 45000;

// The matching wait for a spawn that must NOT happen. Shorter, because nothing
// is coming -- but not trivially short: a window that was merely slow must not
// be mistaken for a window that never existed. The filesystem witness is the
// primary evidence in that case regardless.
constexpr int kSpawnAbsentMs = 15000;

// ---------------------------------------------------------------------------
// Xlib's own error handler
//
// The DEFAULT one calls exit(1). Several helpers below walk the window tree
// with XQueryTree and then ask each child for its geometry or attributes, and
// between those two round trips a window can legitimately disappear -- the
// nudge windows settleWm() creates and destroys are doing precisely that, over
// and over, and so is every client the churn case tears down. A raced
// XGetGeometry then killed the TEST PROCESS with no assertion output at all,
// which reads as an unexplained failure rather than as a race.
//
// MEASURED before this was installed: 2 flakes in 4 runs of the 15-case gate,
// in two different cases, both `BadDrawable` on X_GetGeometry with serial 16.
//
// The helpers already handle the failure correctly -- XGetGeometry returns 0
// and they return false -- so this only lets them reach that code. Errors are
// COUNTED rather than merely swallowed, so a helper that started erroring
// systematically would still be visible instead of silently returning nothing.
//
// XSetErrorHandler is global to Xlib rather than per-connection, so this is
// installed once at static initialisation, before Catch2 runs.
// ---------------------------------------------------------------------------

int g_xErrorCount = 0;

int quietXErrorHandler(Display*, XErrorEvent*)
{
    ++g_xErrorCount;
    return 0;
}

struct QuietXErrors {
    QuietXErrors() { XSetErrorHandler(quietXErrorHandler); }
};

const QuietXErrors g_quietXErrors;

// ---------------------------------------------------------------------------
// Settling (deferred item 9)
// ---------------------------------------------------------------------------

// Wake the WM's event loop so it flushes its X output buffer. The nudge is
// override-redirect, so WindowManager::eventCreate returns immediately for it
// and it can never be managed or perturb an assertion.
void pumpWm(Display* d)
{
    XSetWindowAttributes attr;
    attr.override_redirect = True;
    Window nudge = XCreateWindow(d, DefaultRootWindow(d), -20, -20, 1, 1, 0,
                                 CopyFromParent, InputOnly, CopyFromParent,
                                 CWOverrideRedirect, &attr);
    XSync(d, False);
    XDestroyWindow(d, nudge);
    XSync(d, False);
}

// Fifteen nudges at 20ms intervals -- the shape tests/test_wm_focus.cpp arrived
// at after a TIGHT loop of the same length still read the stale value.
void settleWm(Display* d)
{
    for (int i = 0; i < 15; ++i) {
        pumpWm(d);
        pollSleep();
    }
}

// A deliberate wall-clock interval, used only where the elapsed time is itself
// the thing under test (the long press against destroy-window-delay). Spelled
// as a deadline loop rather than a blocking call so it matches the rest of the
// file's waiting discipline and never blocks the input driver's connection.
void holdFor(int ms)
{
    const auto until = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < until) pollSleep();
}

// ---------------------------------------------------------------------------
// Server observation
// ---------------------------------------------------------------------------

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;

    bool operator==(const Rect& o) const
    {
        return x == o.x && y == o.y && w == o.w && h == o.h;
    }
    bool operator!=(const Rect& o) const { return !(*this == o); }
};

std::string describe(const Rect& r)
{
    return "(" + std::to_string(r.x) + "," + std::to_string(r.y) + " " +
           std::to_string(r.w) + "x" + std::to_string(r.h) + ")";
}

// Absolute rectangle of a window as the SERVER has it. XGetGeometry's x/y are
// parent-relative, so the translate is what makes this absolute for a window the
// WM has reparented into a frame.
bool serverRect(Display* d, Window w, Rect& out)
{
    Window rootRet = None;
    int x = 0, y = 0;
    unsigned int width = 0, height = 0, bw = 0, depth = 0;
    if (!XGetGeometry(d, w, &rootRet, &x, &y, &width, &height, &bw, &depth)) return false;

    int absX = 0, absY = 0;
    Window child = None;
    if (!XTranslateCoordinates(d, w, DefaultRootWindow(d), 0, 0, &absX, &absY, &child)) {
        return false;
    }
    out.x = absX;
    out.y = absY;
    out.w = static_cast<int>(width);
    out.h = static_cast<int>(height);
    return true;
}

Rect rectOf(Display* d, Window w)
{
    Rect r;
    serverRect(d, w, r);
    return r;
}

// Parent-relative geometry, which is what identifies the frame's sub-windows:
// Border::configure() places the tab at (0,0) and the button at
// (TAB_TOP_HEIGHT + 2, TAB_TOP_HEIGHT + 2) = (4,4), square.
bool localRect(Display* d, Window w, Rect& out)
{
    Window rootRet = None;
    int x = 0, y = 0;
    unsigned int width = 0, height = 0, bw = 0, depth = 0;
    if (!XGetGeometry(d, w, &rootRet, &x, &y, &width, &height, &bw, &depth)) return false;
    out.x = x;
    out.y = y;
    out.w = static_cast<int>(width);
    out.h = static_cast<int>(height);
    return true;
}

Window parentOf(Display* d, Window w)
{
    Window wroot = None, parent = None, *children = nullptr;
    unsigned int n = 0;
    if (!XQueryTree(d, w, &wroot, &parent, &children, &n)) return None;
    if (children) XFree(children);
    return parent;
}

std::vector<Window> childrenOf(Display* d, Window w)
{
    Window wroot = None, parent = None, *children = nullptr;
    unsigned int n = 0;
    std::vector<Window> out;
    if (!XQueryTree(d, w, &wroot, &parent, &children, &n)) return out;
    if (children) {
        out.assign(children, children + n);
        XFree(children);
    }
    return out;
}

bool isViewable(Display* d, Window w)
{
    XWindowAttributes attr;
    if (!XGetWindowAttributes(d, w, &attr)) return false;
    return attr.map_state == IsViewable;
}

// _NET_CLIENT_LIST, in the order the WM published it.
std::vector<Window> clientList(Display* d)
{
    static Atom prop = None;
    if (prop == None) prop = XInternAtom(d, "_NET_CLIENT_LIST", False);

    std::vector<Window> out;
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nItems = 0, bytesAfter = 0;
    unsigned char* raw = nullptr;

    if (XGetWindowProperty(d, DefaultRootWindow(d), prop, 0, 1024, False, XA_WINDOW,
                           &actualType, &actualFormat, &nItems, &bytesAfter,
                           &raw) != Success) {
        return out;
    }
    if (raw && actualType == XA_WINDOW && actualFormat == 32) {
        Window* vals = reinterpret_cast<Window*>(raw);
        out.assign(vals, vals + nItems);
    }
    if (raw) XFree(raw);
    return out;
}

bool listed(Display* d, Window w)
{
    const std::vector<Window> l = clientList(d);
    return std::find(l.begin(), l.end(), w) != l.end();
}

Window activeWindow(Display* d)
{
    static Atom atom = None;
    if (atom == None) atom = XInternAtom(d, "_NET_ACTIVE_WINDOW", False);
    Window w = None;
    if (!readWindowProp(d, DefaultRootWindow(d), atom, w)) return None;
    return w;
}

// WM_STATE's state field (Withdrawn 0, Normal 1, Iconic 3).
bool icccmState(Display* d, Window w, long& out)
{
    static Atom prop = None;
    if (prop == None) prop = XInternAtom(d, "WM_STATE", False);

    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nItems = 0, bytesAfter = 0;
    unsigned char* raw = nullptr;

    if (XGetWindowProperty(d, w, prop, 0, 2, False, AnyPropertyType, &actualType,
                           &actualFormat, &nItems, &bytesAfter, &raw) != Success) {
        return false;
    }
    bool ok = false;
    if (raw && actualFormat == 32 && nItems >= 1) {
        out = reinterpret_cast<long*>(raw)[0];
        ok = true;
    }
    if (raw) XFree(raw);
    return ok;
}

// ---------------------------------------------------------------------------
// Protocol errors (copied from tests/test_wm_lifecycle.cpp, plan 08-11)
// ---------------------------------------------------------------------------

// Every X protocol error the WM reports goes through
// WindowManager::errorHandler(), which prints one line shaped
// `wm2: <request> (0xID): <message>`. Nothing else the WM writes carries
// " (0x", so that substring is a precise detector.
std::vector<std::string> xProtocolErrors(const std::string& stderrText)
{
    std::vector<std::string> out;
    std::string line;
    for (char ch : stderrText) {
        if (ch == '\n') {
            if (line.find(" (0x") != std::string::npos) out.push_back(line);
            line.clear();
        } else {
            line.push_back(ch);
        }
    }
    if (line.find(" (0x") != std::string::npos) out.push_back(line);
    return out;
}

// The subset of the above that is NOT a BadWindow (deferred item 13: destroying
// a client necessarily produces exactly one, and WmFixture destroys its own
// readiness probe during startup).
std::vector<std::string> xProtocolErrorsExceptBadWindow(const std::string& stderrText)
{
    std::vector<std::string> out;
    for (const auto& line : xProtocolErrors(stderrText)) {
        if (line.find("BadWindow") == std::string::npos) out.push_back(line);
    }
    return out;
}

std::string joined(const std::vector<std::string>& lines)
{
    std::string out;
    for (const auto& l : lines) { out += l; out += "\n"; }
    return out;
}

// ---------------------------------------------------------------------------
// Client creation
// ---------------------------------------------------------------------------

Window createClient(Display* d, int x, int y, int w, int h, const char* name = nullptr)
{
    Window root = DefaultRootWindow(d);
    Window win = XCreateSimpleWindow(d, root, x, y,
                                     static_cast<unsigned>(w), static_cast<unsigned>(h), 0,
                                     BlackPixel(d, DefaultScreen(d)),
                                     WhitePixel(d, DefaultScreen(d)));
    if (name) XStoreName(d, win, name);
    XSync(d, False);
    return win;
}

// Announce WM_DELETE_WINDOW support, so Client::kill() sends a message this
// connection receives instead of calling XKillClient -- which would tear down
// the TEST's own connection and take the assertions with it.
void announceDeleteProtocol(Display* d, Window win)
{
    Atom del = XInternAtom(d, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(d, win, &del, 1);
    XSync(d, False);
}

// Wait until the WM has both reparented `win` into a frame AND published it in
// _NET_CLIENT_LIST.
Window awaitFrameFor(Display* d, Window win, int timeoutMs = 8000)
{
    Window root = DefaultRootWindow(d);

    Window frame = None;
    const bool framed = WmFixture::pollUntil([&] {
        pumpWm(d);
        Window parent = parentOf(d, win);
        if (parent == None || parent == root) return false;
        frame = parent;
        return true;
    }, timeoutMs);
    if (!framed) return None;

    WmFixture::pollUntil([&] {
        pumpWm(d);
        return listed(d, win);
    }, timeoutMs);

    return frame;
}

Window mapClientAndAwaitFrame(Display* d, int x, int y, int w, int h, Window& clientOut,
                              const char* name = nullptr)
{
    Window win = createClient(d, x, y, w, h, name);
    clientOut = win;
    XMapWindow(d, win);
    XSync(d, False);
    return awaitFrameFor(d, win);
}

void parkPointer(Display* d)
{
    XWarpPointer(d, None, DefaultRootWindow(d), 0, 0, 0, 0, kParkX, kParkY);
    XSync(d, False);
}

// The frame's tab and button, identified by the parent-relative geometry
// Border::configure() gives them: the tab is the child at (0,0), the button is
// the square child at (4,4). Identified by GEOMETRY rather than by stacking
// order, because XQueryTree's order is a stacking fact and the WM raises and
// lowers these windows as it decorates.
Window findFrameChild(Display* d, Window frame, Window client, bool wantButton)
{
    // Separated by SIZE, not by origin. The button used to be identifiable by
    // its (4,4) offset, but it now covers the whole top square of the tab and so
    // shares the tab's (0,0) origin -- an origin test finds whichever of the two
    // the server happens to list first. The button is the small square; the tab
    // is the long strip running the width of the frame.
    Window button = None, tab = None;

    for (Window child : childrenOf(d, frame)) {
        if (child == client) continue;
        Rect r;
        if (!localRect(d, child, r)) continue;
        if (r.w <= 0 || r.h <= 0) continue;

        if (r.w == r.h && r.w <= 64) button = child;
        else                         tab = child;
    }

    return wantButton ? button : tab;
}

// ---------------------------------------------------------------------------
// Config plumbing
// ---------------------------------------------------------------------------

// Write an isolated XDG config tree and return the directory to hand the child
// as XDG_CONFIG_HOME. Lives under the CMake binary directory rather than a bare
// /tmp name (threat T-8-TMP), and is unique per process and per case.
std::string makeConfigTree(const std::string& contents)
{
    static int counter = 0;
    const std::string base = std::string(WM2_TEST_WORKDIR) + "/runtime-cfg-" +
                             std::to_string(::getpid()) + "-" +
                             std::to_string(++counter);
    ::mkdir(base.c_str(), 0700);
    const std::string dir = base + "/wm2-born-again";
    ::mkdir(dir.c_str(), 0700);

    std::ofstream out(dir + "/config");
    out << contents;
    out.close();
    return base;
}

// A fixture whose WM reads NO config file at all -- both XDG variables point at
// directories that do not exist, which applyFile() skips silently. Without the
// XDG_CONFIG_DIRS override, xdgConfigDirs() falls back to /etc/xdg, and a host
// that happens to carry /etc/xdg/wm2-born-again/config would layer its settings
// under every case in this file and quietly change what they prove.
WmFixtureOptions cleanFixture(std::vector<std::string> wmArgs = {})
{
    WmFixtureOptions o;
    const std::string home = makeConfigTree("");
    o.childEnv["XDG_CONFIG_HOME"] = home + "/no-user-config";
    o.childEnv["XDG_CONFIG_DIRS"] = home + "/no-system-config";
    o.wmArgs = std::move(wmArgs);
    return o;
}

// As above, but with a real user config file carrying `configContents`.
WmFixtureOptions configuredFixture(const std::string& configContents,
                                   std::vector<std::string> wmArgs = {})
{
    WmFixtureOptions o;
    const std::string home = makeConfigTree(configContents);
    o.childEnv["XDG_CONFIG_HOME"] = home;
    o.childEnv["XDG_CONFIG_DIRS"] = home + "/no-system-config";
    o.wmArgs = std::move(wmArgs);
    return o;
}

// ---------------------------------------------------------------------------
// Colour observation
//
// Colours are read back from the SERVER with XGetImage over the ROOT window,
// covering the rectangle the window of interest occupies. Reading root rather
// than the window itself is deliberate: the frame and the tab are SHAPED, and
// XGetImage's result outside a window's bounding shape is undefined. Root is
// never shaped, so what comes back is exactly what is on the screen.
// ---------------------------------------------------------------------------

// An alias for the pure header's type rather than a second spelling of it, so
// classify() takes what captureRoot() returns with no conversion and the two
// cannot drift apart (08.5-13 Task 1).
using Histogram = PixelHistogram;

Histogram captureRoot(Display* d, const Rect& r)
{
    Histogram h;
    if (r.w <= 0 || r.h <= 0) return h;

    const int x = std::max(0, r.x);
    const int y = std::max(0, r.y);
    const int w = std::min(r.w, kScreenW - x);
    const int hh = std::min(r.h, kScreenH - y);
    if (w <= 0 || hh <= 0) return h;

    XImage* img = XGetImage(d, DefaultRootWindow(d), x, y,
                            static_cast<unsigned>(w), static_cast<unsigned>(hh),
                            AllPlanes, ZPixmap);
    if (!img) return h;

    for (int iy = 0; iy < hh; ++iy) {
        for (int ix = 0; ix < w; ++ix) {
            ++h[XGetPixel(img, ix, iy)];
        }
    }
    XDestroyImage(img);
    return h;
}

// The SECOND, independent reading of the same instant: the window's own pixels,
// read straight out of it rather than out of the framebuffer underneath it.
//
// captureRoot() above reads DefaultRootWindow() at SCREEN coordinates, so
// compositing and stacking sit between the menu and the sample -- it observes
// where the menu is EXPECTED to be, not the menu. The menu popup is created
// InputOutput with override_redirect (src/Manager.cpp:646-657) and, unlike a
// client frame or a tab, is NOT shaped, so XGetImage on it is well defined and
// returns exactly the window's own contents.
//
// Two independent reads of one instant are what separate a compositing or
// readback race from a genuine failure to paint. See the dual-capture comment
// in the menu-background case for how their agreement is read.
Histogram captureWindow(Display* d, Window w, const Rect& r)
{
    Histogram h;
    if (r.w <= 0 || r.h <= 0) return h;

    XImage* img = XGetImage(d, w, 0, 0, static_cast<unsigned>(r.w),
                            static_cast<unsigned>(r.h), AllPlanes, ZPixmap);
    if (!img) return h;

    for (int iy = 0; iy < r.h; ++iy) {
        for (int ix = 0; ix < r.w; ++ix) {
            ++h[XGetPixel(img, ix, iy)];
        }
    }
    XDestroyImage(img);
    return h;
}

unsigned long dominantPixel(const Histogram& h)
{
    unsigned long best = 0;
    long bestCount = -1;
    for (const auto& kv : h) {
        if (kv.second > bestCount) { bestCount = kv.second; best = kv.first; }
    }
    return best;
}

long countOf(const Histogram& h, unsigned long pixel)
{
    auto it = h.find(pixel);
    return it == h.end() ? 0 : it->second;
}

// The pixel value the server resolves a colour NAME to on this display. Used to
// turn "the tab is red" into an absolute assertion rather than a comparison
// against another run -- a run comparison alone cannot distinguish "the setting
// changed the colour" from "the setting changed something else about the frame".
unsigned long namedPixel(Display* d, const char* name)
{
    XColor screenColour, exact;
    if (!XAllocNamedColor(d, DefaultColormap(d, DefaultScreen(d)), name,
                          &screenColour, &exact)) {
        return ~0UL;
    }
    return screenColour.pixel;
}

std::string hex(unsigned long v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%06lx", v & 0xffffffUL);
    return buf;
}

std::string describeTop(Display* d, const Histogram& h, size_t n = 4)
{
    std::vector<std::pair<long, unsigned long>> byCount;
    for (const auto& kv : h) byCount.push_back({kv.second, kv.first});
    std::sort(byCount.rbegin(), byCount.rend());
    std::string out = "{";
    for (size_t i = 0; i < byCount.size() && i < n; ++i) {
        out += hex(byCount[i].second) + "x" + std::to_string(byCount[i].first);
        if (i + 1 < byCount.size() && i + 1 < n) out += ", ";
    }
    out += "}";
    (void)d;
    return out;
}

// ---------------------------------------------------------------------------
// The root menu, opened with a real button press
// ---------------------------------------------------------------------------

// WindowManager::menu() runs a NESTED event loop with its own pointer grab, so
// while the menu is up the WM is not in loop() and pumpWm() cannot wake it.
// Everything the menu cases observe is therefore read directly from the server
// while the press is still held.
//
// The menu window is created by initialiseScreen() as a 1x1 unmapped child of
// root and only moved/resized when the menu opens, so with no client mapped it
// is the ONLY viewable child of root larger than 1x1. That is why the menu cases
// deliberately map no client of their own.
// The OUTER menu is identified by the press point it is anchored on, not by
// being the first viewable child of root.
//
// WindowManager::menu() places the menu at (pressX - width/2, pressY - 2), so
// the press point lies inside it. The submenu popup -- a second window of the
// same kind, mapped when a MotionNotify lands on a category row -- is placed to
// the SIDE and does not contain the press point. MEASURED: taking the first
// candidate picked up the submenu on a host whose application cache made the
// menu tall enough for the WM to warp the pointer while opening it, and the
// case then compared the wrong window's pixels. Whether a submenu opens at all
// depends on how many applications the host has installed, which is exactly the
// kind of hidden dependency a test should not carry.
//
// THE PRESS POINT IS THE ONLY TEST. 08.5-13 removed a silent fallback here --
// "the first viewable child of root larger than 1x1", returned whenever nothing
// contained the press point -- and that removal is this round's attributed fix,
// established by single-variable bisect on 2026-09-01 rather than inferred.
//
// What the fallback did: when the menu was not yet mapped it returned SOME
// OTHER WINDOW, and because that is non-None the enclosing pollUntil in
// openRootMenu() was satisfied on its FIRST iteration and never retried. The
// twenty-second stage budget was therefore never spent -- the wait looked
// generous and was never used. The old `size() >= 2` completion rule then
// passed trivially on the wrong window, and the case sampled a client frame.
//
// MEASURED under ASan: the shipped run and a `--menu-background=#00cc00` run
// returned the byte-identical histogram
//     {0xdcdee0 x3000, 0x000000 x1428, 0xc8cacc x512, 0x222222 x17}
// -- the green run contained NO GREEN AT ALL. 0xDCDEE0 is frameBackground /
// buttonBackground and 0xC8CACC is tabBackground (include/Config.h), so the
// dominant pixels were a client FRAME and its sideways TAB. Two different
// configurations cannot produce identical pixels unless neither was sampled.
//
// Returning None instead is what makes the poll a poll: it keeps looking until
// the menu is genuinely mapped, and reports honestly if it never is. Note that
// nothing here was made to wait LONGER -- the budget was never the constraint,
// because it was never spent.
Window findOpenMenu(Display* d, int pressX, int pressY)
{
    for (Window child : childrenOf(d, DefaultRootWindow(d))) {
        Rect r;
        if (!serverRect(d, child, r)) continue;
        if (r.w <= 1 || r.h <= 1) continue;
        if (!isViewable(d, child)) continue;
        if (pressX >= r.x && pressX < r.x + r.w &&
            pressY >= r.y && pressY < r.y + r.h) {
            return child;
        }
    }
    return None;
}

// Where every menu case presses. Chosen so WindowManager::menu() does NOT need
// to clamp the menu to a screen edge: a clamp warps the pointer, the warp is a
// MotionNotify, and a MotionNotify over a category row opens a submenu nobody
// asked for. Leaves room for a menu up to 600 wide and 760 tall.
constexpr int kMenuPressX = 300;
constexpr int kMenuPressY = 5;

// Open the root menu with a real press and return only once the WM has
// actually DRAWN it -- not merely mapped it.
//
// WindowManager::menu() ignores a ButtonRelease entirely until its `drawn` flag
// is set, and that flag is set by the Expose handler. A release that beats the
// Expose selects nothing: the menu just closes and no entry runs. MEASURED: one
// flake in six runs of the 15-case gate, in which the New entry simply never
// fired and the case reported a missing window with no other symptom.
//
// "Drawn" was observed from outside as "the menu rectangle is no longer one
// flat colour" -- a count of distinct pixel values. That decision now lives in
// tests/support/PixelVerdict.h, where a display-free test can reach it
// (08.5-13 Task 1). The extraction is faithful: classify() still answers
// exactly what `size() >= 2` answered, defect and all. See the header for why
// that question is the wrong one.
//
// EACH STAGE NOW REPORTS ITSELF. The three stages -- mapped, geometry realised,
// painted -- used to fail behind one shared `false`, so a red run could not say
// which of them expired, and a flake in this helper could not be attributed to
// anything narrower than "the menu did not work". Each expiry now names itself
// and, for the paint stage, the verdict and pixels it last saw.

// The shipped menu background (08.5-02). Callers that configure no colour of
// their own are asking about this one.
constexpr const char* kShippedMenuBackground = "#C8CACC";

bool openRootMenu(Display* d, XTestDriver& driver, int x, int y,
                  Window& menuOut, Rect& rectOut, unsigned long expectedBg)
{
    driver.moveTo(x, y);
    driver.press(Button1);

    // 20 s rather than 8 s at each stage for the same reason kSpawnObserveMs is
    // generous: under the sanitizer, with the whole suite running, the WM takes
    // noticeably longer to reach its Expose handler.
    constexpr int kMenuStageMs = 20000;

    menuOut = None;
    if (!WmFixture::pollUntil([&] {
            menuOut = findOpenMenu(d, x, y);
            return menuOut != None;
        }, kMenuStageMs)) {
        UNSCOPED_INFO("openRootMenu: stage 1 (mapped) expired at "
                      << kMenuStageMs << " ms -- no child of root contains the "
                      "press point (" << x << "," << y << ")");
        return false;
    }

    if (!WmFixture::pollUntil([&] {
            return serverRect(d, menuOut, rectOut) && rectOut.w > 1 && rectOut.h > 1;
        }, kMenuStageMs)) {
        UNSCOPED_INFO("openRootMenu: stage 2 (geometry) expired at "
                      << kMenuStageMs << " ms -- menu " << menuOut
                      << " rect " << describe(rectOut));
        return false;
    }

    PaintVerdict last = PaintVerdict::NoPixels;
    Histogram lastSeen;
    const bool painted = WmFixture::pollUntil([&] {
        lastSeen = captureRoot(d, rectOut);
        last = classify(lastSeen, expectedBg);
        return last == PaintVerdict::Painted;
    }, kMenuStageMs);

    if (!painted) {
        UNSCOPED_INFO("openRootMenu: stage 3 (painted) expired at "
                      << kMenuStageMs << " ms -- last verdict "
                      << describeVerdict(last) << ", expected "
                      << hex(expectedBg) << ", pixels "
                      << describeTop(nullptr, lastSeen));
    }
    return painted;
}

// Same three stages, asking about the shipped colour. Every call site that
// predates 08.5-13 goes through here, so none of them changed.
bool openRootMenu(Display* d, XTestDriver& driver, int x, int y,
                  Window& menuOut, Rect& rectOut)
{
    return openRootMenu(d, driver, x, y, menuOut, rectOut,
                        namedPixel(d, kShippedMenuBackground));
}

// Release over the first menu row ("New"). The WM computes
// sel = (y - 11) / entryHeight from menu-relative coordinates, so a few pixels
// into the first row selects entry 0 whatever the font's entry height is.
void selectFirstMenuEntry(Display* d, XTestDriver& driver, const Rect& menuRect)
{
    driver.moveTo(menuRect.x + menuRect.w / 2, menuRect.y + 14);
    driver.release(Button1);
    XSync(d, False);
}

// Dismiss the menu WITHOUT selecting anything: the grab uses owner_events=False,
// so the release coordinates are menu-relative and a release well outside the
// menu's width sets sel = -1.
void dismissMenu(Display* d, XTestDriver& driver)
{
    driver.moveTo(kScreenW - 5, kScreenH - 5);
    driver.release(Button1);
    XSync(d, False);
    settleWm(d);
}

// ---------------------------------------------------------------------------
// Spawned programs
// ---------------------------------------------------------------------------

// The res_class of a managed window, read from WM_CLASS. This is how a spawned
// program is identified: the WM tells nobody what it launched, but the program
// itself announces its class on the window it maps.
std::string classOf(Display* d, Window w)
{
    XClassHint hint;
    hint.res_name = nullptr;
    hint.res_class = nullptr;
    if (!XGetClassHint(d, w, &hint)) return "";
    std::string out = hint.res_class ? hint.res_class : "";
    if (hint.res_name) XFree(hint.res_name);
    if (hint.res_class) XFree(hint.res_class);
    return out;
}

std::string instanceOf(Display* d, Window w)
{
    XClassHint hint;
    hint.res_name = nullptr;
    hint.res_class = nullptr;
    if (!XGetClassHint(d, w, &hint)) return "";
    std::string out = hint.res_name ? hint.res_name : "";
    if (hint.res_name) XFree(hint.res_name);
    if (hint.res_class) XFree(hint.res_class);
    return out;
}

// Wait for a managed client whose WM_CLASS class matches `cls`, ignoring
// `known`. Returns None on timeout, which is what the negative cases assert.
Window awaitClientWithClass(Display* d, const char* cls,
                            const std::vector<Window>& known, int timeoutMs)
{
    Window found = None;
    WmFixture::pollUntil([&] {
        pumpWm(d);
        for (Window w : clientList(d)) {
            if (std::find(known.begin(), known.end(), w) != known.end()) continue;
            if (classOf(d, w) == cls) { found = w; return true; }
        }
        return false;
    }, timeoutMs);
    return found;
}

// A unique path under the CMake binary directory (threat T-8-TMP, never a bare
// /tmp name) used as the "a shell ran this" witness in the exec-using-shell
// cases.
std::string sentinelPath(const char* tag)
{
    static int counter = 0;
    return std::string(WM2_TEST_WORKDIR) + "/shell-sentinel-" + tag + "-" +
           std::to_string(::getpid()) + "-" + std::to_string(++counter);
}

// ---------------------------------------------------------------------------
// Running the binary as a subprocess that is EXPECTED TO DIE
//
// WindowManager::fatal() prints and calls std::exit (src/Manager.cpp:373), and
// the initialisation-time arm of errorHandler() does the same. An in-process
// test cannot observe either: there is nothing left to assert on. So every
// [wm_errors] case forks, execs, and asserts on the captured exit status and
// stderr -- which is also the only way to distinguish "exited with the wrong
// code" from "did not exit at all", the failure a deadline exists to catch.
// ---------------------------------------------------------------------------

struct RunResult {
    bool timedOut = false;
    bool exitedNormally = false;
    int exitCode = -1;
    int signal = 0;
    long elapsedMs = 0;
    std::string output;
};

std::string readWholeFile(const std::string& path)
{
    std::string out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

RunResult runBinary(const std::vector<std::string>& args,
                    const std::map<std::string, std::string>& env,
                    bool unsetDisplay, int timeoutMs)
{
    static int counter = 0;
    const std::string outPath = std::string(WM2_TEST_WORKDIR) + "/errpath-" +
                                std::to_string(::getpid()) + "-" +
                                std::to_string(++counter) + ".out";

    std::vector<std::string> argv{WM2_BINARY_PATH};
    for (const auto& a : args) argv.push_back(a);

    RunResult result;
    const auto started = Clock::now();

    pid_t pid = ::fork();
    if (pid < 0) return result;
    if (pid == 0) {
        int fd = ::open(outPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd >= 0) {
            ::dup2(fd, STDOUT_FILENO);
            ::dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO) ::close(fd);
        }
        ::setsid();

        // Sanitizer settings for a process that is SUPPOSED to exit early.
        // detect_leaks is off on purpose: a fatal path exits without unwinding,
        // so every live allocation is reported as a leak and the exit code
        // becomes the leak sentinel instead of the code under test. Turning it
        // off here removes noise from a path whose memory behaviour is not what
        // these cases are about -- the [wm_stress] group below is where leaks
        // are the assertion.
        ::setenv("ASAN_OPTIONS", "detect_leaks=0:abort_on_error=0:handle_segv=1", 1);
        ::setenv("UBSAN_OPTIONS", "print_stacktrace=1:halt_on_error=0", 1);

        for (const auto& kv : env) ::setenv(kv.first.c_str(), kv.second.c_str(), 1);
        if (unsetDisplay) ::unsetenv("DISPLAY");

        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        ::execv(cargv[0], cargv.data());
        _exit(127);
    }

    ChildProcess child(pid);
    const bool exited = child.waitForExit(timeoutMs);
    result.elapsedMs = static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count());

    if (!exited) {
        result.timedOut = true;
        child.shutdown();          // never leave a hung WM behind
    } else {
        result.exitedNormally = child.exitedNormally();
        result.exitCode = child.exitCode();
        result.signal = child.termSignal();
    }

    result.output = readWholeFile(outPath);
    ::unlink(outPath.c_str());
    return result;
}

bool contains(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}

// No sanitizer finding hid inside a deliberately-fatal run. Not the point of
// these cases, but a free assertion: the terminating paths run allocation and
// X-connection code like any other, and a report there would otherwise be
// swallowed by the very exit the case is asserting on.
bool sanitizerClean(const std::string& output)
{
    return !contains(output, "AddressSanitizer") &&
           !contains(output, "LeakSanitizer") &&
           !contains(output, "runtime error:");
}

// ---------------------------------------------------------------------------
// A display with NO window manager on it
//
// WmFixture always starts one, and it must: everything else in this file
// asserts against a running WM. But the invalid-colour and unloadable-font
// paths both terminate INSIDE initialiseScreen(), and the root-redirect claim
// happens earlier in that same function -- so on a display that already has a
// WM the second process dies of the redirect conflict first and never reaches
// the path under test.
//
// Built from the same DisplayReservation and ChildProcess the fixture uses (they
// are separate classes for exactly this kind of reuse) rather than by adding a
// "no WM please" mode to shared infrastructure six other suites depend on.
// ---------------------------------------------------------------------------

class BareDisplay {
public:
    BareDisplay()
    {
        constexpr int kBase = 200;
        constexpr int kMaxCandidates = 60;

        for (int n = kBase; n < kBase + kMaxCandidates; ++n) {
            if (!m_reservation.tryReserve(n)) continue;
            m_display = m_reservation.displayString();
            if (spawnXvfb() && waitForServer()) return;
            m_xvfb.shutdown();
            m_reservation.release();
            m_display.clear();
        }
        throw std::runtime_error("BareDisplay: could not reserve a free X display");
    }

    BareDisplay(const BareDisplay&) = delete;
    BareDisplay& operator=(const BareDisplay&) = delete;

    ~BareDisplay()
    {
        m_keepAlive.reset();
        m_xvfb.shutdown();
        m_reservation.release();
    }

    const std::string& display() const { return m_display; }

private:
    bool spawnXvfb()
    {
        const std::string logPath = std::string(WM2_TEST_WORKDIR) + "/bare-xvfb" +
                                    std::to_string(m_reservation.number()) + ".log";
        pid_t pid = ::fork();
        if (pid < 0) return false;
        if (pid == 0) {
            int fd = ::open(logPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
            if (fd >= 0) {
                ::dup2(fd, STDOUT_FILENO);
                ::dup2(fd, STDERR_FILENO);
                if (fd > STDERR_FILENO) ::close(fd);
            }
            ::setsid();
            ::execlp("Xvfb", "Xvfb", m_display.c_str(), "-screen", "0", "1024x768x24",
                     "-ac", "+render", "-noreset", "-nolisten", "tcp",
                     static_cast<char*>(nullptr));
            _exit(127);
        }
        m_xvfb = ChildProcess(pid);
        return true;
    }

    bool waitForServer()
    {
        const bool up = WmFixture::pollUntil([this] {
            if (m_xvfb.tryReap()) return false;
            m_keepAlive = x11::DisplayPtr(XOpenDisplay(m_display.c_str()));
            return m_keepAlive != nullptr;
        }, 10000);
        return up && !m_xvfb.reaped();
    }

    DisplayReservation m_reservation;
    std::string m_display;
    ChildProcess m_xvfb;
    x11::DisplayPtr m_keepAlive;
};

// A fontconfig configuration whose ONLY font directory is empty, so no font
// pattern can resolve. Returns the path to hand the child as FONTCONFIG_FILE.
//
// Deliberately NOT described as a config setting: the binary has no
// font-pattern setting of any kind, which is precisely why this path has to be
// provoked through the environment.
std::string makeEmptyFontConfig()
{
    static int counter = 0;
    const std::string base = std::string(WM2_TEST_WORKDIR) + "/nofonts-" +
                             std::to_string(::getpid()) + "-" +
                             std::to_string(++counter);
    ::mkdir(base.c_str(), 0700);
    ::mkdir((base + "/fonts").c_str(), 0700);
    ::mkdir((base + "/cache").c_str(), 0700);

    const std::string path = base + "/fonts.conf";
    std::ofstream out(path);
    out << "<?xml version=\"1.0\"?>\n"
        << "<!DOCTYPE fontconfig SYSTEM \"fonts.dtd\">\n"
        << "<fontconfig>\n"
        << "  <dir>" << base << "/fonts</dir>\n"
        << "  <cachedir>" << base << "/cache</cachedir>\n"
        << "</fontconfig>\n";
    out.close();
    return path;
}

// The four terminating conditions, named once so the deadline case below runs
// exactly the same launches the individual cases do rather than an approximation
// of them.
constexpr int kFatalTimeoutMs = 20000;   // generous: a slow or loaded host
constexpr int kFatalDeadlineMs = 6000;   // tight: what a healthy host really takes

}  // namespace

// Defined below, beside the [wm_menulabel] cases that first needed it; declared
// here so the exec-using-shell case above them can take the same verified open.
bool openRootMenuVerified(Display* d, XTestDriver& driver, int x, int y,
                          Window& menuOut, Rect& rectOut, std::string& whyOut,
                          unsigned long expectedBg);


// ===========================================================================
// [wm_harness] -- the fixture's own stale-report cleanup (deferred item 14)
// ===========================================================================

TEST_CASE("A stale sanitizer report on a reused display prefix is cleared, and "
          "only that prefix is cleared", "[wm_harness]")
{
    // The hazard, stated as a test rather than as a comment: the fixture
    // attributes ANY file matching its display prefix to its own child. The
    // display pool is small and reused, so a report written by an earlier run
    // on the same number reads as a fresh sanitizer finding in an innocent
    // case -- which cost 08-12 roughly twenty minutes chasing a phantom
    // regression in updateWorkarea().
    std::string prefix, display;

    {
        WmFixture fixture;
        prefix  = fixture.asanLogPrefix();
        display = fixture.display();
        REQUIRE_FALSE(prefix.empty());

        const std::string ours    = prefix + ".stale-08-13";
        const std::string foreign = std::string(WM2_TEST_WORKDIR) +
                                    "/asan-not-this-display." + std::to_string(::getpid());

        { std::ofstream f(ours);    f << "stale\n"; }
        { std::ofstream f(foreign); f << "stale\n"; }

        // Half one: the attribution really is prefix-based, so a stale file
        // really does become someone else's failure. Without this the cleanup
        // below would be a fix for a problem the test never demonstrated.
        INFO("prefix: " << prefix);
        CHECK_FALSE(fixture.asanReports().empty());

        // Half two: the cleanup removes exactly the fixture's own prefix.
        WmFixture::removeReportsWithPrefix(prefix);

        CHECK(fixture.asanReports().empty());
        CHECK_FALSE(pathExists(ours));

        // A neighbouring fixture's reports are NOT collateral. A cleanup that
        // cleared the whole directory would pass every assertion above and would
        // silently delete a concurrent ctest worker's genuine finding.
        CHECK(pathExists(foreign));
        ::unlink(foreign.c_str());
    }

    // Half three: the WIRING, which is the half that actually protects anyone.
    // The two halves above exercise the helper; deleting the CALL to it in
    // WmFixture::start() would leave both of them green, and the call is the
    // whole fix.
    //
    // Reusing the same display number is deterministic rather than lucky: the
    // reservation scans upward from a fixed base and takes the lowest FREE
    // display, the fixture above has released its own reservation, and ctest
    // runs these suites serially (scripts/gates/build-all.sh passes no -j). The
    // wait below is for Xvfb's /tmp lock file, which the reservation also
    // consults and which outlives the process by a moment.
    const std::string number = display.substr(1);
    const std::string xLock  = "/tmp/.X" + number + "-lock";
    WmFixture::pollUntil([&] { return !pathExists(xLock); }, 8000);

    const std::string planted = prefix + ".stale-wiring";
    { std::ofstream f(planted); f << "stale\n"; }
    REQUIRE(pathExists(planted));

    WmFixture second;
    INFO("first display " << display << " prefix " << prefix);
    INFO("second display " << second.display() << " prefix " << second.asanLogPrefix());

    // If this ever fails, the display was not reused and the case below would be
    // vacuous -- so it fails loudly here rather than passing for the wrong
    // reason. Clean up the plant either way.
    const bool reused = (second.asanLogPrefix() == prefix);
    if (!reused) ::unlink(planted.c_str());
    REQUIRE(reused);

    CHECK(second.asanReports().empty());
    CHECK_FALSE(pathExists(planted));
}


// ===========================================================================
// [wm_config_runtime] -- config settings reaching runtime (coverage item 5)
// ===========================================================================

TEST_CASE("Frame thickness changes the measured frame geometry in both directions",
          "[wm_config_runtime]")
{
    // Border::reparent() places the client at (xIndent(), yIndent()) inside the
    // frame, and yIndent() is FRAME_WIDTH + 1 for a non-transient window. So the
    // vertical inset is an EXACT function of the setting and can be asserted
    // absolutely -- no run comparison needed and no dependence on the font.
    //
    // The horizontal inset is xIndent() = m_tabWidth + FRAME_WIDTH + 1, and
    // m_tabWidth depends on whatever font fontconfig resolves on the host. It is
    // therefore asserted as a DIFFERENCE between runs, where the font term
    // cancels exactly.
    struct Measured { int vertical = 0; int horizontal = 0; };

    auto measure = [](int thickness, bool useDefault) {
        std::vector<std::string> args;
        if (!useDefault) args.push_back("--frame-thickness=" + std::to_string(thickness));

        WmFixture fixture(cleanFixture(args));
        x11::DisplayPtr dp = fixture.openDisplay();
        REQUIRE(dp != nullptr);
        Display* d = dp.get();
        parkPointer(d);

        Window win = None;
        Window frame = mapClientAndAwaitFrame(d, 200, 150, 300, 220, win, "thickness");
        REQUIRE(frame != None);
        settleWm(d);

        const Rect frameRect  = rectOf(d, frame);
        const Rect clientRect = rectOf(d, win);

        INFO("frame " << describe(frameRect) << " client " << describe(clientRect));
        INFO("wm stderr:\n" << fixture.wmStderr());
        CHECK(joined(xProtocolErrorsExceptBadWindow(fixture.wmStderr())).empty());

        Measured m;
        m.vertical   = clientRect.y - frameRect.y;
        m.horizontal = clientRect.x - frameRect.x;
        return m;
    };

    const Measured thin    = measure(3, false);
    const Measured shipped = measure(kDefaultFrameThickness, true);
    const Measured thick   = measure(20, false);

    INFO("thin=" << thin.vertical << " shipped=" << shipped.vertical
         << " thick=" << thick.vertical);

    // Absolute: yIndent() == FRAME_WIDTH + 1.
    CHECK(thin.vertical    == 3 + 1);
    CHECK(shipped.vertical == kDefaultFrameThickness + 1);
    CHECK(thick.vertical   == 20 + 1);

    // And both directions from the shipped default really are different, which
    // is the claim "the setting reaches the runtime" actually makes.
    CHECK(thin.vertical  < shipped.vertical);
    CHECK(thick.vertical > shipped.vertical);

    // Relative, with the font-dependent tab width cancelling out.
    CHECK(thin.horizontal  - shipped.horizontal == 3  - kDefaultFrameThickness);
    CHECK(thick.horizontal - shipped.horizontal == 20 - kDefaultFrameThickness);
}

TEST_CASE("tab-font changes the thickness of the tab a real frame is built with",
          "[wm_config_runtime]")
{
    // Two frame sub-window readings, both parent-relative geometry taken from
    // the server, and both an exact function of Border::m_tabWidth -- which is
    // derived from whatever face Border::loadTabFont() actually resolved:
    //
    //   button square   buttonDrawSize() == m_tabWidth - TAB_TOP_HEIGHT * 2 - 4
    //   client inset    xIndent()        == m_tabWidth + FRAME_WIDTH + 1
    //
    // Frame thickness is left at the shipped default in both runs, so FRAME_WIDTH
    // cancels out of the difference and the two readings must move together by
    // exactly the same amount. That internal agreement is what makes this a
    // measurement of the TAB rather than of the frame around it.
    //
    // Nothing here asks Xft or fontconfig anything, on purpose. A case that asked
    // the font library what it would load for a pattern would pass just as
    // happily with the configured value dropped on the floor before it ever
    // reached loadTabFont() -- which is precisely the class of dead-config defect
    // this whole tag group exists to catch.
    struct Measured { int buttonSize = 0; int horizontal = 0; };

    auto measure = [](const std::vector<std::string>& args) {
        WmFixture fixture(cleanFixture(args));
        x11::DisplayPtr dp = fixture.openDisplay();
        REQUIRE(dp != nullptr);
        Display* d = dp.get();
        parkPointer(d);

        Window win = None;
        Window frame = mapClientAndAwaitFrame(d, 200, 150, 300, 220, win, "tabfont");
        REQUIRE(frame != None);
        settleWm(d);

        Window button = findFrameChild(d, frame, win, true);
        REQUIRE(button != None);

        Rect buttonRect{};
        REQUIRE(localRect(d, button, buttonRect));

        const Rect frameRect  = rectOf(d, frame);
        const Rect clientRect = rectOf(d, win);

        // Bound BEFORE the assertions that may fail, so a failure reports the
        // window manager's own account of the run rather than an empty string
        // (the guard-ordering defect recorded in 08.5-06).
        const std::string stderrText = fixture.wmStderr();

        INFO("button " << describe(buttonRect) << " frame " << describe(frameRect)
             << " client " << describe(clientRect));
        INFO("wm stderr:\n" << stderrText);
        CHECK(joined(xProtocolErrorsExceptBadWindow(stderrText)).empty());

        Measured m;
        m.buttonSize = buttonRect.w;
        m.horizontal = clientRect.x - frameRect.x;
        return m;
    };

    // The shipped pattern is size 12 (include/Config.h). Size 32 of the SAME
    // family chain is used rather than a different family: it is resolvable
    // wherever the default is resolvable, so this case cannot fail for the
    // uninteresting reason that a test host lacks some particular font.
    const Measured shipped = measure({});
    const Measured large   =
        measure({"--tab-font=Ubuntu,Noto Sans,DejaVu Sans,Sans:bold:size=32"});

    INFO("shipped button=" << shipped.buttonSize << " inset=" << shipped.horizontal);
    INFO("large   button=" << large.buttonSize   << " inset=" << large.horizontal);

    // The claim the setting makes: a larger face builds a thicker tab.
    CHECK(large.buttonSize > shipped.buttonSize);
    CHECK(large.horizontal > shipped.horizontal);

    // ...and both independent readings moved by the same amount, so what changed
    // is the tab's thickness and nothing else about the frame.
    CHECK(large.buttonSize - shipped.buttonSize ==
          large.horizontal - shipped.horizontal);
}

TEST_CASE("An unresolvable tab-font still leaves the window manager framing windows",
          "[wm_config_runtime]")
{
    // T-9-01. Border::loadTabFont()'s four-rung ladder may not terminate the
    // process (XDIS-04/XDIS-05), and `tab-font` is the first user-supplied string
    // ever to reach its top rung. fontconfig substitutes rather than fails for an
    // unknown family, so the expected outcome is a substituted face rather than a
    // walk down the ladder -- but the property that matters is that the window
    // manager is still alive and still framing, and that is asserted rather than
    // assumed.
    WmFixture fixture(cleanFixture(
        {"--tab-font=NoSuchFontFamilyAnywhere12345:bold:size=12"}));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    parkPointer(d);

    Window win = None;
    Window frame = mapClientAndAwaitFrame(d, 200, 150, 300, 220, win, "nonsense-font");

    const std::string stderrText = fixture.wmStderr();
    INFO("wm stderr:\n" << stderrText);

    CHECK(fixture.wmAlive());
    REQUIRE(frame != None);
    CHECK(joined(xProtocolErrorsExceptBadWindow(stderrText)).empty());
}

TEST_CASE("Tab foreground and background colours reach the rendered tab",
          "[wm_config_runtime]")
{
    // Captured over the frame's rectangle on ROOT. The tab is shaped, so
    // XGetImage on the tab window itself would return undefined content outside
    // its bounding region; root is never shaped.
    auto capture = [](const std::vector<std::string>& args, Histogram& out,
                      unsigned long& fgWanted, unsigned long& bgWanted,
                      const char* fgName, const char* bgName) {
        WmFixture fixture(cleanFixture(args));
        x11::DisplayPtr dp = fixture.openDisplay();
        REQUIRE(dp != nullptr);
        Display* d = dp.get();
        parkPointer(d);

        fgWanted = namedPixel(d, fgName);
        bgWanted = namedPixel(d, bgName);

        Window win = None;
        // A label is required or Border::drawLabel() returns before it draws
        // anything, and the foreground colour would never reach a pixel.
        Window frame = mapClientAndAwaitFrame(d, 300, 200, 260, 200, win,
                                              "IIIIIIIIIIIIIIII");
        REQUIRE(frame != None);
        settleWm(d);

        out = captureRoot(d, rectOf(d, frame));

        INFO("wm stderr:\n" << fixture.wmStderr());
        CHECK(joined(xProtocolErrorsExceptBadWindow(fixture.wmStderr())).empty());
    };

    Histogram shipped, configured;
    unsigned long shippedFg = 0, shippedBg = 0, configuredFg = 0, configuredBg = 0;

    // The shipped palette, silver since plan 08.5-02. Named as literal hex
    // because that is what include/Config.h now carries; a colour NAME here
    // would keep passing if the default drifted to a different silver.
    capture({}, shipped, shippedFg, shippedBg, "#000000", "#C8CACC");
    capture({"--tab-background=#ff0000", "--tab-foreground=#0000ff"},
            configured, configuredFg, configuredBg, "#0000ff", "#ff0000");

    REQUIRE(configuredFg != ~0UL);
    REQUIRE(configuredBg != ~0UL);

    INFO("shipped tab pixels:    " << describeTop(nullptr, shipped));
    INFO("configured tab pixels: " << describeTop(nullptr, configured));
    INFO("wanted bg " << hex(configuredBg) << " fg " << hex(configuredFg));

    // The configured BACKGROUND is present, in quantity, and was not there
    // before. "In quantity" is what separates a painted tab from a stray pixel.
    CHECK(countOf(configured, configuredBg) > 200);
    CHECK(countOf(shipped, configuredBg) == 0);

    // The configured FOREGROUND reached the label. Xft antialiases, so the count
    // is small -- but the glyph cores are the exact colour, and the shipped run
    // (black on silver) contains none of it at all.
    CHECK(countOf(configured, configuredFg) > 0);
    CHECK(countOf(shipped, configuredFg) == 0);

    // The shipped defaults are still what the shipped run renders, so this case
    // fails if a future change swaps the defaults out from under it rather than
    // only if the flags stop working.
    CHECK(countOf(shipped, shippedBg) > 200);
    CHECK(shippedFg != configuredFg);
}

TEST_CASE("Menu background colour reaches the menu opened by a real root click",
          "[wm_config_runtime]")
{
    // No client is mapped in this case on purpose, so that nothing else of any
    // size is on the screen while the menu is up.
    //
    // THE DUAL CAPTURE, AND HOW TO READ IT (08.5-13).
    //
    // Two independent readings are taken at the same instant:
    //
    //   root   captureRoot()   -- XGetImage on DefaultRootWindow() at SCREEN
    //                             coordinates. Compositing and stacking sit
    //                             between the menu and this sample; it reads
    //                             where the menu is EXPECTED to be.
    //   direct captureWindow() -- XGetImage on the menu window itself. Nothing
    //                             sits between it and the pixels.
    //
    // Their agreement partitions every failure into exactly one bucket, and
    // there is no fourth:
    //
    //   they DISAGREE                 -> the READBACK PATH is at fault. The
    //                                    defect is in the test instrument, and
    //                                    `direct` is the correct instrument.
    //   they AGREE, wrong colour      -> PRODUCTION did not paint. Check the
    //                                    window manager's stderr for whether it
    //                                    ever reached its Expose arm; the outer
    //                                    menu is drawn ONLY from there
    //                                    (src/Buttons.cpp:312/396/543) and
    //                                    nothing retries.
    //   they AGREE, right colour      -> the sample merely ran EARLY, and the
    //                                    completion predicate was the defect.
    //
    // The assertions are made on `direct`. `root` is reported alongside through
    // INFO so a disagreement is visible in the failure output rather than
    // having to be reproduced.
    auto capture = [](const std::vector<std::string>& args, Histogram& out,
                      Histogram& rootOut,
                      unsigned long& wanted, const char* colourName) {
        WmFixture fixture(cleanFixture(args));
        x11::DisplayPtr dp = fixture.openDisplay();
        REQUIRE(dp != nullptr);
        Display* d = dp.get();
        parkPointer(d);

        wanted = namedPixel(d, colourName);

        XTestDriver driver(fixture.display());
        driver.moveTo(kParkX, kParkY);

        // openRootMenu() returns only once the menu is mapped AND drawn, which
        // is two separate server round trips. The colour this run configured is
        // handed to the paint stage: the whole subject of this case is whether
        // THAT colour reached the menu, so it is the colour the completion
        // verdict must be asked about.
        Window menu = None;
        Rect menuRect{};
        REQUIRE(openRootMenu(d, driver, kMenuPressX, kMenuPressY, menu, menuRect,
                             wanted));

        // Both readings, back to back, before anything is allowed to change.
        rootOut = captureRoot(d, menuRect);
        out     = captureWindow(d, menu, menuRect);

        // The discriminator record. Printed unconditionally, on every run and
        // whether the case passes or fails, because a partition built only from
        // failures cannot say what the passing runs looked like. 08.5-13
        // Task 4's thirty-run log is assembled from these lines.
        std::cout << "MENUPAINT-DISCRIMINATOR"
                  << " colour=" << colourName
                  << " expected=" << hex(wanted)
                  << " root=" << describeVerdict(classify(rootOut, wanted))
                  << " direct=" << describeVerdict(classify(out, wanted))
                  << " rootDom=" << hex(dominantPixel(rootOut))
                  << " directDom=" << hex(dominantPixel(out))
                  << " agree=" << (dominantPixel(rootOut) == dominantPixel(out)
                                   ? "yes" : "no")
                  << std::endl;

        dismissMenu(d, driver);

        INFO("menu rect " << describe(menuRect));
        INFO("root readback verdict:   "
             << describeVerdict(classify(rootOut, wanted))
             << " " << describeTop(nullptr, rootOut));
        INFO("direct readback verdict: "
             << describeVerdict(classify(out, wanted))
             << " " << describeTop(nullptr, out));
        INFO("wm stderr:\n" << fixture.wmStderr());
        CHECK(joined(xProtocolErrorsExceptBadWindow(fixture.wmStderr())).empty());
    };

    Histogram shipped, configured;
    Histogram shippedRoot, configuredRoot;
    unsigned long shippedWanted = 0, configuredWanted = 0;

    // the shipped silver, 08.5-02
    capture({}, shipped, shippedRoot, shippedWanted, "#C8CACC");
    capture({"--menu-background=#00cc00"}, configured, configuredRoot,
            configuredWanted, "#00cc00");

    REQUIRE(configuredWanted != ~0UL);

    INFO("shipped menu pixels    (direct): " << describeTop(nullptr, shipped));
    INFO("configured menu pixels (direct): " << describeTop(nullptr, configured));
    INFO("shipped menu pixels    (root):   " << describeTop(nullptr, shippedRoot));
    INFO("configured menu pixels (root):   " << describeTop(nullptr, configuredRoot));

    // The positive criterion, on the reading with nothing between it and the
    // pixels: the configured colour DOMINATES the menu, rather than merely
    // appearing somewhere in it.
    CHECK(classify(configured, configuredWanted) == PaintVerdict::Painted);
    CHECK(classify(shipped, shippedWanted) == PaintVerdict::Painted);

    // Every CHECK this case made before 08.5-13, unchanged. The case is made
    // MORE specific by the verdict above, never less.
    CHECK(dominantPixel(configured) == configuredWanted);
    CHECK(dominantPixel(shipped) == shippedWanted);
    CHECK(dominantPixel(shipped) != dominantPixel(configured));
    CHECK(countOf(shipped, configuredWanted) == 0);
}

TEST_CASE("menu-font changes the height of the root menu a real click opens",
          "[wm_config_runtime]")
{
    // WindowManager::menu() computes entryHeight = ascent + descent + 4 and
    // outerH = entryHeight * n + 13. Nothing clamps that height -- only the
    // menu's POSITION is pushed back on screen -- so with the same entry count
    // in both runs the mapped menu's height is a strictly monotonic function of
    // the face m_menuFont resolved to.
    //
    // The height is read from the server, off the menu window the WM actually
    // mapped. Both runs use cleanFixture with no client mapped and identical
    // environment, so n is identical and the only free variable is the font.
    auto measure = [](const std::vector<std::string>& args) {
        WmFixture fixture(cleanFixture(args));
        x11::DisplayPtr dp = fixture.openDisplay();
        REQUIRE(dp != nullptr);
        Display* d = dp.get();
        parkPointer(d);

        XTestDriver driver(fixture.display());
        driver.moveTo(kParkX, kParkY);

        Window menu = None;
        Rect menuRect{};
        const bool opened =
            openRootMenu(d, driver, kMenuPressX, kMenuPressY, menu, menuRect);

        // Bound before the assertion that may fail (08.5-06 guard ordering).
        const std::string stderrText = fixture.wmStderr();

        INFO("menu rect " << describe(menuRect));
        INFO("wm stderr:\n" << stderrText);
        REQUIRE(opened);

        dismissMenu(d, driver);
        CHECK(joined(xProtocolErrorsExceptBadWindow(stderrText)).empty());

        return menuRect.h;
    };

    // Size 32 of the shipped family chain, for the same reason the tab-font case
    // uses it: resolvable wherever the default is, so the case cannot fail
    // because a host lacks some particular family.
    const int shipped = measure({});
    const int large   =
        measure({"--menu-font=Ubuntu,Noto Sans,DejaVu Sans,Sans:size=32"});

    INFO("shipped menu height=" << shipped << " large menu height=" << large);
    CHECK(shipped > 0);
    CHECK(large > shipped);
}

TEST_CASE("An unresolvable menu-font still lets the window manager start",
          "[wm_config_runtime]")
{
    // T-9-02. The menu font load keeps its generic-sans second rung and its
    // fatal() on total failure: the menu measures every row against this font,
    // so there is no "carry on without it" the way there is for the tab. This
    // plan does not change that contract, and this case is what says so -- only
    // a host with no sans font AT ALL reaches the fatal, which is exactly the
    // condition that already ended startup before `menu-font` existed.
    WmFixture fixture(cleanFixture(
        {"--menu-font=NoSuchFontFamilyAnywhere12345:size=12"}));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    parkPointer(d);

    Window win = None;
    Window frame = mapClientAndAwaitFrame(d, 200, 150, 300, 220, win, "nonsense-menu-font");

    const std::string stderrText = fixture.wmStderr();
    INFO("wm stderr:\n" << stderrText);

    CHECK(fixture.wmAlive());
    REQUIRE(frame != None);
    CHECK(joined(xProtocolErrorsExceptBadWindow(stderrText)).empty());
}

TEST_CASE("destroy-window-delay decides whether a tab-button press hides or deletes",
          "[wm_config_runtime]")
{
    // Border::eventButton()'s tab-button loop starts with action == 1 (hide) and
    // switches to action == 2 (delete) once the accumulated press time passes
    // config().destroyWindowDelay. A short press must therefore hide and a long
    // press must delete -- and BOTH halves are needed: a build that always hid
    // passes a delete-only test's precondition, and a build that always deleted
    // passes a hide-only one.
    //
    // 250ms rather than the shipped 1500ms so the long press is quick, but still
    // genuinely crossing the configured threshold: the hold below is more than
    // twice it. The loop also abandons entirely past 5000ms, so the hold must
    // stay well under that too.
    constexpr int kDelayMs = 250;

    WmFixture fixture(cleanFixture({"--destroy-window-delay=" + std::to_string(kDelayMs)}));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    parkPointer(d);

    XTestDriver driver(fixture.display());
    driver.moveTo(kParkX, kParkY);

    // Two clients, so the two halves do not have to share one window's state.
    //
    // WM_PROTOCOLS is announced BEFORE the map, not after. Client::getProtocols()
    // runs inside Client::manage(), so a protocol announced afterwards is not
    // seen -- and Client::kill() then falls through to XKillClient, which closes
    // the connection of the window's owner. That owner is this test, so the case
    // died mid-run with "X connection broken" and took its own assertions with
    // it. Same ordering constraint as the class hint in tests/test_wm_rules.cpp.
    Window shortWin = createClient(d, 120, 90, 220, 160, "shortpress");
    announceDeleteProtocol(d, shortWin);
    XMapWindow(d, shortWin);
    XSync(d, False);
    Window shortFrame = awaitFrameFor(d, shortWin);
    REQUIRE(shortFrame != None);

    Window longWin = createClient(d, 520, 380, 220, 160, "longpress");
    announceDeleteProtocol(d, longWin);
    XMapWindow(d, longWin);
    XSync(d, False);
    Window longFrame = awaitFrameFor(d, longWin);
    REQUIRE(longFrame != None);
    settleWm(d);

    auto pressButtonOf = [&](Window frame, Window client, int holdMs) {
        // The tab button is only REACHABLE on an active client. Under the
        // shipped pointer-focus policy that means putting the pointer on the
        // window first: setFrameVisibility(false, ...) SUBTRACTS the button's
        // square from the frame's bounding shape for an inactive client, so a
        // press aimed at it would fall through to the root window and open the
        // menu instead. Nothing about that announces itself -- the button's own
        // geometry is unchanged and findFrameChild() still finds it.
        const Rect c = rectOf(d, client);
        driver.moveTo(c.x + c.w / 2, c.y + c.h / 2);
        XSync(d, False);
        REQUIRE(WmFixture::pollUntil([&] {
            pumpWm(d);
            return activeWindow(d) == client;
        }, 8000));

        Window button = findFrameChild(d, frame, client, true);
        REQUIRE(button != None);
        const Rect b = rectOf(d, button);
        REQUIRE(b.w > 0);
        driver.moveTo(b.x + b.w / 2, b.y + b.h / 2);
        XSync(d, False);
        driver.press(Button1);
        if (holdMs > 0) holdFor(holdMs);
        driver.release(Button1);
        XSync(d, False);
    };

    auto sawDelete = [&](Window target) {
        static Atom protocols = XInternAtom(d, "WM_PROTOCOLS", False);
        static Atom del = XInternAtom(d, "WM_DELETE_WINDOW", False);
        XEvent ev;
        while (XCheckTypedWindowEvent(d, target, ClientMessage, &ev)) {
            if (ev.xclient.message_type == protocols &&
                static_cast<Atom>(ev.xclient.data.l[0]) == del) {
                return true;
            }
        }
        return false;
    };

    // --- short press: hide, no delete -------------------------------------
    pressButtonOf(shortFrame, shortWin, 0);

    long state = -1;
    REQUIRE(WmFixture::pollUntil([&] {
        pumpWm(d);
        return icccmState(d, shortWin, state) && state == IconicState;
    }, 8000));

    settleWm(d);
    INFO("wm stderr:\n" << fixture.wmStderr());
    CHECK_FALSE(sawDelete(shortWin));

    // --- long press: delete, no hide --------------------------------------
    pressButtonOf(longFrame, longWin, kDelayMs * 3);

    REQUIRE(WmFixture::pollUntil([&] {
        pumpWm(d);
        return sawDelete(longWin);
    }, 8000));

    settleWm(d);
    long longState = -1;
    REQUIRE(icccmState(d, longWin, longState));
    INFO("long-press client WM_STATE: " << longState);
    CHECK(longState == NormalState);

    REQUIRE(fixture.wmAlive());
    INFO("wm stderr:\n" << fixture.wmStderr());
    CHECK(joined(xProtocolErrorsExceptBadWindow(fixture.wmStderr())).empty());
}

TEST_CASE("new-window-command decides which program the menu's New entry starts",
          "[wm_config_runtime]")
{
    // The identity of the launched program is read from the class hint of the
    // window that appears, because that is the only thing about a spawned
    // process observable from outside the WM.
    WmFixture fixture(cleanFixture({std::string("--new-window-command=") + kProbeProgram}));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    parkPointer(d);

    XTestDriver driver(fixture.display());
    driver.moveTo(kParkX, kParkY);

    const std::vector<Window> before = clientList(d);

    Window menu = None;
    Rect menuRect{};
    REQUIRE(openRootMenu(d, driver, kMenuPressX, kMenuPressY, menu, menuRect));
    selectFirstMenuEntry(d, driver, menuRect);

    const Window spawned = awaitClientWithClass(d, kProbeClass, before, kSpawnObserveMs);
    INFO("wm stderr:\n" << fixture.wmStderr());
    INFO("client list size: " << clientList(d).size());
    REQUIRE(spawned != None);
    CHECK(listed(d, spawned));

    REQUIRE(fixture.wmAlive());
}

TEST_CASE("exec-using-shell gates whether a command with arguments and "
          "metacharacters is evaluated by a shell", "[wm_config_runtime]")
{
    // Two halves that share one command string. The positive half proves the
    // witness mechanism works at all; without it the negative half could pass
    // because the sentinel path was wrong, the menu never opened, or the WM
    // never spawned anything -- none of which is the claim being made.
    //
    // The command carries BOTH an argument and a shell metacharacter, so the two
    // halves separate "arguments require shell mode" (functional) from "with
    // shell mode off nothing is evaluated by a shell" (threat T-8-SHELL).
    //
    // The window manager's stderr is written out through `stderrOut` rather
    // than annotated inside the lambda: an INFO declared here dies at the
    // `return` below, so the three positive-control assertions in the
    // enclosing scope reported with no `with message:` block. Measured against
    // a deliberate break in 08.5-06: absent before this hoist, present after.
    auto run = [](bool shellEnabled, const char* tag, bool& sentinelSeen,
                  Window& spawnedOut, std::string& stderrOut) {
        const std::string sentinel = sentinelPath(tag);
        ::unlink(sentinel.c_str());

        // The witness comes FIRST. A shell runs the program in the foreground,
        // so a witness placed after it would not be written until the program
        // exits -- and the program is expected to still be running when the
        // case reads for it. Measured: with the two the other way round the
        // positive control failed against a shell that had done exactly what it
        // was asked.
        const std::string command = "touch " + sentinel + "; " +
                                    std::string(kProbeProgram) + " -name shellmarker";

        std::vector<std::string> args{"--new-window-command=" + command};
        args.push_back(shellEnabled ? "--exec-using-shell" : "--no-exec-using-shell");

        WmFixture fixture(cleanFixture(args));
        x11::DisplayPtr dp = fixture.openDisplay();
        REQUIRE(dp != nullptr);
        Display* d = dp.get();
        parkPointer(d);

        XTestDriver driver(fixture.display());
        driver.moveTo(kParkX, kParkY);

        const std::vector<Window> before = clientList(d);

        Window menu = None;
        Rect menuRect{};
        // Verified open, as the three [wm_menulabel] sites use (WINDOWS.md
        // ledger 13): the raw helper had no retry around deferred item 17, so
        // one never-opens run failed this case's positive control after the
        // whole 20 s stage budget, and the failure said nothing about shells.
        std::string why;
        const bool opened = openRootMenuVerified(d, driver, kMenuPressX, kMenuPressY,
                                                 menu, menuRect, why,
                                                 namedPixel(d, kShippedMenuBackground));
        INFO("menu open diagnostics: " << why);
        REQUIRE(opened);
        selectFirstMenuEntry(d, driver, menuRect);

        spawnedOut = awaitClientWithClass(d, kProbeClass, before,
                                          shellEnabled ? kSpawnObserveMs : kSpawnAbsentMs);

        // The sentinel is written by the SECOND half of the command, after the
        // program starts, so it is polled on its own deadline rather than read
        // once. On the negative side the poll is expected to run out -- which is
        // exactly why it has a deadline and not a single read.
        sentinelSeen = WmFixture::pollUntil([&] {
            return pathExists(sentinel);
        }, shellEnabled ? kSpawnObserveMs : kSpawnAbsentMs);

        INFO("wm stderr:\n" << fixture.wmStderr());
        CHECK(fixture.wmAlive());

        std::string instance;
        if (spawnedOut != None) instance = instanceOf(d, spawnedOut);
        ::unlink(sentinel.c_str());
        stderrOut = fixture.wmStderr();
        return instance;
    };

    bool shellSentinel = false, plainSentinel = false;
    Window shellWindow = None, plainWindow = None;
    std::string shellErrs, plainErrs;

    const std::string shellInstance = run(true, "on", shellSentinel, shellWindow, shellErrs);
    const std::string plainInstance = run(false, "off", plainSentinel, plainWindow, plainErrs);

    INFO("wm stderr (shell mode on):\n" << shellErrs);
    INFO("wm stderr (shell mode off):\n" << plainErrs);

    // Positive control: with shell mode ON the argument took effect (the window
    // carries the instance name the argument set) and the metacharacter really
    // was evaluated (the sentinel exists).
    CHECK(shellWindow != None);
    CHECK(shellInstance == "shellmarker");
    CHECK(shellSentinel);

    // The claim: with shell mode OFF nothing at all was executed through a
    // shell. Not merely "the expected window did not appear" -- the sentinel is
    // the direct evidence that the second command never ran.
    CHECK(plainWindow == None);
    CHECK_FALSE(plainSentinel);
    CHECK(plainInstance.empty());
}

TEST_CASE("A setting given on the command line overrides the same setting in the "
          "config file", "[wm_config_runtime]")
{
    // Observed through the running binary, not the parser: frame thickness is
    // the setting because its effect is an exact, absolute geometry fact
    // (yIndent() == FRAME_WIDTH + 1).
    auto verticalInset = [](const std::string& configContents,
                            const std::vector<std::string>& args) {
        WmFixture fixture(configuredFixture(configContents, args));
        x11::DisplayPtr dp = fixture.openDisplay();
        REQUIRE(dp != nullptr);
        Display* d = dp.get();
        parkPointer(d);

        Window win = None;
        Window frame = mapClientAndAwaitFrame(d, 200, 150, 300, 220, win, "precedence");
        REQUIRE(frame != None);
        settleWm(d);

        INFO("wm stderr:\n" << fixture.wmStderr());
        return rectOf(d, win).y - rectOf(d, frame).y;
    };

    // Control: the config FILE is genuinely read. Without this the precedence
    // assertion below would pass just as well against a binary that ignored the
    // file entirely -- which is the failure mode most worth catching, because it
    // is what "the setting parses but reaches nothing" looks like from outside.
    const int fileOnly = verticalInset("frame-thickness = 3\n", {});
    CHECK(fileOnly == 3 + 1);

    // And the CLI wins over it.
    const int cliWins = verticalInset("frame-thickness = 3\n", {"--frame-thickness=20"});
    CHECK(cliWins == 20 + 1);
    CHECK(cliWins != fileOnly);
}


// ===========================================================================
// [wm_errors] -- the terminating X11 error paths (coverage item 6)
//                and the help flag
// ===========================================================================

TEST_CASE("With no display available the binary exits non-zero and names the display",
          "[wm_errors]")
{
    const RunResult r = runBinary({}, {}, /*unsetDisplay=*/true, kFatalTimeoutMs);

    INFO("output:\n" << r.output);
    CHECK_FALSE(r.timedOut);
    CHECK(r.exitedNormally);
    CHECK(r.exitCode != 0);

    // The message must name the DISPLAY as the problem. A bare non-zero exit
    // would be satisfied by any of the other three conditions in this group, so
    // the exit status alone does not identify the path that was taken.
    CHECK(contains(r.output, "can't open display"));
    CHECK(sanitizerClean(r.output));
}

TEST_CASE("An unparseable colour setting exits non-zero and names the offending setting",
          "[wm_errors]")
{
    BareDisplay server;

    // menu-borders is allocated in initialiseScreen() through
    // WindowManager::allocateColour(), whose failure message carries the
    // caller-supplied description -- so the stderr names WHICH colour setting
    // was wrong rather than reporting a generic allocation failure. The tab and
    // frame colours are allocated later, in the first Border, so a case using
    // one of those would additionally need a client to exist.
    const RunResult r = runBinary({"--menu-borders=definitely-not-a-colour"},
                                  {{"DISPLAY", server.display()}},
                                  false, kFatalTimeoutMs);

    INFO("output:\n" << r.output);
    CHECK_FALSE(r.timedOut);
    CHECK(r.exitedNormally);
    CHECK(r.exitCode != 0);
    CHECK(contains(r.output, "menu border"));
    CHECK(contains(r.output, "colour"));
    CHECK(sanitizerClean(r.output));

    // A valid colour on the same display and the same flag must NOT die, or the
    // case above proves only that the binary dislikes being started at all.
    const RunResult ok = runBinary({"--menu-borders=blue"},
                                   {{"DISPLAY", server.display()}},
                                   false, 4000);
    INFO("control output:\n" << ok.output);
    CHECK(ok.timedOut);          // it started successfully and had to be killed
    CHECK_FALSE(contains(ok.output, "couldn't load"));
}

TEST_CASE("With no font available at all the binary exits non-zero and names the menu font",
          "[wm_errors]")
{
    BareDisplay server;

    const std::string fontConfig = makeEmptyFontConfig();
    const RunResult r = runBinary({},
                                  {{"DISPLAY", server.display()},
                                   {"FONTCONFIG_FILE", fontConfig}},
                                  false, kFatalTimeoutMs);

    INFO("FONTCONFIG_FILE: " << fontConfig);
    INFO("output:\n" << r.output);
    CHECK_FALSE(r.timedOut);
    CHECK(r.exitedNormally);
    CHECK(r.exitCode != 0);

    // The MENU font is the fatal one: initialiseScreen() tries the configured
    // pattern, then a generic fallback, and calls fatal() when neither resolves.
    //
    // This subprocess starts NO CLIENT, so it never constructs a Border and
    // never reaches the rotated-tab font ladder. It must not be read as evidence
    // about that ladder; plan 08-06's RENDER-less group is what proves the tab
    // font degrades without terminating, and the two claims are deliberately
    // kept apart.
    CHECK(contains(r.output, "menu font"));
    CHECK(sanitizerClean(r.output));
}

TEST_CASE("A second window manager fails cleanly and leaves the incumbent working",
          "[wm_errors]")
{
    WmFixture incumbent;
    x11::DisplayPtr dp = incumbent.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    parkPointer(d);

    const RunResult r = runBinary({}, {{"DISPLAY", incumbent.display()}},
                                  false, kFatalTimeoutMs);

    INFO("second WM output:\n" << r.output);
    CHECK_FALSE(r.timedOut);
    CHECK(r.exitedNormally);
    CHECK(r.exitCode != 0);

    // It must identify the CONFLICT, not report a generic protocol error and
    // leave the user to work out that another window manager is the cause.
    CHECK(contains(r.output, "another window manager"));
    CHECK(sanitizerClean(r.output));

    // And the incumbent must be untouched. A conflict that takes down the
    // running session would be considerably worse than one that fails cleanly,
    // and "still alive" is not the same claim as "still working" -- so this
    // maps a fresh client and requires it to be framed and published.
    REQUIRE(incumbent.wmAlive());

    Window win = None;
    Window frame = mapClientAndAwaitFrame(d, 240, 180, 260, 190, win, "survivor");
    INFO("incumbent stderr:\n" << incumbent.wmStderr());
    REQUIRE(frame != None);
    CHECK(listed(d, win));
}

TEST_CASE("Every terminating path exits within a deadline rather than hanging",
          "[wm_errors]")
{
    // A path that HANGS instead of exiting is a distinct and worse failure than
    // one that exits with the wrong status -- the process sits there holding a
    // display, and only a deadline tells the two apart. The four cases above use
    // a generous timeout so a loaded host does not flake; this one re-runs the
    // same launches against a tight deadline and reports what each measured.
    BareDisplay server;
    const std::string fontConfig = makeEmptyFontConfig();

    struct Path { const char* name; RunResult result; };
    std::vector<Path> paths;

    paths.push_back({"no display",
                     runBinary({}, {}, true, kFatalDeadlineMs)});

    paths.push_back({"invalid colour",
                     runBinary({"--menu-borders=definitely-not-a-colour"},
                               {{"DISPLAY", server.display()}}, false, kFatalDeadlineMs)});

    paths.push_back({"no font",
                     runBinary({}, {{"DISPLAY", server.display()},
                                    {"FONTCONFIG_FILE", fontConfig}},
                               false, kFatalDeadlineMs)});

    {
        WmFixture incumbent;
        paths.push_back({"root redirect conflict",
                         runBinary({}, {{"DISPLAY", incumbent.display()}},
                                   false, kFatalDeadlineMs)});
    }

    for (const Path& p : paths) {
        INFO("path: " << p.name << " elapsed=" << p.result.elapsedMs << "ms"
             << " exit=" << p.result.exitCode
             << "\noutput:\n" << p.result.output);
        CHECK_FALSE(p.result.timedOut);
        CHECK(p.result.elapsedMs < kFatalDeadlineMs);
        CHECK(p.result.exitCode != 0);
    }
}

TEST_CASE("The help flag prints usage and exits successfully", "[wm_errors]")
{
    // Run with NO DISPLAY, which is the case that matters: help that needs an X
    // server is help you cannot read when you are trying to work out why the
    // window manager will not start.
    const RunResult r = runBinary({"--help"}, {}, true, kFatalTimeoutMs);

    INFO("output:\n" << r.output);
    CHECK_FALSE(r.timedOut);
    REQUIRE(r.exitedNormally);
    CHECK(r.exitCode == 0);

    // At least the string settings and BOTH halves of the boolean pairs. The
    // usage text is generated from the same rows getopt_long() is driven by, so
    // these are spot checks on a generated list rather than a second copy of it.
    CHECK(contains(r.output, "--frame-thickness"));
    CHECK(contains(r.output, "--new-window-command"));
    CHECK(contains(r.output, "--tab-foreground"));
    CHECK(contains(r.output, "--auto-raise"));
    CHECK(contains(r.output, "--no-auto-raise"));
    CHECK(contains(r.output, "--no-click-to-focus"));
    CHECK(contains(r.output, "--no-focus-stealing-prevention"));
    CHECK(contains(r.output, "--destroy-window-delay"));
    CHECK(contains(r.output, "--exec-using-shell"));
    CHECK(sanitizerClean(r.output));
}

TEST_CASE("An unrecognised flag exits non-zero and its advice names a flag that works",
          "[wm_errors]")
{
    const RunResult bad = runBinary({"--nosuchflag"}, {}, true, kFatalTimeoutMs);

    INFO("output:\n" << bad.output);
    CHECK_FALSE(bad.timedOut);
    REQUIRE(bad.exitedNormally);
    CHECK(bad.exitCode != 0);
    CHECK(contains(bad.output, "unrecognized option"));

    // The advice line. Before this plan it pointed at a flag that was not in the
    // option table, so following it produced the same error again -- the binary
    // told the user to do something it then refused to do.
    REQUIRE(contains(bad.output, "--help"));

    // Follow the advice, from the text itself rather than from a literal this
    // test happens to agree with. A future rewording that pointed somewhere else
    // would take this assertion with it.
    const std::string advice = bad.output.substr(bad.output.find("--help"));
    std::string flag;
    for (char ch : advice) {
        if (ch == '\'' || ch == ' ' || ch == '\n') break;
        flag.push_back(ch);
    }
    INFO("following the binary's own advice: " << flag);

    const RunResult advised = runBinary({flag}, {}, true, kFatalTimeoutMs);
    INFO("advised output:\n" << advised.output);
    CHECK_FALSE(advised.timedOut);
    REQUIRE(advised.exitedNormally);
    CHECK(advised.exitCode == 0);
    CHECK_FALSE(contains(advised.output, "unrecognized option"));
}


// ===========================================================================
// [wm_stress] -- 100+ window create/map/unmap/destroy churn (coverage item 13)
// ===========================================================================

namespace {

// The resident-set reader moved to tests/support/WmFixture.h in plan 09-09,
// which is also where test_wm_resource.cpp's copy went. This file had the THIRD
// copy; leaving it here made every call ambiguous the moment the shared one
// arrived, because `using namespace wm2test` at the top of this file puts both
// in scope. Three readers of one /proc field, in one suite, measuring one
// 512 MB budget, was never right -- and it is what the plan that added the
// settings window's own budget case set out to avoid.

// Zombie children of `parent`. The WM double-forks so its grandchildren are
// orphaned to init and can never be zombies; what this counts is the
// INTERMEDIATE child, which spawn() is supposed to reap with wait().
int zombieChildrenOf(pid_t parent)
{
    int zombies = 0;
    DIR* dp = ::opendir("/proc");
    if (!dp) return 0;
    while (struct dirent* e = ::readdir(dp)) {
        const char* name = e->d_name;
        if (name[0] < '0' || name[0] > '9') continue;

        const std::string statPath = std::string("/proc/") + name + "/stat";
        FILE* f = std::fopen(statPath.c_str(), "rb");
        if (!f) continue;
        std::string content;
        char buf[1024];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) content.append(buf, n);
        std::fclose(f);

        // Parse after the LAST ')': the comm field is parenthesised and may
        // itself contain spaces and parentheses.
        const size_t close = content.find_last_of(')');
        if (close == std::string::npos) continue;
        std::istringstream in(content.substr(close + 1));
        std::string state;
        long ppid = 0;
        if (!(in >> state >> ppid)) continue;
        if (ppid == parent && state == "Z") ++zombies;
    }
    ::closedir(dp);
    return zombies;
}

// A FIXED, DOCUMENTED growth budget, chosen BEFORE the test was first run and
// deliberately not derived from any measurement this case takes.
//
// Reasoning behind the numbers, so a future reader can argue with it rather
// than guess at it: a managed window costs the WM one Client, one Border and a
// handful of X resource handles -- on the order of a kilobyte of heap each --
// and the churn below never holds more than a batch at a time. The m_clients
// vector reaching a few hundred entries is about a kilobyte. So a leak-free run
// should grow by well under a megabyte, and everything above that is headroom
// for allocator fragmentation and glibc arena behaviour.
//
// Under the sanitizer every allocation gains redzones and freed memory sits in
// a quarantine before it is returned, so the same churn legitimately holds
// several times as much. Hence a separate, larger constant rather than one
// loose number that would be meaningless in the debug tree.
//
// If a run exceeds these, that is a FINDING. It is not a budget to raise.
constexpr long kGrowthBudgetKbDebug = 8192;    // 8 MB
constexpr long kGrowthBudgetKbAsan  = 32768;   // 32 MB

#if defined(__SANITIZE_ADDRESS__)
constexpr long kGrowthBudgetKb = kGrowthBudgetKbAsan;
constexpr const char* kGrowthBudgetTree = "asan";
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
constexpr long kGrowthBudgetKb = kGrowthBudgetKbAsan;
constexpr const char* kGrowthBudgetTree = "asan";
#  else
constexpr long kGrowthBudgetKb = kGrowthBudgetKbDebug;
constexpr const char* kGrowthBudgetTree = "debug";
#  endif
#else
constexpr long kGrowthBudgetKb = kGrowthBudgetKbDebug;
constexpr const char* kGrowthBudgetTree = "debug";
#endif

constexpr int kBatchSize  = 40;
constexpr int kBatchCount = 3;    // 120 windows, comfortably past the required 100

// How many of a batch are unmapped, and where the destroyed range starts, so
// the two subsets OVERLAP rather than partition. A strictly sequential
// create-then-destroy loop would never exercise the container-mutation ordering
// that an overlapping unmap/destroy pair does.
constexpr int kUnmapCount   = 20;
constexpr int kDestroyStart = 10;
constexpr int kDestroyCount = 20;

// True once every window in `wins` is published in _NET_CLIENT_LIST. Reads the
// whole list once per poll rather than querying each window: 40 windows a poll
// through XQueryTree would make the case slower than the churn it measures.
bool allListed(Display* d, const std::vector<Window>& wins)
{
    const std::vector<Window> l = clientList(d);
    for (Window w : wins) {
        if (std::find(l.begin(), l.end(), w) == l.end()) return false;
    }
    return true;
}

bool noneListed(Display* d, const std::vector<Window>& wins)
{
    const std::vector<Window> l = clientList(d);
    for (Window w : wins) {
        if (std::find(l.begin(), l.end(), w) != l.end()) return false;
    }
    return true;
}

}  // namespace

TEST_CASE("Repeated create, map, unmap and destroy over 120 windows leaves the WM "
          "correct, bounded and free of zombies", "[wm_stress]")
{
    WmFixture fixture;
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    parkPointer(d);

    // The baseline is taken AFTER the first window is managed, not at process
    // start: the one-off allocations of initialisation, the font, the colours
    // and the first Border's statics all belong to startup rather than to the
    // churn, and charging them to the churn would make the budget below a
    // measurement of startup instead.
    Window warmup = None;
    REQUIRE(mapClientAndAwaitFrame(d, 10, 10, 80, 60, warmup, "warmup") != None);
    settleWm(d);

    long baselineKb = 0;
    REQUIRE(residentKb(fixture.wm().pid(), baselineKb));

    XDestroyWindow(d, warmup);
    XSync(d, False);

    std::vector<Window> everCreated;

    for (int batch = 0; batch < kBatchCount; ++batch) {
        std::vector<Window> wins;
        wins.reserve(kBatchSize);

        // create
        for (int i = 0; i < kBatchSize; ++i) {
            const int x = 20 + (i % 8) * 100;
            const int y = 20 + (i / 8) * 120;
            wins.push_back(createClient(d, x, y, 70, 50, "churn"));
        }
        everCreated.insert(everCreated.end(), wins.begin(), wins.end());

        // map, and wait for the WM to have taken all of them under management
        for (Window w : wins) XMapWindow(d, w);
        XSync(d, False);
        INFO("batch " << batch << ": waiting for " << wins.size() << " windows to be managed");
        REQUIRE(WmFixture::pollUntil([&] {
            pumpWm(d);
            return allListed(d, wins);
        }, 60000));

        // unmap a subset...
        for (int i = 0; i < kUnmapCount && i < kBatchSize; ++i) {
            XUnmapWindow(d, wins[static_cast<size_t>(i)]);
        }
        XSync(d, False);

        // ...and destroy a DIFFERENT, OVERLAPPING subset. Half of these are
        // already unmapped and half are still mapped, so the destroy handler is
        // entered from both states within one batch -- which is the ordering a
        // sequential loop never reaches.
        std::vector<Window> destroyed;
        for (int i = kDestroyStart;
             i < kDestroyStart + kDestroyCount && i < kBatchSize; ++i) {
            destroyed.push_back(wins[static_cast<size_t>(i)]);
            XDestroyWindow(d, wins[static_cast<size_t>(i)]);
        }
        XSync(d, False);

        if (!WmFixture::pollUntil([&] {
                pumpWm(d);
                return noneListed(d, destroyed);
            }, 60000)) {
            const std::vector<Window> l = clientList(d);
            std::string leftover;
            for (Window w : destroyed) {
                if (std::find(l.begin(), l.end(), w) != l.end()) {
                    leftover += " " + std::to_string(w);
                }
            }
            INFO("wm alive: " << fixture.wmAlive());
            INFO("still listed after destroy:" << leftover);
            INFO("wm stderr:\n" << fixture.wmStderr());
            FAIL("destroyed windows never left _NET_CLIENT_LIST");
        }

        // remap what was unmapped but not destroyed, so the next batch is laid
        // over a live population rather than a clean slate
        for (int i = 0; i < kUnmapCount && i < kDestroyStart; ++i) {
            XMapWindow(d, wins[static_cast<size_t>(i)]);
        }
        XSync(d, False);

        // tear the rest of this batch down
        for (int i = 0; i < kBatchSize; ++i) {
            if (i >= kDestroyStart && i < kDestroyStart + kDestroyCount) continue;
            XDestroyWindow(d, wins[static_cast<size_t>(i)]);
        }
        XSync(d, False);

        REQUIRE(WmFixture::pollUntil([&] {
            pumpWm(d);
            return noneListed(d, wins);
        }, 60000));
    }

    CHECK(everCreated.size() >= 100);
    settleWm(d);

    // --- 1: no stale client-list entry survived ---------------------------
    //
    // Filtered to the windows this case owns (deferred item 7: the WM adopts its
    // own menu, submenu and EWMH check windows, so the list is never literally
    // empty).
    const std::vector<Window> remaining = clientList(d);
    std::vector<Window> stale;
    for (Window w : everCreated) {
        if (std::find(remaining.begin(), remaining.end(), w) != remaining.end()) {
            stale.push_back(w);
        }
    }
    INFO("stale client-list entries: " << stale.size());
    CHECK(stale.empty());

    // --- 2: the active window does not name anything destroyed ------------
    const Window active = activeWindow(d);
    INFO("_NET_ACTIVE_WINDOW: " << active);
    CHECK(std::find(everCreated.begin(), everCreated.end(), active) == everCreated.end());
    CHECK(active != warmup);

    // --- 3: the WM is still WORKING, not merely still running -------------
    //
    // "Alive" and "correct" are different claims, and a WM whose client
    // bookkeeping had been corrupted by the churn would satisfy the first.
    Window fresh = None;
    Window freshFrame = mapClientAndAwaitFrame(d, 300, 250, 240, 180, fresh, "after-churn");
    INFO("wm stderr:\n" << fixture.wmStderr());
    REQUIRE(freshFrame != None);
    CHECK(listed(d, fresh));
    CHECK(parentOf(d, fresh) == freshFrame);
    CHECK(parentOf(d, freshFrame) == DefaultRootWindow(d));

    // --- 3b: a WM_COLORMAP_WINDOWS entry that dies under the WM's feet ----
    //
    // Folded into this case rather than given one of its own because it is the
    // same claim -- the WM holds a reference to a window a client can destroy at
    // any moment -- and because it exists at all only for an honest reason:
    // deleting the return check on the SECOND XGetWindowAttributes in
    // Client::getColormaps() left the whole suite green. That is not an
    // equivalent mutant, it is an uncovered branch, and the branch is only
    // reachable through WM_COLORMAP_WINDOWS, which nothing else here writes.
    //
    // Note the property is written TWICE: the second write is what makes
    // getColormaps() run again with the referenced window already gone, which
    // is the state in which an unchecked read yields the PREVIOUS window's
    // colormap. The client must also be ACTIVE, or eventProperty() refreshes
    // the array without installing anything and the bad value never reaches the
    // server.
    {
        XTestDriver driver(fixture.display());
        const Rect fr = rectOf(d, fresh);
        driver.moveTo(fr.x + fr.w / 2, fr.y + fr.h / 2);
        XSync(d, False);
        REQUIRE(WmFixture::pollUntil([&] {
            pumpWm(d);
            return activeWindow(d) == fresh;
        }, 8000));

        Window cmapWin = createClient(d, 760, 40, 40, 30, "colormap-donor");
        XMapWindow(d, cmapWin);
        XSync(d, False);

        const Atom cmapProp = XInternAtom(d, "WM_COLORMAP_WINDOWS", False);
        XChangeProperty(d, fresh, cmapProp, XA_WINDOW, 32, PropModeReplace,
                        reinterpret_cast<unsigned char*>(&cmapWin), 1);
        XSync(d, False);
        settleWm(d);

        XDestroyWindow(d, cmapWin);
        XSync(d, False);
        settleWm(d);

        XChangeProperty(d, fresh, cmapProp, XA_WINDOW, 32, PropModeReplace,
                        reinterpret_cast<unsigned char*>(&cmapWin), 1);
        XSync(d, False);
        settleWm(d);

        XDeleteProperty(d, fresh, cmapProp);
        XSync(d, False);
        settleWm(d);

        driver.moveTo(kParkX, kParkY);
        XSync(d, False);
        settleWm(d);

        // Still framed, still managed, still the WM's client -- the point is
        // that the bad reference changed nothing observable, not merely that
        // the WM stayed up.
        CHECK(listed(d, fresh));
        CHECK(parentOf(d, fresh) == freshFrame);
    }

    // --- 4: resident memory is inside a FIXED, pre-chosen budget ----------
    long afterKb = 0;
    REQUIRE(residentKb(fixture.wm().pid(), afterKb));
    const long growthKb = afterKb - baselineKb;

    // Baseline, budget and result are recorded separately and in full, so the
    // number that matters is in the log whether the case passes or fails --
    // and so nobody can later mistake the budget for something derived from
    // the result.
    std::printf("[wm_stress] resident memory (%s tree): "
                "post-startup baseline %ld kB, "
                "after %zu windows %ld kB, "
                "growth %ld kB, fixed budget %ld kB\n",
                kGrowthBudgetTree, baselineKb, everCreated.size(), afterKb,
                growthKb, kGrowthBudgetKb);
    std::fflush(stdout);

    UNSCOPED_INFO("resident memory (" << kGrowthBudgetTree << " tree)"
                  << ": post-startup baseline " << baselineKb << " kB"
                  << ", after " << everCreated.size() << " windows " << afterKb << " kB"
                  << ", growth " << growthKb << " kB"
                  << ", fixed budget " << kGrowthBudgetKb << " kB");
    CHECK(growthKb < kGrowthBudgetKb);

    // --- 5: repeated spawn() leaves no zombie -----------------------------
    //
    // The checklist's zombie item shares this case's shape, so it is folded in
    // here rather than given a fixture of its own. WmFixture launches the WM
    // with a real, preflight-checked new-window command, and menu entry 0 runs
    // it; three selections exercise the double-fork reaping guarantee three
    // times over.
    {
        XTestDriver driver(fixture.display());
        driver.moveTo(kParkX, kParkY);

        for (int i = 0; i < 3; ++i) {
            Window menu = None;
            Rect menuRect{};
            REQUIRE(openRootMenu(d, driver, kMenuPressX, kMenuPressY, menu, menuRect));
            selectFirstMenuEntry(d, driver, menuRect);
            settleWm(d);
        }
    }

    settleWm(d);
    const int zombies = zombieChildrenOf(fixture.wm().pid());
    INFO("zombie children of the WM: " << zombies);
    CHECK(zombies == 0);

    // --- the WM survived, and survived without protocol errors ------------
    //
    // One error class is separated out rather than simply required absent, and
    // the reason is worth stating because "we excluded the failing case" is
    // usually the wrong answer:
    //
    //   X_SetInputFocus ... BadMatch means the target window was not viewable
    //   when the request reached the server. Client::activate() DOES guard --
    //   it returns early unless the client is managed, not hidden and not
    //   withdrawn -- but the WM's state can only be as fresh as the last event
    //   it has processed, and under this churn a window is routinely destroyed
    //   between the WM deciding to focus it and the request arriving. No query
    //   can close that window; a viewability check would only move the race one
    //   round trip earlier. The server discards the request and nothing
    //   downstream is affected.
    //
    // So it is BOUNDED rather than excluded: a handful across 120 windows is the
    // race, and a count that scales with the churn would be a real regression in
    // the focus path. Every other error class still fails the case outright.
    REQUIRE(fixture.wmAlive());

    std::vector<std::string> focusRaces, otherErrors;
    for (const auto& line : xProtocolErrorsExceptBadWindow(fixture.wmStderr())) {
        if (line.find("X_SetInputFocus") != std::string::npos &&
            line.find("BadMatch") != std::string::npos) {
            focusRaces.push_back(line);
        } else {
            otherErrors.push_back(line);
        }
    }

    INFO("wm stderr:\n" << fixture.wmStderr());
    CHECK(joined(otherErrors).empty());
    INFO("focus races:\n" << joined(focusRaces));
    CHECK(focusRaces.size() <= 10);

    // --- the sanitizer is the primary evidence ----------------------------
    //
    // A clean SIGTERM exit runs the leak check, so a use-after-free or a leak in
    // the repeated path fails this case automatically in the asan tree. In the
    // debug tree the same lines assert an orderly shutdown after the churn,
    // which is worth having on its own.
    REQUIRE(fixture.terminateWmCleanly());
    REQUIRE(fixture.asanReports().empty());
}


// ===========================================================================
// [wm_tablabel] -- the sideways tab tracks the window title (deferred item 11)
//
// The wm2 sideways tab label is the product's stated non-negotiable visual
// identity. Deferred item 11 recorded, from 08-06, that the tab barely grows
// with the title: 325x54 for a one-character title against 325x58 for a
// thirty-four-character one. Plan 08-14's screenshots showed the consequence is
// not the overhanging label 08-06 predicted -- it is NO LABEL AT ALL, because
// the draw origin is computed on the same swapped axis and lands outside the
// tab, where it is clipped away.
//
// MEASURED on this host, rotated face at size 12, XftTextExtentsUtf8:
//
//     string                                 width   height
//     "M"                                       12       13
//     "Hello"                                   12       39
//     "A Very Long Window Title Indeed Yes"     16      286
//     40 digits                                 12      360
//
// So for a rotated font `width` is the CONSTANT thickness across the string and
// `height` is the along-string advance. Every rotated read in Border had the two
// the wrong way round.
//
// THE ASSERTIONS ARE ABSOLUTE, NOT RUN-COMPARISONS, and they are made on ONE
// window that is RETITLED rather than on two windows: two windows differ in
// stacking and active state, and the active client is decorated differently
// (deferred item 16), so a two-window comparison would be confounded by
// something other than the title. Retitling holds everything else fixed.
// ===========================================================================

namespace {

// A title long enough that its rotated advance (~286 px measured above) cannot
// possibly fit in the ~40 px tab the defect produces, and short enough to fit
// the tall test window once the defect is fixed.
const char* const kLongTitle = "A Very Long Window Title Indeed Yes";
const char* const kShortTitle = "A";

// Tall enough that fixTabHeight()'s maxHeight (the client height, less the tab
// width) leaves room for the long title's full advance, so a correct
// implementation is not forced into the ellipsis-shortening path. That path is
// real and now reachable, but it is not what this case is about.
constexpr int kTallWindowH = 460;

struct TabObservation {
    int tabLength = -1;      // the tab window's own height, parent-relative
    long inkPixels = 0;      // pixels of the configured FOREGROUND colour in it
};

// Retitle, let the WM notice, and read back the tab's length and how much label
// ink is actually on the screen inside it.
TabObservation observeTabFor(Display* d, Window frame, Window client,
                             unsigned long inkPixel, const char* title)
{
    TabObservation obs;

    XStoreName(d, client, title);
    XSync(d, False);
    settleWm(d);

    const Window tab = findFrameChild(d, frame, client, false);
    if (tab == None) return obs;

    Rect local;
    if (!localRect(d, tab, local)) return obs;
    obs.tabLength = local.h;

    // Read the SERVER's pixels over the tab's footprint, through root -- the tab
    // is shaped, and XGetImage outside a bounding shape is undefined (08-13).
    Rect abs;
    if (!serverRect(d, tab, abs)) return obs;
    obs.inkPixels = countOf(captureRoot(d, abs), inkPixel);
    return obs;
}

}  // namespace

TEST_CASE("The sideways tab grows with the window title and renders its label",
          "[wm_tablabel]")
{
    // Explicit colours so the ink assertion is ABSOLUTE -- the count is of the
    // pixel the server resolves the configured tab-foreground NAME to, not of
    // "some colour that differs from the other run".
    WmFixture fixture(cleanFixture({"--tab-background=blue",
                                    "--tab-foreground=red"}));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    parkPointer(d);

    const unsigned long ink = namedPixel(d, "red");
    REQUIRE(ink != ~0UL);

    Window client = None;
    const Window frame =
        mapClientAndAwaitFrame(d, 40, 40, 240, kTallWindowH, client, kShortTitle);
    REQUIRE(frame != None);
    settleWm(d);

    const TabObservation shortObs = observeTabFor(d, frame, client, ink, kShortTitle);
    const TabObservation longObs  = observeTabFor(d, frame, client, ink, kLongTitle);

    std::printf("[wm2 tablabel] short title %-4s tab length %4d px, label ink %5ld px\n"
                "[wm2 tablabel] long  title (%zu chars) tab length %4d px, label ink %5ld px\n",
                kShortTitle, shortObs.tabLength, shortObs.inkPixels,
                std::strlen(kLongTitle), longObs.tabLength, longObs.inkPixels);
    std::fflush(stdout);

    REQUIRE(shortObs.tabLength > 0);
    REQUIRE(longObs.tabLength > 0);

    // POSITIVE CONTROL. If a one-character title draws no ink either, the
    // mechanism is broken rather than the axis, and every assertion below would
    // pass or fail for the wrong reason.
    INFO("short-title label ink: " << shortObs.inkPixels << " px");
    REQUIRE(shortObs.inkPixels > 0);

    // 1. THE TAB TRACKS THE TITLE. Measured before the fix: 54 vs 58 px, a ratio
    //    of 1.07 for a 34-fold difference in title length. A correct
    //    implementation is bounded below by the rotated advance, which is an
    //    order of magnitude larger.
    INFO("tab length " << shortObs.tabLength << " -> " << longObs.tabLength);
    CHECK(longObs.tabLength > shortObs.tabLength * 2);

    // 2. THE LONG LABEL IS ACTUALLY ON THE SCREEN. This is the user-visible
    //    claim and the one the screenshots made: before the fix the long title
    //    rendered ZERO pixels of label because the draw origin was computed from
    //    the along-string advance and landed far outside the tab.
    INFO("label ink " << shortObs.inkPixels << " -> " << longObs.inkPixels);
    CHECK(longObs.inkPixels > shortObs.inkPixels);

    // 3. The tab is still a TAB and not the whole window: the shaped strip must
    //    stay within the frame it decorates. A "fix" that simply made the tab
    //    enormous would satisfy 1 and 2 and wreck the layout.
    Rect frameRect;
    REQUIRE(serverRect(d, frame, frameRect));
    INFO("frame " << describe(frameRect) << ", long tab length " << longObs.tabLength);
    CHECK(longObs.tabLength <= frameRect.h);

    // 4. No X protocol error other than the destroy-path BadWindow of deferred
    //    item 13. The tab's length feeds shapeTab()'s rectangle list, and 08-13
    //    found that geometry silently breaking the YXSorted promise at every
    //    non-default frame thickness -- rejected whole, logged, frame left
    //    unshaped, invisible to any assertion about windows or geometry.
    const std::string errs = fixture.wmStderr();
    INFO("WM stderr:\n" << errs);
    CHECK_FALSE(contains(errs, "BadMatch"));
    CHECK_FALSE(contains(errs, "BadValue"));
    CHECK_FALSE(contains(errs, "BadDrawable"));
    CHECK_FALSE(contains(errs, "RenderBadPicture"));
}

TEST_CASE("A long title on a short window is shortened to fit its tab, still legibly",
          "[wm_tablabel]")
{
    // THE PATH THIS CASE COVERS WAS DEAD CODE UNTIL PLAN 08-14.
    //
    // fixTabHeight()'s icon-name-then-ellipsis shortening loop trims the label
    // until the tab fits the window. Before the axis fix, m_tabHeight was
    // computed from the constant across-string thickness, so it came out around
    // 40 px whatever the title was and almost always landed under maxHeight on
    // the first try -- the loop was reachable in principle and essentially never
    // entered in practice. Now that the tab length tracks the title, a long
    // title on a short window enters it every time.
    //
    // Deferred item 11 flagged exactly this: "that loop is currently near-dead".
    // A fix that switches on a previously-unexercised loop without covering it
    // is a fix that has moved the risk rather than removed it.
    WmFixture fixture(cleanFixture({"--tab-background=blue",
                                    "--tab-foreground=red"}));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    parkPointer(d);

    const unsigned long ink = namedPixel(d, "red");
    REQUIRE(ink != ~0UL);

    // Short enough that the long title's full rotated advance (~286 px measured)
    // CANNOT fit, so the shortening path is forced rather than merely available.
    constexpr int kShortWindowH = 120;

    Window client = None;
    const Window frame =
        mapClientAndAwaitFrame(d, 40, 40, 240, kShortWindowH, client, kShortTitle);
    REQUIRE(frame != None);
    settleWm(d);

    // THE ICON NAME MUST ALSO BE LONG, or this case does not reach the loop it
    // exists to cover. fixTabHeight() has THREE rungs: measure the title, then
    // fall back to the icon name (or the literal "incognito" when there is
    // none), and only then enter the ellipsis loop. "incognito" is nine
    // characters and comfortably fits a 120 px window, so a case that sets only
    // a long TITLE returns at the second rung and the loop stays unexercised.
    //
    // MEASURED while writing this: with the icon name left unset, mutation M4 --
    // which corrupts the loop's own measurement -- stayed GREEN, because the
    // loop was never entered. Setting the icon name long is what turns this into
    // a real test of the third rung rather than a second test of the second one.
    XSetIconName(d, client, kLongTitle);
    XSync(d, False);
    settleWm(d);

    const TabObservation shortObs = observeTabFor(d, frame, client, ink, kShortTitle);
    const TabObservation longObs  = observeTabFor(d, frame, client, ink, kLongTitle);

    Rect frameRect;
    REQUIRE(serverRect(d, frame, frameRect));

    std::printf("[wm2 tablabel] SHORT WINDOW %d px, frame %d px\n"
                "[wm2 tablabel]   short title tab length %4d px, label ink %5ld px\n"
                "[wm2 tablabel]   long  title tab length %4d px, label ink %5ld px\n",
                kShortWindowH, frameRect.h,
                shortObs.tabLength, shortObs.inkPixels,
                longObs.tabLength, longObs.inkPixels);
    std::fflush(stdout);

    REQUIRE(shortObs.tabLength > 0);
    REQUIRE(longObs.tabLength > 0);
    REQUIRE(shortObs.inkPixels > 0);          // positive control, as above

    // 1. THE TAB STILL GREW to use the room it has. A shortening loop that gave
    //    up and left the stub tab would fail here -- which is exactly what the
    //    unfixed loop does, because it measures the wrong axis and concludes the
    //    label already fits after a single trim.
    //
    //    Measured against the ROOM, not as a ratio of the two tab lengths. The
    //    tab window is m_tabHeight + 2 + m_tabWidth tall and m_tabHeight itself
    //    carries another m_tabWidth, so a ratio of two lengths is really a ratio
    //    of two constants plus two titles, and it moves whenever the tab
    //    THICKNESS changes for reasons that have nothing to do with the title.
    //    MEASURED in quick task 260906-ldw, which widened the strip to clear the
    //    descenders: the tab grew perfectly well from 70 px to 115 px in a 129 px
    //    frame -- 89% of the room -- and `longObs.tabLength > shortObs.tabLength
    //    * 2` read 115 > 140 and called that a failure. Against the frame the
    //    same two readings are 89% and 54%, which is the claim this case makes
    //    and is stable across tab thicknesses.
    INFO("tab length " << shortObs.tabLength << " -> " << longObs.tabLength
         << " in a frame " << frameRect.h << " px tall");
    CHECK(longObs.tabLength > shortObs.tabLength);
    CHECK(longObs.tabLength * 10 >= frameRect.h * 8);
    CHECK(shortObs.tabLength * 10 < frameRect.h * 8);   // the stub tab does not

    // 2. AND IT DID NOT OVERFLOW THE WINDOW. This is the whole point of the
    //    loop: the tab must be bounded by the frame it decorates. Getting (1)
    //    without (2) would mean a tab hanging off the bottom of a short window.
    INFO("tab length " << longObs.tabLength << " vs frame height " << frameRect.h);
    CHECK(longObs.tabLength <= frameRect.h);

    // 3. The shortened label is still DRAWN. Trimming to nothing would satisfy
    //    (1) and (2) and leave the user with the blank tab this whole fix is
    //    about.
    INFO("label ink " << longObs.inkPixels << " px");
    CHECK(longObs.inkPixels > 0);

    // 4. The shortened geometry does not break the SHAPE promise. shapeTab()
    //    builds its rectangle list from m_tabHeight, and 08-13 found that list
    //    silently rejected whole -- frame left unshaped, logged, invisible to
    //    every assertion about windows and geometry. This loop is now the thing
    //    computing that height on a path nothing exercised before.
    const std::string errs = fixture.wmStderr();
    INFO("WM stderr:\n" << errs);
    CHECK_FALSE(contains(errs, "BadMatch"));
    CHECK_FALSE(contains(errs, "BadValue"));
    CHECK_FALSE(contains(errs, "BadDrawable"));
    CHECK_FALSE(contains(errs, "RenderBadPicture"));
}



// ===========================================================================
// [wm_tablabel] -- the label's clearance from the tab's two long edges
//
// Quick task 260906-ldw. The sideways tab reads bottom to top, so for the
// 90-degree rotated face the BASELINE side of every glyph faces the frame and
// the ascender side faces the outside of the window.
//
// MEASURED on this host, rotated face at the shipped pattern, size 12,
// XftTextExtentsUtf8 (ink spans [origin.x - x, origin.x - x + width)):
//
//     sample             width      x   above baseline   below baseline
//     "M"                   12     12               12                0
//     "g"                   12      9                9                3
//     "Mg"                  15     12               12                3
//     "gjpqy settings"      16     13               13                3
//     "MMMMM settings"      16     13               13                3
//     "Hello"               14     14               14                0
//     printable ASCII       18     14               14                4
//
// So `width` is the across-strip thickness, `x` is the distance from the draw
// origin to the ASCENDER edge, and `width - x` is the descender depth on the
// FRAME side. The tab was sized from "M" + 4 -- a sample with no descender at
// all -- and drawn at `2 + width of the label itself`, so (a) every descender
// ran into the frame line and (b) the baseline moved with the title.
//
// This case asserts the two clearances and the baseline's independence from the
// title. It is a PIXEL case because the defect is invisible to geometry: the
// tab window's rectangle is identical either way, only the ink inside it moves.
// ===========================================================================

namespace {

// Clearances the quick task asks for, in tab columns.
constexpr int kOuterClearance = 2;   // ascender side, away from the frame
constexpr int kFrameClearance = 5;   // baseline side, against the frame line

// A pair chosen so that the ONLY across-strip difference between them is the
// descenders. What fixes the ascender column is the TALLEST glyph in the string,
// and both of these top out on the dot of an "i" -- MEASURED at 13 px above the
// baseline for each, against 3 px below for the first and 0 for the second. A
// baseline that is a property of the FONT therefore puts their first ink on the
// same column; a baseline computed from the label's own extents, as it was, puts
// them 3 columns apart, because that is how much thicker the descender title is.
//
// "MMMMM settings" would NOT do as the second title, tempting as the symmetry
// is: it also ends in "settings", so its across-strip extent is identical to the
// first's and the old title-dependent baseline would land on the same column by
// coincidence -- the case would go green against the defect it exists to catch.
const char* const kDescenderTitle = "gjpqy settings";
const char* const kNoDescenderTitle = "static routines";

// How many pixels of a given colour each COLUMN across the tab strip carries.
// Read from root, at screen coordinates, for the reason captureRoot() gives:
// the tab is shaped and XGetImage outside a bounding shape is undefined.
std::vector<long> inkColumnCounts(Display* d, const Rect& r, unsigned long ink)
{
    std::vector<long> cols;
    if (r.w <= 0 || r.h <= 0) return cols;

    const int x = std::max(0, r.x);
    const int y = std::max(0, r.y);
    const int w = std::min(r.w, kScreenW - x);
    const int h = std::min(r.h, kScreenH - y);
    if (w <= 0 || h <= 0) return cols;

    XImage* img = XGetImage(d, DefaultRootWindow(d), x, y,
                            static_cast<unsigned>(w), static_cast<unsigned>(h),
                            AllPlanes, ZPixmap);
    if (!img) return cols;

    cols.assign(static_cast<std::size_t>(w), 0);
    for (int iy = 0; iy < h; ++iy) {
        for (int ix = 0; ix < w; ++ix) {
            if (XGetPixel(img, ix, iy) == ink) ++cols[static_cast<std::size_t>(ix)];
        }
    }
    XDestroyImage(img);
    return cols;
}

struct TabInk {
    int tabWidth  = -1;          // Border::m_tabWidth, derived from the client inset
    int tabHeight = -1;          // Border::m_tabHeight, derived from the tab window
    std::vector<long> columns;   // ink per column, index 0 == the OUTER edge
    int  firstInk = -1;          // ascender edge of the drawn label
    int  lastInk  = -1;          // baseline edge of the drawn label
    long total    = 0;
};

// Retitle -- as the cases above do, and for the same reason: two windows differ
// in stacking and in active state, and the active client is decorated
// differently, so a two-window comparison of the BASELINE would be confounded by
// something other than the title.
TabInk observeTabInk(Display* d, Window frame, Window client, unsigned long ink,
                     const char* title, int frameThickness)
{
    TabInk obs;

    XStoreName(d, client, title);
    XSync(d, False);
    settleWm(d);

    // Border::xIndent() == m_tabWidth + FRAME_WIDTH + 1, and the client sits at
    // that offset inside its frame. Derived from live geometry rather than
    // hardcoded: m_tabWidth is whatever the resolved face measures.
    const Rect frameRect  = rectOf(d, frame);
    const Rect clientRect = rectOf(d, client);
    obs.tabWidth = (clientRect.x - frameRect.x) - frameThickness - 1;
    if (obs.tabWidth <= 0) return obs;

    const Window tab = findFrameChild(d, frame, client, false);
    if (tab == None) return obs;

    Rect local;
    if (!localRect(d, tab, local)) return obs;
    // Border::configure() makes the tab window m_tabHeight + 2 + m_tabWidth tall.
    obs.tabHeight = local.h - 2 - obs.tabWidth;

    Rect abs;
    if (!serverRect(d, tab, abs)) return obs;

    // Restrict to the label's own run down the straight column: below the square
    // button that occupies the top of the tab, above the row where the shaped
    // diagonal foot begins (Border::drawBevel() names that row m_tabHeight).
    const int rowLo = obs.tabWidth + 2;
    const int rowHi = obs.tabHeight - 1;
    if (rowHi <= rowLo) return obs;

    Rect strip;
    strip.x = abs.x;
    strip.y = abs.y + rowLo;
    strip.w = obs.tabWidth;
    strip.h = rowHi - rowLo;

    obs.columns = inkColumnCounts(d, strip, ink);
    for (std::size_t i = 0; i < obs.columns.size(); ++i) {
        obs.total += obs.columns[i];
        if (obs.columns[i] > 0) {
            if (obs.firstInk < 0) obs.firstInk = static_cast<int>(i);
            obs.lastInk = static_cast<int>(i);
        }
    }
    return obs;
}

std::string describeColumns(const TabInk& obs)
{
    std::string s;
    for (std::size_t i = 0; i < obs.columns.size(); ++i) {
        s += std::to_string(i) + ":" + std::to_string(obs.columns[i]) + " ";
    }
    return s;
}

}  // namespace

TEST_CASE("Descenders in the tab label stop short of the frame, and the baseline "
          "does not move with the title",
          "[wm_tablabel]")
{
    // Explicit colours so the ink test is ABSOLUTE: the count is of the pixel
    // the server resolves the configured tab-foreground NAME to. Anti-aliased
    // fringe pixels are blends and are deliberately NOT counted -- what is
    // asserted is that no SOLID label pixel reaches either edge.
    WmFixture fixture(cleanFixture({"--tab-background=blue",
                                    "--tab-foreground=red"}));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    parkPointer(d);

    const unsigned long ink = namedPixel(d, "red");
    REQUIRE(ink != ~0UL);

    Window client = None;
    const Window frame = mapClientAndAwaitFrame(d, 40, 40, 240, kTallWindowH,
                                                client, kDescenderTitle);
    REQUIRE(frame != None);
    settleWm(d);

    const TabInk desc =
        observeTabInk(d, frame, client, ink, kDescenderTitle, kDefaultFrameThickness);
    const TabInk plain =
        observeTabInk(d, frame, client, ink, kNoDescenderTitle, kDefaultFrameThickness);

    std::printf("[wm2 tabclear] tab width %d px, tab height %d px\n"
                "[wm2 tabclear]   \"%s\"  ink %5ld px, columns %d..%d\n"
                "[wm2 tabclear]   \"%s\"  ink %5ld px, columns %d..%d\n",
                desc.tabWidth, desc.tabHeight,
                kDescenderTitle, desc.total, desc.firstInk, desc.lastInk,
                kNoDescenderTitle, plain.total, plain.firstInk, plain.lastInk);
    std::fflush(stdout);

    REQUIRE(desc.tabWidth > kOuterClearance + kFrameClearance);
    REQUIRE(desc.tabHeight > 0);
    REQUIRE(plain.tabWidth == desc.tabWidth);

    // (a) POSITIVE CONTROL. Every clearance assertion below is satisfied
    //     vacuously by a tab with no label on it at all, which is precisely the
    //     failure mode plan 08-14 found last time something moved on this axis.
    INFO("descender-title ink: " << desc.total << " px");
    REQUIRE(desc.total > 0);
    INFO("plain-title ink: " << plain.total << " px");
    REQUIRE(plain.total > 0);

    // (b) THE FRAME SIDE. The deepest descender must stop kFrameClearance
    //     columns short of the frame edge. Before this change the tab was sized
    //     from "M", which has no descender, and every descender ran to the tab's
    //     clipping edge -- the operator measured it on the 09-06 screenshot.
    INFO("columns (descender title): " << describeColumns(desc));
    INFO("last ink column " << desc.lastInk << " of tab width " << desc.tabWidth);
    CHECK(desc.lastInk <= desc.tabWidth - 1 - kFrameClearance);
    INFO("columns (plain title): " << describeColumns(plain));
    CHECK(plain.lastInk <= plain.tabWidth - 1 - kFrameClearance);

    // (c) THE OUTER SIDE. The ascender tops must stay kOuterClearance columns
    //     inside the tab's outer edge -- a "fix" that bought frame-side room by
    //     sliding the whole label outwards would fail here.
    INFO("first ink column " << desc.firstInk);
    CHECK(desc.firstInk >= kOuterClearance);
    INFO("first ink column (plain) " << plain.firstInk);
    CHECK(plain.firstInk >= kOuterClearance);

    // (d) THE BASELINE IS A PROPERTY OF THE FONT, NOT OF THE TITLE. Both titles
    //     end in "settings", so the tallest ascender in each is the same glyph;
    //     a baseline computed from the label's own extents -- as it was --
    //     shifts the whole label whenever the title's thickness changes.
    INFO("ascender column " << desc.firstInk << " vs " << plain.firstInk);
    CHECK(plain.firstInk == desc.firstInk);

    // (e) No protocol error. The tab width feeds shapeTab()'s rectangle list,
    //     which 08-13 found silently rejected whole at some thicknesses -- logged
    //     and invisible to every assertion about windows and geometry.
    const std::string errs = fixture.wmStderr();
    INFO("WM stderr:\n" << errs);
    CHECK_FALSE(contains(errs, "BadMatch"));
    CHECK_FALSE(contains(errs, "BadValue"));
    CHECK_FALSE(contains(errs, "BadDrawable"));
    CHECK_FALSE(contains(errs, "RenderBadPicture"));
}

// ===========================================================================
// [wm_tablabel] -- the title the WM reads is the one the client advertises
//
// Plan 08.5-01, D-8.5-02. Until that plan, Client::manage() read the window
// title with getProperty(XA_WM_NAME) and NOTHING anywhere read _NET_WM_NAME for
// a client: Atoms::net_wmName was interned and used solely to name the window
// manager's own check window (src/Manager.cpp).
//
// That is not a cosmetic gap. _NET_WM_NAME is UTF-8 by specification and
// WM_NAME has no reliable encoding, so the property the WM was ignoring is the
// only one that can carry most of the world's window titles. And once RULES-01
// matches on the title, reading the wrong property means a rule the user wrote
// correctly silently never fires -- the exact failure class Phase 8 spent
// fourteen plans removing from this codebase.
//
// The observable is the same one deferred item 11 established above: the
// sideways tab's LENGTH tracks the title, and the label's ink is on the screen.
// Nothing publishes a client's title back out of the WM, so the tab is the only
// place its reading of the title becomes visible from outside the process.
// ===========================================================================

namespace {

// Set _NET_WM_NAME as a real EWMH client would: UTF8_STRING, format 8.
void setNetWmName(Display* d, Window win, const char* utf8)
{
    Atom netWmName = XInternAtom(d, "_NET_WM_NAME", False);
    Atom utf8String = XInternAtom(d, "UTF8_STRING", False);
    XChangeProperty(d, win, netWmName, utf8String, 8, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(utf8),
                    static_cast<int>(std::strlen(utf8)));
    XSync(d, False);
}

void clearNetWmName(Display* d, Window win)
{
    XDeleteProperty(d, win, XInternAtom(d, "_NET_WM_NAME", False));
    XSync(d, False);
}

// The _NET_WM_NAME counterpart of observeTabFor(): retitle through the EWMH
// property instead of WM_NAME, then read back the same two numbers.
TabObservation observeTabForNetName(Display* d, Window frame, Window client,
                                    unsigned long inkPixel, const char* title)
{
    TabObservation obs;

    setNetWmName(d, client, title);
    settleWm(d);

    const Window tab = findFrameChild(d, frame, client, false);
    if (tab == None) return obs;

    Rect local;
    if (!localRect(d, tab, local)) return obs;
    obs.tabLength = local.h;

    Rect abs;
    if (!serverRect(d, tab, abs)) return obs;
    obs.inkPixels = countOf(captureRoot(d, abs), inkPixel);
    return obs;
}

// 35 Cyrillic characters -- the same character COUNT as kLongTitle, two bytes
// each in UTF-8, so 70 bytes. That pair of numbers is what makes the case
// discriminating: a reader that stops at the first byte with the high bit set
// sizes the tab for zero characters, and one that treats the bytes as Latin-1
// sizes it for seventy.
//
// CYRILLIC AND NOT CJK, DELIBERATELY, and the reason is worth recording because
// the first draft of this case used CJK and failed for a reason that had nothing
// to do with what it was testing. The WM resolves its tab font through
// fontconfig to a single face -- DejaVu Sans on this host -- and Xft draws
// NOTHING for a codepoint that face lacks. Noto's CJK fonts are installed here,
// but wm2 does no per-glyph font fallback, so a CJK title measures correctly
// (397 px, MEASURED) and renders zero pixels of label. That is a real gap in
// VISL-03's promise and it is recorded as deferred item 18; it is not this
// case's subject. DejaVu Sans covers Cyrillic, Greek and accented Latin, so
// those exercise the multi-byte path against glyphs that exist.
const char* const kLongUtf8Title =
    "ЗаголовокОкнаПроверкаДлинныйТексток";

}  // namespace

TEST_CASE("A title advertised only through _NET_WM_NAME reaches the sideways tab",
          "[wm_tablabel]")
{
    WmFixture fixture(cleanFixture({"--tab-background=blue",
                                    "--tab-foreground=red"}));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    parkPointer(d);

    const unsigned long ink = namedPixel(d, "red");
    REQUIRE(ink != ~0UL);

    // Mapped with NO title of any kind, so the WM falls back to its default
    // label. This is also behaviour 5 of the plan's task list: no title, no
    // icon name, no crash, a default label.
    Window client = None;
    const Window frame =
        mapClientAndAwaitFrame(d, 40, 40, 240, kTallWindowH, client, nullptr);
    REQUIRE(frame != None);
    settleWm(d);

    const Window tab0 = findFrameChild(d, frame, client, false);
    REQUIRE(tab0 != None);
    Rect untitled;
    REQUIRE(localRect(d, tab0, untitled));

    // Now advertise a long title through the EWMH property ONLY. WM_NAME is
    // never set on this window.
    const TabObservation netObs =
        observeTabForNetName(d, frame, client, ink, kLongTitle);

    std::printf("[wm2 tablabel] untitled tab length %4d px\n"
                "[wm2 tablabel] _NET_WM_NAME (%zu chars) tab length %4d px, ink %5ld px\n",
                untitled.h, std::strlen(kLongTitle),
                netObs.tabLength, netObs.inkPixels);
    std::fflush(stdout);

    REQUIRE(netObs.tabLength > 0);

    // 1. THE TAB GREW. A WM that reads only WM_NAME sees no title at all here
    //    and leaves the tab at its default-label length.
    INFO("untitled " << untitled.h << " -> _NET_WM_NAME " << netObs.tabLength);
    CHECK(netObs.tabLength > untitled.h * 2);

    // 2. AND THE LABEL IS ON THE SCREEN. Growing the tab without drawing into
    //    it would satisfy (1) and leave the user with a blank strip.
    INFO("label ink " << netObs.inkPixels << " px");
    CHECK(netObs.inkPixels > 0);

    const std::string errs = fixture.wmStderr();
    INFO("WM stderr:\n" << errs);
    CHECK_FALSE(contains(errs, "BadMatch"));
    CHECK_FALSE(contains(errs, "BadValue"));
    CHECK_FALSE(contains(errs, "BadDrawable"));
    CHECK_FALSE(contains(errs, "RenderBadPicture"));
}

TEST_CASE("_NET_WM_NAME wins over WM_NAME when a client sets both",
          "[wm_tablabel]")
{
    // The precedence is EWMH-then-ICCCM, and it is not arbitrary: a client that
    // sets both is almost always a toolkit publishing the real title in UTF-8
    // and a lossy transliteration in the legacy property for the benefit of
    // window managers from the 1990s. This one is from the 1990s and should
    // still prefer the good one.
    WmFixture fixture(cleanFixture({"--tab-background=blue",
                                    "--tab-foreground=red"}));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    parkPointer(d);

    const unsigned long ink = namedPixel(d, "red");
    REQUIRE(ink != ~0UL);

    // WM_NAME short, _NET_WM_NAME long. A WM reading the legacy property gets
    // the short one and produces a stub tab.
    Window client = None;
    const Window frame =
        mapClientAndAwaitFrame(d, 40, 40, 240, kTallWindowH, client, kShortTitle);
    REQUIRE(frame != None);
    settleWm(d);

    const TabObservation legacyOnly =
        observeTabFor(d, frame, client, ink, kShortTitle);

    const TabObservation bothSet =
        observeTabForNetName(d, frame, client, ink, kLongTitle);

    std::printf("[wm2 tablabel] WM_NAME=%s only          tab length %4d px\n"
                "[wm2 tablabel] + _NET_WM_NAME long      tab length %4d px, ink %5ld px\n",
                kShortTitle, legacyOnly.tabLength,
                bothSet.tabLength, bothSet.inkPixels);
    std::fflush(stdout);

    REQUIRE(legacyOnly.tabLength > 0);
    REQUIRE(bothSet.tabLength > 0);

    INFO("WM_NAME-only " << legacyOnly.tabLength
         << " -> both set " << bothSet.tabLength);
    CHECK(bothSet.tabLength > legacyOnly.tabLength * 2);
    CHECK(bothSet.inkPixels > legacyOnly.inkPixels);

    // And the reverse direction, which is the half a naive "prefer the EWMH
    // property" implementation gets wrong: DELETING _NET_WM_NAME must fall back
    // to WM_NAME rather than leaving the stale EWMH title on the tab.
    clearNetWmName(d, client);
    settleWm(d);

    const Window tab = findFrameChild(d, frame, client, false);
    REQUIRE(tab != None);
    Rect afterDelete;
    REQUIRE(localRect(d, tab, afterDelete));

    std::printf("[wm2 tablabel] _NET_WM_NAME deleted     tab length %4d px\n",
                afterDelete.h);
    std::fflush(stdout);

    INFO("after deleting _NET_WM_NAME: " << afterDelete.h
         << ", WM_NAME-only baseline was " << legacyOnly.tabLength);
    CHECK(afterDelete.h < bothSet.tabLength);
}

TEST_CASE("A WM_NAME-only client is unaffected by the EWMH title read",
          "[wm_tablabel]")
{
    // THE REGRESSION GUARD. This case passed before plan 08.5-01 and must pass
    // after it: preferring _NET_WM_NAME must not disturb the large population of
    // clients -- every plain Xlib program, every one of this suite's own test
    // clients -- that set only the legacy property.
    WmFixture fixture(cleanFixture({"--tab-background=blue",
                                    "--tab-foreground=red"}));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    parkPointer(d);

    const unsigned long ink = namedPixel(d, "red");
    REQUIRE(ink != ~0UL);

    Window client = None;
    const Window frame =
        mapClientAndAwaitFrame(d, 40, 40, 240, kTallWindowH, client, kShortTitle);
    REQUIRE(frame != None);
    settleWm(d);

    const TabObservation shortObs = observeTabFor(d, frame, client, ink, kShortTitle);
    const TabObservation longObs  = observeTabFor(d, frame, client, ink, kLongTitle);

    REQUIRE(shortObs.tabLength > 0);
    REQUIRE(longObs.tabLength > 0);
    REQUIRE(shortObs.inkPixels > 0);

    INFO("WM_NAME tab length " << shortObs.tabLength << " -> " << longObs.tabLength);
    CHECK(longObs.tabLength > shortObs.tabLength * 2);
    CHECK(longObs.inkPixels > shortObs.inkPixels);
}

TEST_CASE("A UTF-8 title is not truncated at its first multi-byte character",
          "[wm_tablabel]")
{
    // WHAT THIS PROVES AND WHAT IT DOES NOT. Nothing publishes a client's title
    // back out of the window manager, so this cannot assert that the glyphs are
    // the RIGHT glyphs -- mojibake and correct rendering both make ink. What it
    // can prove, and what the failure mode actually looks like, is LENGTH: a
    // reader that stops at the first byte with the high bit set produces a tab
    // sized for zero characters, and one that treats UTF-8 as Latin-1 produces a
    // tab sized for three times too many. Both are caught by bounding the result
    // against the same character count in ASCII.
    WmFixture fixture(cleanFixture({"--tab-background=blue",
                                    "--tab-foreground=red"}));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    parkPointer(d);

    const unsigned long ink = namedPixel(d, "red");
    REQUIRE(ink != ~0UL);

    Window client = None;
    const Window frame =
        mapClientAndAwaitFrame(d, 40, 40, 240, kTallWindowH, client, nullptr);
    REQUIRE(frame != None);
    settleWm(d);

    // THE UNTITLED BASELINE IS NOT OPTIONAL. Both observations below set only
    // _NET_WM_NAME, so on a WM that ignores that property entirely they come
    // back IDENTICAL -- and every relative assertion in this case then holds
    // trivially on two default-label tabs. Measured: this case passed vacuously
    // against the unfixed binary until this baseline was added.
    const Window tab0 = findFrameChild(d, frame, client, false);
    REQUIRE(tab0 != None);
    Rect untitled;
    REQUIRE(localRect(d, tab0, untitled));

    const TabObservation asciiObs =
        observeTabForNetName(d, frame, client, ink, kLongTitle);
    const TabObservation utf8Obs =
        observeTabForNetName(d, frame, client, ink, kLongUtf8Title);

    // 0. VACUITY GUARD. Both titles must have actually reached the tab.
    INFO("untitled baseline " << untitled.h
         << ", ascii " << asciiObs.tabLength
         << ", utf8 " << utf8Obs.tabLength);
    REQUIRE(asciiObs.tabLength > untitled.h * 2);
    REQUIRE(utf8Obs.tabLength > untitled.h * 2);

    std::printf("[wm2 tablabel] ASCII    (%zu chars, %zu bytes) tab %4d px, ink %5ld px\n"
                "[wm2 tablabel] Cyrillic (35 chars, %zu bytes) tab %4d px, ink %5ld px\n",
                std::strlen(kLongTitle), std::strlen(kLongTitle),
                asciiObs.tabLength, asciiObs.inkPixels,
                std::strlen(kLongUtf8Title),
                utf8Obs.tabLength, utf8Obs.inkPixels);
    std::fflush(stdout);

    REQUIRE(asciiObs.tabLength > 0);
    REQUIRE(utf8Obs.tabLength > 0);

    // 1. NOT TRUNCATED. A stub tab is what a byte-at-a-time reader that stops on
    //    the first high byte produces.
    INFO("utf8 tab " << utf8Obs.tabLength << " vs ascii tab " << asciiObs.tabLength);
    CHECK(utf8Obs.tabLength > asciiObs.tabLength / 2);

    // 2. AND ACTUALLY DRAWN. This is the assertion that caught the CJK
    //    missing-glyph case: the tab was sized correctly and the label was
    //    blank, which every length-based assertion happily accepted.
    INFO("utf8 label ink " << utf8Obs.inkPixels << " px");
    CHECK(utf8Obs.inkPixels > 0);

    // 3. NOT DOUBLED. Treating each two-byte sequence as two Latin-1 characters
    //    would size the tab for seventy characters rather than thirty-five.
    INFO("utf8 tab " << utf8Obs.tabLength << " vs 1.5x ascii "
         << (asciiObs.tabLength * 3 / 2));
    CHECK(utf8Obs.tabLength < asciiObs.tabLength * 3 / 2);

    const std::string errs = fixture.wmStderr();
    INFO("WM stderr:\n" << errs);
    CHECK_FALSE(contains(errs, "BadMatch"));
    CHECK_FALSE(contains(errs, "BadValue"));
    CHECK_FALSE(contains(errs, "RenderBadPicture"));
}


// ===========================================================================
// [wm_menulabel] -- the root menu highlight must not erase the row's label
//
// Found by the operator's manual XRDP pass and reproduced on plain Xvfb, so it
// is not remote-desktop-specific. Both of menu()'s drawing paths fill a row
// rectangle and never redraw the text inside it:
//
//   MotionNotify -- fills the PREVIOUS row with the background colour (erasing
//                   its label) and the NEW row with the highlight colour
//                   (painting over its label). Neither redraws the label.
//   Expose       -- draws every label in a loop and THEN fills the selected row
//                   on top of the text it has just drawn.
//
// So the row under the pointer goes blank, and the row you just left stays
// blank. openCategorySubmenu() has the identical pair of bugs.
//
// GEOMETRY IS DISCOVERED, NOT ASSUMED. The menu's row height depends on the
// font and its row COUNT depends on how many applications the host has
// installed (08-13's flake note), so this case never computes a row rectangle.
// It finds the highlight fill by its COLOUR -- the bounding box of the
// configured highlight pixel -- and counts foreground ink strictly inside that
// box. The box excludes the menu's 1 px border verticals by construction,
// which matters: the border is drawn in the FOREGROUND colour and would
// otherwise contribute a constant ~40 px of "ink" to every measurement and mask
// the very loss this case exists to detect.
//
// EVERY CAPTURE HAPPENS WHILE BUTTON1 IS HELD. menu() runs a nested event loop
// under a pointer grab and unmaps the window on release, so a capture taken
// after the release would find nothing at all. The root menu is Button1, not
// Button3.
// ===========================================================================

namespace {

// A raw pixel grid, because this case needs pixel POSITIONS and not just a
// histogram: it locates the highlight band before it can count ink inside it.
struct Bitmap {
    int w = 0, h = 0;
    std::vector<unsigned long> px;
    bool valid() const { return w > 0 && h > 0 && !px.empty(); }
    unsigned long at(int x, int y) const {
        return px[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
    }
};

// Read through ROOT for the same reason captureRoot() does: menu windows are
// plain rectangles, but reading root keeps one convention for every pixel
// assertion in this file.
Bitmap captureRootBitmap(Display* d, const Rect& r)
{
    Bitmap b;
    if (r.w <= 0 || r.h <= 0) return b;

    const int x = std::max(0, r.x);
    const int y = std::max(0, r.y);
    const int w = std::min(r.w, kScreenW - x);
    const int h = std::min(r.h, kScreenH - y);
    if (w <= 0 || h <= 0) return b;

    XImage* img = XGetImage(d, DefaultRootWindow(d), x, y,
                            static_cast<unsigned>(w), static_cast<unsigned>(h),
                            AllPlanes, ZPixmap);
    if (!img) return b;

    b.w = w;
    b.h = h;
    b.px.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
    for (int iy = 0; iy < h; ++iy) {
        for (int ix = 0; ix < w; ++ix) {
            b.px[static_cast<size_t>(iy) * static_cast<size_t>(w) +
                 static_cast<size_t>(ix)] = XGetPixel(img, ix, iy);
        }
    }
    XDestroyImage(img);
    return b;
}

// Bounding box of every pixel equal to `pixel`. Returns false when there are
// none, which is how "no row is highlighted" is observed.
bool pixelBounds(const Bitmap& b, unsigned long pixel,
                 int& x0, int& y0, int& x1, int& y1)
{
    x0 = b.w; y0 = b.h; x1 = -1; y1 = -1;
    for (int y = 0; y < b.h; ++y) {
        for (int x = 0; x < b.w; ++x) {
            if (b.at(x, y) != pixel) continue;
            if (x < x0) x0 = x;
            if (y < y0) y0 = y;
            if (x > x1) x1 = x;
            if (y > y1) y1 = y;
        }
    }
    return x1 >= 0 && y1 >= 0;
}

long countInBox(const Bitmap& b, unsigned long pixel,
                int x0, int y0, int x1, int y1)
{
    long count = 0;
    for (int y = std::max(0, y0); y <= std::min(b.h - 1, y1); ++y) {
        for (int x = std::max(0, x0); x <= std::min(b.w - 1, x1); ++x) {
            if (b.at(x, y) == pixel) ++count;
        }
    }
    return count;
}

long countAll(const Bitmap& b, unsigned long pixel)
{
    return countInBox(b, pixel, 0, 0, b.w - 1, b.h - 1);
}

}  // namespace


// Move the pointer to (cx,cy) REPEATEDLY until `pred` holds.
//
// A single moveTo() is not enough and the reason is a real property of the code
// under test, not a timing guess. menu()'s MotionNotify handler opens with
//
//     if (!drawn) break;
//
// so a motion that arrives before the Expose handler has run is DISCARDED
// OUTRIGHT, and nothing ever replays it -- the pointer is already where we put
// it, so no further motion is generated and the highlight never appears.
// openRootMenu() waits for the menu to be "drawn", but it infers that from the
// pixels on screen, which can show a second colour slightly before the WM has
// finished its Expose handler.
//
// MEASURED: 2 failures in 20 ASan runs, both this exact wait timing out at 20 s,
// and 0 in the debug tree -- the sanitizer widens the map-to-Expose window. This
// is the same family as 08-13's three flake fixes and as deferred item 9.
//
// The nudge alternates x by one pixel so every iteration is a genuine position
// CHANGE (XTEST emits nothing for a move to where the pointer already is), and
// stays within the same row so it cannot select a different entry.
bool nudgeUntil(XTestDriver& driver, int cx, int cy,
                const std::function<bool()>& pred, int timeoutMs = 20000)
{
    int toggle = 0;
    return WmFixture::pollUntil([&] {
        driver.moveTo(cx + (toggle++ % 2), cy);
        return pred();
    }, timeoutMs);
}


// openRootMenu(), but VERIFIED and retried.
//
// The WM's own menu window is sometimes already invalid by the time menu() runs:
// XMoveResizeWindow, XMapRaised and XUnmapWindow all come back BadWindow for it
// and no menu ever appears. MEASURED on the ASan tree, 1 run in 10, with the
// WM's stderr showing exactly that request triple against its own m_menuWindow
// id. It is a PRE-EXISTING defect -- see deferred item 17 -- and user-visible:
// the root menu simply does not open.
//
// This wrapper insists the window it returns is viewable AND contains the press
// point, and retries the whole press/release cycle if not.
//
// It was written to close a hole findOpenMenu() had: a silent fallback to "the
// first viewable child", which handed the caller some unrelated window whenever
// the menu was missing. 08.5-13 removed that fallback at its source, so the
// hole is gone and these checks are now belt and braces rather than the only
// defence. They stay: the retry is what answers deferred item 17, in which the
// menu genuinely never opens, and that is a different failure from the one the
// fallback caused.
//
// Retrying rather than waiting longer is 08-06's remedy for deferred item 10,
// for the same reason: no amount of extra waiting fixes an interaction that
// never started.
bool openRootMenuVerified(Display* d, XTestDriver& driver, int x, int y,
                          Window& menuOut, Rect& rectOut, std::string& whyOut,
                          unsigned long expectedBg)
{
    // Five, not three: the only thing that fails these cases is deferred
    // item 17, whose rate this directly divides down.
    constexpr int kAttempts = 5;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        if (openRootMenu(d, driver, x, y, menuOut, rectOut, expectedBg) &&
            menuOut != None && isViewable(d, menuOut) &&
            x >= rectOut.x && x < rectOut.x + rectOut.w &&
            y >= rectOut.y && y < rectOut.y + rectOut.h) {
            return true;
        }
        whyOut = "attempt " + std::to_string(attempt + 1) +
                 ": menu=" + std::to_string(menuOut) +
                 " rect=" + describe(rectOut) +
                 " viewable=" + std::to_string(menuOut != None && isViewable(d, menuOut));
        // Release the press this attempt is still holding before trying again,
        // clear of the menu so nothing is selected.
        driver.moveTo(kScreenW - 5, kScreenH - 5);
        driver.release(Button1);
        XSync(d, False);
        settleWm(d);
    }
    return false;
}

TEST_CASE("Highlighting a root-menu row does not erase its label, and neither "
          "does leaving it", "[wm_menulabel]")
{
    WmFixture fixture(cleanFixture({"--menu-background=blue",
                                    "--menu-foreground=red",
                                    "--menu-highlight=green"}));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    XTestDriver driver(fixture.display());   // throws if XTEST is unavailable
    parkPointer(d);

    const unsigned long fg = namedPixel(d, "red");
    const unsigned long hl = namedPixel(d, "green");
    REQUIRE(fg != ~0UL);
    REQUIRE(hl != ~0UL);
    REQUIRE(fg != hl);

    Window menu = None;
    Rect menuRect;
    std::string why;
    REQUIRE(openRootMenuVerified(d, driver, kMenuPressX, kMenuPressY,
                                 menu, menuRect, why, namedPixel(d, "blue")));
    // INFO placed AFTER the call, not before it. Catch2 evaluates the
    // streamed expression where the INFO stands, so reading `why` above the
    // call that fills it captured the empty string it still held -- every
    // failure downstream printed a diagnostics line with nothing after the
    // colon, which is exactly when that line was wanted.
    INFO("menu open diagnostics: " << why);

    // --- A: nothing hovered. selecting is -1 until the first MotionNotify, so
    //     the initial Expose draws every label and highlights nothing.
    const Bitmap before = captureRootBitmap(d, menuRect);
    REQUIRE(before.valid());
    REQUIRE(countAll(before, hl) == 0);          // control: no highlight yet

    // --- B: hover row 0. Row 0 is always the "New" entry (menuLabelFn), never a
    //     category row -- deliberately, because hovering a CATEGORY row opens a
    //     submenu (D-03/D-04) and this case would then be measuring the wrong
    //     window. The y offset is selectFirstMenuEntry()'s, which lands in row 0
    //     whatever the font's entry height is.
    REQUIRE(nudgeUntil(driver, menuRect.x + menuRect.w / 2, menuRect.y + 14, [&] {
        return countAll(captureRootBitmap(d, menuRect), hl) > 0;
    }));
    const Bitmap hovered = captureRootBitmap(d, menuRect);
    REQUIRE(hovered.valid());

    // The highlight fill IS the row rectangle. Discovering it by colour avoids
    // recomputing entryHeight, which depends on the font.
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    REQUIRE(pixelBounds(hovered, hl, x0, y0, x1, y1));

    // --- B2: RE-EXPOSE the menu while row 0 is still selected.
    //
    // menu()'s Expose handler has its own ordering of the same two operations,
    // and it is only wrong when a row IS selected -- the FIRST exposure always
    // happens with selecting == -1, so the initial draw looks correct however
    // the handler is ordered. Without this step the Expose ordering is an
    // uncovered branch: measured, the mutation that reverts it stayed green.
    //
    // Damage is inflicted the way a real desktop inflicts it -- another window
    // briefly covering the menu. The cover is override-redirect so the WM
    // ignores it entirely (eventCreate returns immediately) and it cannot be
    // framed or managed.
    {
        XSetWindowAttributes at;
        at.override_redirect = True;
        Window cover = XCreateSimpleWindow(d, DefaultRootWindow(d),
                                           menuRect.x, menuRect.y,
                                           static_cast<unsigned>(menuRect.w),
                                           static_cast<unsigned>(menuRect.h),
                                           0, BlackPixel(d, DefaultScreen(d)),
                                           BlackPixel(d, DefaultScreen(d)));
        XChangeWindowAttributes(d, cover, CWOverrideRedirect, &at);
        XMapRaised(d, cover);
        XSync(d, False);
        // The menu really is hidden before we uncover it, or the Expose we are
        // waiting for may never be generated at all.
        REQUIRE(WmFixture::pollUntil([&] {
            return countAll(captureRootBitmap(d, menuRect), hl) == 0;
        }, 20000));
        XDestroyWindow(d, cover);
        XSync(d, False);
    }
    // The WM has repainted when the highlight is back on screen.
    REQUIRE(WmFixture::pollUntil([&] {
        return countAll(captureRootBitmap(d, menuRect), hl) > 0;
    }, 20000));
    const Bitmap reExposed = captureRootBitmap(d, menuRect);
    REQUIRE(reExposed.valid());

    // --- C: leave the row without entering another. Same y, far off to the
    //     right: menu() reads menu-relative coordinates (owner_events=False), so
    //     x > maxWidth sets selecting = -1 and takes the UNHIGHLIGHT branch for
    //     row 0. Moving along the same row cannot cross a category row, so no
    //     submenu can open behind our backs. XTEST delivers one motion event at
    //     the destination rather than a swept path, so there are no intermediate
    //     rows either.
    REQUIRE(nudgeUntil(driver, kScreenW - 6, menuRect.y + 14, [&] {
        return countAll(captureRootBitmap(d, menuRect), hl) == 0;
    }));
    const Bitmap left = captureRootBitmap(d, menuRect);
    REQUIRE(left.valid());

    const long inkNormal      = countInBox(before,    fg, x0, y0, x1, y1);
    const long inkHighlighted = countInBox(hovered,   fg, x0, y0, x1, y1);
    const long inkReExposed   = countInBox(reExposed, fg, x0, y0, x1, y1);
    const long inkAfterLeave  = countInBox(left,      fg, x0, y0, x1, y1);

    std::printf("[wm2 menulabel] menu %s, highlight box (%d,%d)-(%d,%d)\n"
                "[wm2 menulabel]   row 0 label ink, not hovered  %4ld px\n"
                "[wm2 menulabel]   row 0 label ink, HOVERED      %4ld px\n"
                "[wm2 menulabel]   row 0 label ink, RE-EXPOSED   %4ld px\n"
                "[wm2 menulabel]   row 0 label ink, after LEAVE  %4ld px\n",
                describe(menuRect).c_str(), x0, y0, x1, y1,
                inkNormal, inkHighlighted, inkReExposed, inkAfterLeave);
    std::fflush(stdout);

    // Release before asserting: menu() holds a pointer grab in a nested loop,
    // and a failed REQUIRE would otherwise leave the whole display grabbed and
    // take every following case with it.
    dismissMenu(d, driver);

    // POSITIVE CONTROL. If row 0 draws no ink even unhovered, the colours or the
    // capture are wrong and every assertion below would pass or fail for a
    // reason that has nothing to do with the defect.
    INFO("row 0 ink when not hovered: " << inkNormal << " px");
    REQUIRE(inkNormal > 0);

    // 1. THE HOVERED ROW KEEPS ITS LABEL. Before the fix the highlight fill is
    //    painted over the text and nothing redraws it: measured ZERO.
    INFO("ink " << inkNormal << " -> " << inkHighlighted << " while hovered");
    CHECK(inkHighlighted > 0);
    CHECK(inkHighlighted * 2 >= inkNormal);

    // 2. AND IT SURVIVES A REPAINT. The Expose handler draws the whole menu
    //    from scratch, and it is the one path where the ordering of "fill the
    //    selected row" against "draw the labels" is only observable when a row
    //    is actually selected.
    INFO("ink " << inkNormal << " -> " << inkReExposed << " after re-exposure");
    CHECK(inkReExposed > 0);
    CHECK(inkReExposed * 2 >= inkNormal);

    // 3. AND SO DOES THE ROW YOU JUST LEFT. This is the second half of the same
    //    defect and it needs its own assertion: a fix that redrew the label only
    //    on the highlight branch would satisfy (1) and still leave a blank row
    //    behind the pointer.
    INFO("ink " << inkNormal << " -> " << inkAfterLeave << " after leaving");
    CHECK(inkAfterLeave > 0);
    CHECK(inkAfterLeave * 2 >= inkNormal);

    CHECK(fixture.wmAlive());
}

TEST_CASE("Highlighting a category submenu row does not erase its label either",
          "[wm_menulabel]")
{
    // openCategorySubmenu() carried the IDENTICAL pair of defects as menu() and
    // was fixed the same way. Without this case both submenu fixes are uncovered
    // branches -- and a symmetric fix applied to two places is exactly the shape
    // of change where one of the two silently gets missed.
    //
    // The submenu is only reachable if the host has applications to categorise,
    // and 08-13 warned that menu content is host-dependent. That dependency is
    // handled by PROVING the alternative rather than skipping: if no submenu
    // opens, the menu must have had exactly one row, which is the only state in
    // which there is no category to hover.
    WmFixture fixture(cleanFixture({"--menu-background=blue",
                                    "--menu-foreground=red",
                                    "--menu-highlight=green"}));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    XTestDriver driver(fixture.display());
    parkPointer(d);

    const unsigned long fg = namedPixel(d, "red");
    const unsigned long hl = namedPixel(d, "green");
    REQUIRE(fg != ~0UL);
    REQUIRE(hl != ~0UL);

    Window menu = None;
    Rect menuRect;
    std::string why;
    REQUIRE(openRootMenuVerified(d, driver, kMenuPressX, kMenuPressY,
                                 menu, menuRect, why, namedPixel(d, "blue")));
    INFO("menu open diagnostics: " << why);

    // Row height is MEASURED, not computed: hover row 0 and read back the
    // height of the highlight fill. entryHeight depends on the menu font.
    const bool gotHl = nudgeUntil(driver, menuRect.x + menuRect.w / 2,
                                  menuRect.y + 14, [&] {
        return countAll(captureRootBitmap(d, menuRect), hl) > 0;
    });
    INFO("DIAG menu=" << menu << " rect=" << describe(menuRect)
         << " viewable=" << isViewable(d, menu)
         << " nowRect=" << describe(rectOf(d, menu))
         << " distinctPixels=" << captureRoot(d, menuRect).size()
         << " rootChildren=" << childrenOf(d, DefaultRootWindow(d)).size());
    INFO("DIAG wm stderr:\n" << fixture.wmStderr());
    REQUIRE(gotHl);

    int hx0 = 0, hy0 = 0, hx1 = 0, hy1 = 0;
    REQUIRE(pixelBounds(captureRootBitmap(d, menuRect), hl, hx0, hy0, hx1, hy1));
    const int entryHeight = hy1 - hy0 + 1;
    REQUIRE(entryHeight > 0);

    const int rowCount = (menuRect.h - 13) / entryHeight;
    INFO("menu " << describe(menuRect) << ", entryHeight " << entryHeight
         << ", rows " << rowCount);

    // With no hidden clients, row 0 is "New" and row 1 is the first CATEGORY
    // row -- hovering it opens the submenu (D-03/D-04), which is the behaviour
    // this case needs and which the operator has decided to leave as designed.
    const std::vector<Window> beforeChildren = childrenOf(d, DefaultRootWindow(d));

    Window submenu = None;
    // Nudged for the same reason as every other hover here: a motion that beats
    // the Expose handler is dropped and never replayed.
    const bool opened = nudgeUntil(driver, menuRect.x + menuRect.w / 2,
                                   menuRect.y + 14 + entryHeight, [&] {
        for (Window w : childrenOf(d, DefaultRootWindow(d))) {
            if (w == menu) continue;
            if (std::find(beforeChildren.begin(), beforeChildren.end(), w) !=
                beforeChildren.end()) {
                // Present before too -- only interesting if it has just become
                // viewable, which is how the reused m_submenuWindow appears.
                Rect r;
                if (!isViewable(d, w) || !serverRect(d, w, r)) continue;
                if (r.w <= 1 || r.h <= 1) continue;
                if (w == menu) continue;
                submenu = w;
                return true;
            }
        }
        return false;
    });

    if (!opened) {
        // PROVE the alternative rather than skipping: the only state with no
        // category to hover is a one-row menu.
        dismissMenu(d, driver);
        INFO("no submenu opened; menu row count was " << rowCount);
        REQUIRE(rowCount <= 1);
        WARN("host has no application categories -- submenu path not exercised");
        return;
    }

    Rect subRect;
    REQUIRE(serverRect(d, submenu, subRect));
    REQUIRE(WmFixture::pollUntil([&] {
        return captureRootBitmap(d, subRect).valid() &&
               captureRoot(d, subRect).size() >= 2;         // drawn, not just mapped
    }, 20000));

    const Bitmap subBefore = captureRootBitmap(d, subRect);
    REQUIRE(subBefore.valid());

    // Hover the submenu's first row.
    const bool subHighlighted =
        nudgeUntil(driver, subRect.x + subRect.w / 2, subRect.y + 14, [&] {
            return countAll(captureRootBitmap(d, subRect), hl) > 0;
        });

    Bitmap subHovered;
    int sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;
    long subInkNormal = 0, subInkHighlighted = 0;
    if (subHighlighted) {
        subHovered = captureRootBitmap(d, subRect);
        if (pixelBounds(subHovered, hl, sx0, sy0, sx1, sy1)) {
            subInkNormal      = countInBox(subBefore,  fg, sx0, sy0, sx1, sy1);
            subInkHighlighted = countInBox(subHovered, fg, sx0, sy0, sx1, sy1);
        }
    }

    // Leave the submenu row along the SAME row, so prev2 is a valid index and
    // openCategorySubmenu()'s UNHIGHLIGHT branch runs. Without this the submenu
    // case only ever enters one row and never leaves it, so that branch is
    // uncovered -- measured, the mutation that removes its redraw stayed green.
    // Coordinates are submenu-relative (owner_events=False), so a large x is
    // outside the submenu and sets selecting2 = -1.
    long subInkAfterLeave = 0;
    bool subLeft = false;
    if (subHighlighted) {
        subLeft = nudgeUntil(driver, kScreenW - 6, subRect.y + 14, [&] {
            return countAll(captureRootBitmap(d, subRect), hl) == 0;
        });
        if (subLeft) {
            subInkAfterLeave = countInBox(captureRootBitmap(d, subRect), fg,
                                          sx0, sy0, sx1, sy1);
        }
    }

    std::printf("[wm2 menulabel] submenu %s\n"
                "[wm2 menulabel]   row 0 label ink, not hovered  %4ld px\n"
                "[wm2 menulabel]   row 0 label ink, HOVERED      %4ld px\n"
                "[wm2 menulabel]   row 0 label ink, after LEAVE  %4ld px\n",
                describe(subRect).c_str(),
                subInkNormal, subInkHighlighted, subInkAfterLeave);
    std::fflush(stdout);

    // Release well clear of both popups, so no application is launched: a
    // release over a submenu row RUNS that entry.
    driver.moveTo(kScreenW - 5, kScreenH - 5);
    driver.release(Button1);
    XSync(d, False);
    settleWm(d);

    REQUIRE(subHighlighted);
    INFO("submenu ink " << subInkNormal << " -> " << subInkHighlighted
         << " -> " << subInkAfterLeave);
    REQUIRE(subInkNormal > 0);                    // positive control
    CHECK(subInkHighlighted > 0);
    CHECK(subInkHighlighted * 2 >= subInkNormal);

    REQUIRE(subLeft);
    CHECK(subInkAfterLeave > 0);
    CHECK(subInkAfterLeave * 2 >= subInkNormal);

    CHECK(fixture.wmAlive());
}


// ===========================================================================
// [wm_menureopen] -- the root menu still works after a submenu episode
//
// Guards the ONE-GRAB, ONE-LOOP invariant that menu() now holds.
//
// The reported defect this pins: "once a submenu has been activated, the main
// menu cannot be re-activated: it remains static even if the submenu has been
// abandoned and the cursor is moving over main menu items." Found on a real
// XRDP session, reproduced on plain Xvfb, so it was never remote-specific.
//
// The cause was structural, not a leak. openCategorySubmenu() ungrabbed the
// outer menu, took a SECOND grab on the submenu window with owner_events=False,
// and ran a SECOND nested event loop. From that point every pointer event
// belonged to the submenu: moving back over the outer menu delivered motion to
// the submenu's loop, which resolved it against its own rectangle, found the
// pointer outside, and highlighted nothing -- while the outer menu kept its last
// highlight frozen, because no loop was reading for it any more. There was no
// way back; releasing was the only exit and it closed both popups.
//
// menu() now owns the whole interaction: one XGrabPointer, one XUngrabPointer,
// one loop, and the submenu is a state of that loop hit-tested in root
// coordinates. openCategorySubmenu() no longer exists.
//
// This case asserts the two things that must both hold afterwards, because
// either alone is satisfiable by a broken WM:
//
//  1. The pointer is genuinely UNGRABBED after a submenu episode -- probed
//     directly from this connection, so a leak is reported as AlreadyGrabbed
//     rather than being misdiagnosed later as "the menu did not respond".
//  2. The reopened menu TRACKS the pointer. Opening is not enough: a menu whose
//     loop never receives MotionNotify still maps.
// ===========================================================================

TEST_CASE("The root menu still opens and tracks the pointer after a submenu episode",
          "[wm_menureopen]")
{
    WmFixture fixture(cleanFixture({"--menu-background=blue",
                                    "--menu-foreground=red",
                                    "--menu-highlight=green"}));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    XTestDriver driver(fixture.display());
    parkPointer(d);

    const unsigned long hl = namedPixel(d, "green");
    REQUIRE(hl != ~0UL);

    // --- Episode 1: open the menu, hover a category so the submenu takes the
    //     grab, abandon it, and release.
    Window menu = None;
    Rect menuRect;
    std::string why;
    REQUIRE(openRootMenuVerified(d, driver, kMenuPressX, kMenuPressY,
                                 menu, menuRect, why, namedPixel(d, "blue")));
    INFO("first menu open: " << why);

    REQUIRE(nudgeUntil(driver, menuRect.x + menuRect.w / 2, menuRect.y + 14, [&] {
        return countAll(captureRootBitmap(d, menuRect), hl) > 0;
    }));
    int hx0 = 0, hy0 = 0, hx1 = 0, hy1 = 0;
    REQUIRE(pixelBounds(captureRootBitmap(d, menuRect), hl, hx0, hy0, hx1, hy1));
    const int entryHeight = hy1 - hy0 + 1;
    REQUIRE(entryHeight > 0);
    const int rowCount = (menuRect.h - 13) / entryHeight;

    const std::vector<Window> before = childrenOf(d, DefaultRootWindow(d));
    Window submenu = None;
    const bool opened = nudgeUntil(driver, menuRect.x + menuRect.w / 2,
                                   menuRect.y + 14 + entryHeight, [&] {
        for (Window w : childrenOf(d, DefaultRootWindow(d))) {
            if (w == menu) continue;
            Rect r;
            if (!isViewable(d, w) || !serverRect(d, w, r)) continue;
            if (r.w <= 1 || r.h <= 1) continue;
            if (std::find(before.begin(), before.end(), w) == before.end() ||
                r.x != menuRect.x) {
                submenu = w;
                return true;
            }
        }
        return false;
    });

    if (!opened) {
        dismissMenu(d, driver);
        INFO("no submenu opened; row count was " << rowCount);
        REQUIRE(rowCount <= 1);
        WARN("host has no application categories -- submenu episode not exercised");
        return;
    }

    // Abandon the submenu and release, which is the operator's exact sequence.
    driver.moveTo(kScreenW - 5, kScreenH - 5);
    driver.release(Button1);
    XSync(d, False);
    settleWm(d);

    // --- The direct measurement of the hypothesis: is the pointer still
    //     grabbed? A leaked grab makes THIS connection's XGrabPointer return
    //     AlreadyGrabbed. Asserted before the second menu is attempted, so a
    //     leak is reported as a leak rather than as "the menu did not respond".
    const int grabStatus = XGrabPointer(d, DefaultRootWindow(d), False,
                                        ButtonPressMask, GrabModeAsync,
                                        GrabModeAsync, None, None, CurrentTime);
    if (grabStatus == GrabSuccess) XUngrabPointer(d, CurrentTime);
    XSync(d, False);

    // The probe above grabs with ButtonPressMask on THIS connection, while the
    // synthetic press below is issued from XTestDriver's SEPARATE connection.
    // XSync(d) orders d and says nothing about the driver, so in principle the
    // press can reach the server before it has processed this ungrab, and the
    // probe's own grab would then swallow the click. This settle closes that
    // ordering hole and is worth keeping on those grounds alone.
    //
    // It is NOT, however, the cause of this case's intermittent failure, and
    // saying so would be guessing. MEASURED, 12 isolated runs each:
    //
    //   without the settle .............. 11 pass / 1 fail
    //   with the settle ................. 11 pass / 1 fail
    //   pre-existing two-loop menu() .... 11 pass / 1 fail
    //
    // The third row is the one that settles attribution: the same rate on the
    // OLD menu implementation means the flake predates the one-grab rewrite and
    // is not caused by it. It belongs to the deferred item 12 / item 17 family
    // -- the menu occasionally not appearing at all -- which is recorded, and
    // still unexplained, rather than fixed here.
    settleWm(d);
    INFO("XGrabPointer after the submenu episode returned "
         << grabStatus << " (GrabSuccess=" << GrabSuccess
         << ", AlreadyGrabbed=" << AlreadyGrabbed << ")");
    CHECK(grabStatus != AlreadyGrabbed);
    CHECK(grabStatus == GrabSuccess);

    // --- Episode 2: the menu must open again AND track the pointer. Opening is
    //     not enough on its own -- a leaked grab still lets the window map, and
    //     it is the MotionNotify that never arrives.
    Window menu2 = None;
    Rect menuRect2;
    std::string why2;
    const bool reopened = openRootMenuVerified(d, driver, kMenuPressX, kMenuPressY,
                                               menu2, menuRect2, why2,
                                               namedPixel(d, "blue"));
    INFO("second menu open: " << why2);
    INFO("WM stderr at second open:\n" << fixture.wmStderr());
    REQUIRE(reopened);

    // "A highlight exists" is NOT enough and the difference is load-bearing: if
    // the previous episode's menu were still mapped, its stale highlight would
    // satisfy that check without the WM having processed anything at all.
    // MEASURED while writing this -- with the final XUnmapWindow removed, a
    // presence-only check passed vacuously.
    //
    // So this asserts WHERE the highlight is. Row 0's fill starts at y == 9 in
    // menu-window coordinates, so a highlight belonging to row 0 begins above
    // the first row boundary, and one left over from the category row we hovered
    // in episode 1 does not.
    int r2x0 = 0, r2y0 = 0, r2x1 = 0, r2y1 = 0;
    const bool tracks = nudgeUntil(driver, menuRect2.x + menuRect2.w / 2,
                                   menuRect2.y + 14, [&] {
        const Bitmap b = captureRootBitmap(d, menuRect2);
        return pixelBounds(b, hl, r2x0, r2y0, r2x1, r2y1) && r2y0 < entryHeight;
    });

    std::printf("[wm2 menureopen] episode 1 submenu %s\n"
                "[wm2 menureopen] grab after episode: %s\n"
                "[wm2 menureopen] menu reopened %s, highlight tracks: %s\n",
                describe(rectOf(d, submenu)).c_str(),
                grabStatus == GrabSuccess ? "free" : "STILL GRABBED",
                describe(menuRect2).c_str(), tracks ? "yes" : "NO");
    std::printf("[wm2 menureopen] reopened highlight band y=%d..%d (row 0 requires y0 < %d)\n",
                r2y0, r2y1, entryHeight);
    std::fflush(stdout);

    dismissMenu(d, driver);

    INFO("second menu " << describe(menuRect2)
         << " highlight tracked the pointer: " << tracks);
    CHECK(tracks);

    // A failed XGrabPointer inside menu() must not pass silently.
    const std::string errs = fixture.wmStderr();
    INFO("WM stderr:\n" << errs);
    CHECK_FALSE(contains(errs, "AlreadyGrabbed"));

    CHECK(fixture.wmAlive());
}


// ===========================================================================
// [wm_menuback] -- travelling BACK from the submenu to the outer menu
//
// This is the operator's reported defect stated exactly: "once a submenu has
// been activated, the main menu cannot be re-activated: it remains static even
// if the submenu has been abandoned and the cursor is moving over main menu
// items. I tried to reactivate by approaching from different directions, but it
// would not work."
//
// [wm_menureopen] does NOT cover this. That case abandons the submenu, releases,
// and opens a SECOND menu -- it proves the grab came back, not that the outer
// menu is reachable while a submenu is open. The reported symptom happens
// mid-interaction, with the button still held, and would survive a green
// [wm_menureopen] untouched. Adding this was the direct result of noticing that
// gap in my own coverage rather than in the code.
//
// What must hold, with the button STILL DOWN, after moving from a category row
// back onto a non-category row of the outer menu:
//   1. the submenu is unmapped, and
//   2. the outer menu's highlight has MOVED to the row now under the pointer.
//
// Under the old two-loop design (2) was impossible: the submenu's nested loop
// owned every pointer event, so the outer menu kept a frozen highlight on the
// category row. Assert the highlight's POSITION, never its mere presence -- a
// frozen highlight from the category row is still a highlight, and a
// presence-only check passes vacuously against exactly the bug being fixed.
// ===========================================================================

TEST_CASE("Moving from a submenu back to the outer menu re-highlights it and closes the submenu",
          "[wm_menuback]")
{
    WmFixture fixture(cleanFixture({"--menu-background=blue",
                                    "--menu-foreground=red",
                                    "--menu-highlight=green"}));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    XTestDriver driver(fixture.display());
    parkPointer(d);

    const unsigned long hl = namedPixel(d, "green");
    REQUIRE(hl != ~0UL);

    Window menu = None;
    Rect menuRect;
    std::string why;
    REQUIRE(openRootMenuVerified(d, driver, kMenuPressX, kMenuPressY,
                                 menu, menuRect, why, namedPixel(d, "blue")));
    INFO("menu open: " << why);

    // Row 0, to learn the row height from the highlight band it produces.
    REQUIRE(nudgeUntil(driver, menuRect.x + menuRect.w / 2, menuRect.y + 14, [&] {
        return countAll(captureRootBitmap(d, menuRect), hl) > 0;
    }));
    int hx0 = 0, hy0 = 0, hx1 = 0, hy1 = 0;
    REQUIRE(pixelBounds(captureRootBitmap(d, menuRect), hl, hx0, hy0, hx1, hy1));
    const int entryHeight = hy1 - hy0 + 1;
    REQUIRE(entryHeight > 0);

    // Walk down the rows until one opens a submenu.
    const std::vector<Window> before = childrenOf(d, DefaultRootWindow(d));
    Window submenu = None;
    int categoryRow = -1;
    const int rowCount = (menuRect.h - 13) / entryHeight;

    for (int row = 1; row < rowCount && submenu == None; ++row) {
        const int ty = menuRect.y + 14 + row * entryHeight;
        nudgeUntil(driver, menuRect.x + menuRect.w / 2, ty, [&] {
            for (Window w : childrenOf(d, DefaultRootWindow(d))) {
                if (w == menu) continue;
                Rect r;
                if (!isViewable(d, w) || !serverRect(d, w, r)) continue;
                if (r.w <= 1 || r.h <= 1) continue;
                if (std::find(before.begin(), before.end(), w) == before.end() ||
                    r.x != menuRect.x) {
                    submenu = w;
                    return true;
                }
            }
            return false;
        });
        if (submenu != None) categoryRow = row;
    }

    if (submenu == None) {
        dismissMenu(d, driver);
        WARN("host has no application categories -- back-travel not exercised");
        return;
    }

    INFO("submenu opened from row " << categoryRow << ": " << describe(rectOf(d, submenu)));

    // --- The move under test. Button is STILL DOWN. Back to row 0, which is
    //     "New" -- a non-category row, so the submenu must close.
    const bool backHighlighted =
        nudgeUntil(driver, menuRect.x + menuRect.w / 2, menuRect.y + 14, [&] {
            int y0 = 0, y1 = 0, x0 = 0, x1 = 0;
            const Bitmap b = captureRootBitmap(d, menuRect);
            // Row 0's fill begins at y == 9 in menu coordinates, so a highlight
            // that belongs to row 0 starts above the first row boundary. A
            // highlight frozen on the category row starts far below it.
            return pixelBounds(b, hl, x0, y0, x1, y1) && y0 < entryHeight;
        });

    const bool submenuClosed = !isViewable(d, submenu);

    std::printf("[wm2 menuback] category row %d, submenu %s\n"
                "[wm2 menuback] after moving back to row 0: highlight tracks %s, submenu closed %s\n",
                categoryRow, describe(rectOf(d, submenu)).c_str(),
                backHighlighted ? "yes" : "NO",
                submenuClosed ? "yes" : "NO");
    std::fflush(stdout);

    dismissMenu(d, driver);

    INFO("WM stderr:\n" << fixture.wmStderr());
    CHECK(backHighlighted);
    CHECK(submenuClosed);
    CHECK(fixture.wmAlive());
}


// ---------------------------------------------------------------------------
// The close/hide button answers across the whole top square of the tab
//
// The button is the only pointer route to hiding or closing a window and it was
// 8x8 -- 64 square pixels. MEASURED with a hit probe against the pre-fix binary,
// on a 16px tab, every direction of near-miss did something ELSE:
//
//     4px to any side          -> the TAB, which starts a DRAG
//     1-2px right or below     -> the frame's shaped hole, which falls THROUGH
//                                 to the root window and opens the menu
//
// Missing a close button by two pixels and getting a dragged window or the root
// menu is a worse outcome than missing it and getting nothing, which is what
// made this worth widening rather than leaving to the user's aim.
//
// The fix widens only the INPUT region: the button window is grown to the tab's
// top square while a bounding shape holds the PAINTED square at its old size and
// place. This case therefore asserts both halves. The second is not decoration
// -- without it "make the target easier to hit" silently becomes "make the
// button bigger", which was explicitly not wanted.
//
// Driven by real presses rather than by XTranslateCoordinates, which models
// neither input shapes nor setFrameVisibility()'s subtraction of the button
// square from an INACTIVE client's frame. An earlier cut of this case used that
// call and reported a hit region that no press agrees with.
// ---------------------------------------------------------------------------

TEST_CASE("The window button answers across the whole tab-top square while "
          "painting the same square as before", "[wm_button]")
{
    WmFixture fixture(cleanFixture());
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    parkPointer(d);

    XTestDriver driver(fixture.display());

    // Presses the button of `client`'s frame at frame-relative (dx,dy), the way
    // a user aiming at the button and missing by that much would.
    auto pressAt = [&](Window frame, Window client, int dx, int dy) {
        // The button square is SUBTRACTED from an inactive client's frame shape,
        // so the window has to be active before a press can reach it at all.
        const Rect c = rectOf(d, client);
        driver.moveTo(c.x + c.w / 2, c.y + c.h / 2);
        XSync(d, False);
        REQUIRE(WmFixture::pollUntil([&] {
            pumpWm(d);
            return activeWindow(d) == client;
        }, 8000));

        const Rect f = rectOf(d, frame);
        driver.moveTo(f.x + dx, f.y + dy);
        XSync(d, False);
        driver.press(Button1);
        driver.release(Button1);
        XSync(d, False);
    };

    auto hidesWhenPressedAt = [&](int dx, int dy) {
        Window win = createClient(d, 200, 200, 300, 200, "buttonhit");
        XMapWindow(d, win);
        XSync(d, False);
        Window frame = awaitFrameFor(d, win);
        REQUIRE(frame != None);
        settleWm(d);

        pressAt(frame, win, dx, dy);

        long state = -1;
        const bool hidden = WmFixture::pollUntil([&] {
            pumpWm(d);
            return icccmState(d, win, state) && state == IconicState;
        }, 4000);

        XDestroyWindow(d, win);
        XSync(d, False);
        settleWm(d);
        return hidden;
    };

    // Everything below is expressed against the TAB WIDTH, measured live.
    // Border::buttonDrawSize() is m_tabWidth - TAB_TOP_HEIGHT * 2 - 4 and
    // buttonHitSize() is m_tabWidth itself, so every offset in this case is a
    // function of a number that depends on whatever face fontconfig resolves.
    // It used to be written out as the literals that number produced on this
    // host (8, 12, 13, 15), which quietly turned a case about the BUTTON into a
    // case about the tab font: quick task 260906-ldw widened the strip from
    // 16 px to 25 px to clear the label's descenders and every one of those
    // literals became wrong, none of them because anything about the button had
    // changed. Derived from the client's inset in the frame, which is
    // Border::xIndent() == m_tabWidth + FRAME_WIDTH + 1.
    int tabWidth = 0;

    // The painted square is unchanged: still buttonDrawSize() at the same inset.
    // Read off the BOUNDING shape, because the window is deliberately larger
    // than what it paints; an unshaped button reports its whole rectangle, so
    // this reads correctly against either build.
    {
        Window win = createClient(d, 200, 200, 300, 200, "buttondraw");
        XMapWindow(d, win);
        XSync(d, False);
        Window frame = awaitFrameFor(d, win);
        REQUIRE(frame != None);
        settleWm(d);

        tabWidth = (rectOf(d, win).x - rectOf(d, frame).x) - kDefaultFrameThickness - 1;
        INFO("measured tab width " << tabWidth);
        REQUIRE(tabWidth > 8);

        Window button = findFrameChild(d, frame, win, true);
        REQUIRE(button != None);

        int bx = 0, by = 0;
        Window ignore = None;
        XTranslateCoordinates(d, button, frame, 0, 0, &bx, &by, &ignore);

        int count = 0, ordering = 0;
        XRectangle* rects = XShapeGetRectangles(d, button, ShapeBounding,
                                                &count, &ordering);
        REQUIRE(rects != nullptr);
        REQUIRE(count == 1);
        const int drawnX = bx + rects[0].x;
        const int drawnY = by + rects[0].y;
        const int drawnW = rects[0].width;
        const int drawnH = rects[0].height;
        XFree(rects);

        INFO("painted square at frame (" << drawnX << "," << drawnY << ") "
             << drawnW << "x" << drawnH << ", tab width " << tabWidth);
        CHECK(drawnX == 4);                  // buttonDrawInset()
        CHECK(drawnY == 4);
        CHECK(drawnW == tabWidth - 8);       // buttonDrawSize()
        CHECK(drawnH == tabWidth - 8);

        XDestroyWindow(d, win);
        XSync(d, False);
        settleWm(d);
    }

    // Control: the centre of the painted square has always worked. If this fails
    // the case proves nothing about the corners below.
    CHECK(hidesWhenPressedAt(6, 6));

    // The near-misses. Every one of these used to hit the tab or fall through.
    // The hit square is buttonHitSize() == m_tabWidth on a side, so the far
    // corner is at m_tabWidth - 1 and the band the painted square does not cover
    // begins at m_tabWidth - 4.
    CHECK(hidesWhenPressedAt(1, 1));
    CHECK(hidesWhenPressedAt(tabWidth - 4, tabWidth - 4));
    CHECK(hidesWhenPressedAt(tabWidth - 1, tabWidth - 1));
    // The residual, pinned deliberately rather than left unsaid: a 1px sliver
    // down the notch's inner edge is not part of ANY window of this frame -- the
    // frame's own bounding shape excludes it, so a press there reaches the root
    // window. Nothing in the press handlers can claim it; only adding those
    // pixels to the frame's shape could, and that would fill the visible gap the
    // notch is made of. Left alone on purpose. If a later change closes the gap,
    // this flips and should be updated, not deleted.
    CHECK_FALSE(hidesWhenPressedAt(tabWidth - 3, tabWidth - 3));

    // ...and the widening stops at the tab's top square: below it the tab must
    // still be draggable, or this would trade a fiddly button for a window that
    // cannot be moved.
    CHECK_FALSE(hidesWhenPressedAt(6, tabWidth + 14));

    REQUIRE(fixture.wmAlive());
    INFO("wm stderr:\n" << fixture.wmStderr());
    CHECK(joined(xProtocolErrorsExceptBadWindow(fixture.wmStderr())).empty());
}


// ===========================================================================
// [wm_bevel] -- THE FLAT LOOK: no bevel on either tab, active or inactive
//
// Quick task 261004-vp6 restored the flat look the 1997 original had. Plan
// 08.5-02's 1 px raised bevel was active-only, and this case used to pin that
// asymmetry; the asymmetry is gone because the bevel is gone, so the case now
// pins its absence on BOTH windows. A changed visual claim gets a changed
// assertion rather than a deleted case: an edge highlight that crept back in
// would look plausible in a screenshot and would be nobody's bug report.
//
// The two shade constants below are KEPT, and they are still correct. The tab
// background is still #C8CACC and WindowManager::allocateShadeOf() still
// derives these exact two shades from it at frame construction -- the bevel GCs
// are still allocated and still re-derived on a live tab-background change.
// What changed is that nothing draws with them any more, so the shades are the
// right values to look for and the right answer is zero of each.
//
// The focus cue is unaffected: an inactive window's frame is shape-subtracted
// away, which is how this window manager showed activity before the bevel
// existed and how it shows it again.
// ===========================================================================

namespace {

// The highlight shade WindowManager::allocateShadeOf() produces from the
// shipped #C8CACC tab background at +0.76 toward white. MEASURED, not derived
// here -- recomputing the blend in the test would let the same arithmetic error
// pass on both sides.
const char* const kBevelHighlight = "#F2F3F3";
const char* const kBevelShadow    = "#898A8B";

// The shipped tab background, which both tabs are still painted in. Used as
// the vacuity guard below: it is what proves the sampled rectangle is a tab.
const char* const kShippedTabBackground = "#C8CACC";

}  // namespace

TEST_CASE("Neither the active nor the inactive window's tab wears a bevel",
          "[wm_bevel]")
{
    WmFixture fixture(cleanFixture({}));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    parkPointer(d);

    const unsigned long light  = namedPixel(d, kBevelHighlight);
    const unsigned long shadow = namedPixel(d, kBevelShadow);
    const unsigned long tabBg  = namedPixel(d, kShippedTabBackground);
    REQUIRE(light  != ~0UL);
    REQUIRE(shadow != ~0UL);
    REQUIRE(tabBg  != ~0UL);

    Window first = None;
    const Window firstFrame =
        mapClientAndAwaitFrame(d, 60, 60, 240, kTallWindowH, first, "first");
    REQUIRE(firstFrame != None);
    settleWm(d);

    auto tabHistogram = [&](Window frame, Window client) {
        const Window tab = findFrameChild(d, frame, client, false);
        REQUIRE(tab != None);
        Rect abs{};
        REQUIRE(serverRect(d, tab, abs));
        return captureRoot(d, abs);
    };

    // --- active -------------------------------------------------------
    const Histogram activeTab = tabHistogram(firstFrame, first);
    const long activeLight  = countOf(activeTab, light);
    const long activeShadow = countOf(activeTab, shadow);

    std::printf("[wm2 bevel] ACTIVE   tab: highlight %4ld px, shadow %4ld px\n",
                activeLight, activeShadow);
    std::fflush(stdout);

    // VACUITY GUARD (a), for the ACTIVE capture. Four `== 0` assertions would
    // all pass on an empty or mis-aimed capture, so the rectangle has to prove
    // it is a tab first: a real tab contributes thousands of pixels of the tab
    // background, and a mis-aimed one contributes none.
    //
    // A FLOOR, not a dominance test. The tab window is an L -- a band across
    // the frame's top and a column down its left, with a stair-stepped
    // diagonal -- and it is SHAPED, so the root background shows through most
    // of its bounding rectangle. MEASURED here: ~19900 px of root white
    // against ~1500 px of tab background, so the dominant pixel of this
    // capture is the desktop and always was. The 200 floor is the same one
    // this file's other tab-colour cases use.
    INFO("wm stderr:\n" << fixture.wmStderr());
    INFO("active tab histogram: " << describeTop(nullptr, activeTab));
    REQUIRE(countOf(activeTab, tabBg) > 200);

    CHECK(activeLight  == 0);
    CHECK(activeShadow == 0);

    // --- now make it inactive by mapping a second client --------------
    Window second = None;
    const Window secondFrame =
        mapClientAndAwaitFrame(d, 420, 60, 240, kTallWindowH, second, "second");
    REQUIRE(secondFrame != None);
    settleWm(d);
    settleWm(d);

    const Histogram inactiveTab = tabHistogram(firstFrame, first);
    const long inactiveLight  = countOf(inactiveTab, light);
    const long inactiveShadow = countOf(inactiveTab, shadow);

    std::printf("[wm2 bevel] INACTIVE tab: highlight %4ld px, shadow %4ld px\n",
                inactiveLight, inactiveShadow);
    std::fflush(stdout);

    // VACUITY GUARD (b): the second window really did take focus, so the first
    // window's tab really is the INACTIVE one. Read from _NET_ACTIVE_WINDOW
    // rather than inferred from a bevel -- that inference is what the flat look
    // removed, and the property is what made the comparison meaningful anyway.
    INFO("_NET_ACTIVE_WINDOW: " << activeWindow(d) << ", second: " << second);
    REQUIRE(activeWindow(d) == second);

    // VACUITY GUARD (a) again, for the INACTIVE capture.
    INFO("inactive tab histogram: " << describeTop(nullptr, inactiveTab));
    REQUIRE(countOf(inactiveTab, tabBg) > 200);

    INFO("active " << activeLight << " -> inactive " << inactiveLight);
    CHECK(inactiveLight  == 0);
    CHECK(inactiveShadow == 0);

    const std::string errs = fixture.wmStderr();
    INFO("WM stderr:\n" << errs);
    CHECK_FALSE(contains(errs, "BadMatch"));
    CHECK_FALSE(contains(errs, "BadValue"));
    CHECK_FALSE(contains(errs, "BadDrawable"));
}

// ---------------------------------------------------------------------------
// The window manager's own report of the cold-cache property wait (08.5-06).
//
// This case is the observer end of a path that starts inside the window
// manager's event loop: src/Events.cpp invalidates m_currentTime on every
// iteration, WindowManager::timestamp() falls into its cold-cache branch when a
// handler did not record an event time of its own, and the counters that branch
// keeps are printed as one summary line at the tail of loop(). The line is only
// readable after the child has exited, which is why this case terminates the
// window manager before reading its stderr.
//
// Its value is a PRINTED MEASUREMENT, not a threshold -- the same shape as the
// [wm_stress] resident-memory case. It asserts exactly two things: that the
// summary line reached the observer at all, and that the workload actually
// reached the branch. The second is an ANTI-VACUITY GUARD, not a claim about
// any defect: a case reporting cold=0 measures nothing. No bound on the wait is
// asserted, because no bound exists yet -- installing one is plan 08.5-07's
// work and asserting one here would pre-judge the measurement this case exists
// to take.
// ---------------------------------------------------------------------------

namespace {

// The summary line's four figures, as printed by WindowManager::loop(), plus
// the line itself. The raw line is kept because this case ECHOES it verbatim to
// stdout: a `with message:` block is only rendered when an assertion fails, so a
// green run would otherwise carry no evidence of the very line it exists to
// prove reached the observer.
struct TimestampSummary {
    bool          found        = false;
    unsigned long cold         = 0;
    unsigned long blocked      = 0;
    unsigned long foreign      = 0;
    long          longestMs    = 0;
    std::string   line;
};

TimestampSummary parseTimestampSummary(const std::string& stderrText)
{
    TimestampSummary s;
    const char* kMarker = "wm2: timestamp: cold=";
    const std::string::size_type at = stderrText.rfind(kMarker);
    if (at == std::string::npos) return s;

    const std::string line = stderrText.substr(at, stderrText.find('\n', at) - at);
    if (std::sscanf(line.c_str(),
                    "wm2: timestamp: cold=%lu blocked=%lu foreign=%lu longestms=%ld",
                    &s.cold, &s.blocked, &s.foreign, &s.longestMs) != 4) {
        return s;
    }
    s.line  = line;
    s.found = true;
    return s;
}

}  // namespace

TEST_CASE("The window manager reports how often its timestamp path took a "
          "cold-cache property wait", "[wm_timestamp]")
{
    WmFixture fixture(cleanFixture({}));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    parkPointer(d);

    Window firstClient = None, secondClient = None;
    const Window firstFrame = mapClientAndAwaitFrame(d, 40, 40, 240, 200, firstClient);
    REQUIRE(firstFrame != None);
    const Window secondFrame = mapClientAndAwaitFrame(d, 400, 40, 240, 200, secondClient);
    REQUIRE(secondFrame != None);

    // Drive the pointer-entry focus route: an EnterNotify reaches
    // considerFocusChange(), the pointer-stopped deadline expires in
    // checkDelaysForFocus(), and the activate() that follows calls
    // timestamp(false) from a handler that recorded no event time of its own.
    //
    // settleWm() between iterations, never a tight pumpWm() loop: the spacing
    // is what lets the deadline actually expire, and a tight loop of the same
    // length measurably reads stale state elsewhere in this file.
    for (int i = 0; i < 8; ++i) {
        const Window target = (i % 2 == 0) ? firstClient : secondClient;
        XWarpPointer(d, None, target, 0, 0, 0, 0, 60, 60);
        XSync(d, False);
        settleWm(d);
    }

    parkPointer(d);
    settleWm(d);

    REQUIRE(fixture.wmAlive());

    // The summary is written at the tail of loop(), so it is only in the
    // captured stderr once the child has exited.
    REQUIRE(fixture.terminateWmCleanly());

    const std::string errs = fixture.wmStderr();
    const TimestampSummary summary = parseTimestampSummary(errs);

    // Echoed VERBATIM, so the window manager's own line is in the ctest
    // transcript of a passing run and not only of a failing one. The line is
    // fixed ASCII state words plus integers by construction (T-8-TRACE-01), so
    // echoing it into a committed transcript is safe.
    std::printf("[wm_timestamp] %s\n",
                summary.found ? summary.line.c_str()
                              : "NO SUMMARY LINE -- the window manager printed none");
    std::printf("[wm_timestamp] cold-cache property wait: "
                "branch entries %lu, blocked waits %lu, foreign matches %lu, "
                "longest blocked wait %ld ms\n",
                summary.cold, summary.blocked, summary.foreign, summary.longestMs);
    std::fflush(stdout);

    INFO("wm stderr:\n" << errs);

    // 1. The fact reached the observer end-to-end.
    REQUIRE(summary.found);

    // 2. ANTI-VACUITY GUARD. A run reporting zero branch entries never reached
    //    the path and measures nothing.
    CHECK(summary.cold > 0);
}


// ---------------------------------------------------------------------------
// WINDOWS.md ledger 8. Every modal grab loop -- the root menu, releaseGrab(),
// move, resize, the tab button and the gesture recogniser -- waited either in
// XMaskEvent or in a 50 ms sleep, and neither watched the exit flag or the
// self-pipe. SIGTERM delivered while the operator held a button was therefore
// honoured only once the button came up. The menu is the loop a test can hold
// open deterministically: Button1 stays pressed for the menu's whole life.
TEST_CASE("SIGTERM while the root menu is held open is honoured without waiting "
          "for the release", "[wm_process]")
{
    WmFixture fixture(cleanFixture());
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    XTestDriver driver(fixture.display());   // throws if XTEST is unavailable
    parkPointer(d);

    Window menu = None;
    Rect menuRect{};
    std::string why;
    const bool opened = openRootMenuVerified(d, driver, kMenuPressX, kMenuPressY,
                                             menu, menuRect, why,
                                             namedPixel(d, kShippedMenuBackground));
    INFO("menu open diagnostics: " << why);
    REQUIRE(opened);
    // Button1 is still held here; the window manager is inside menu()'s loop.

    // The fixture's own pid, SIGTERM only. 3000 ms is well inside the fixture's
    // 8000 ms SIGKILL escalation and far above a healthy exit, which the
    // [wm_process] SIGTERM case measures in tens of milliseconds.
    const bool clean = fixture.terminateWmCleanly(3000);
    // Released only AFTER the verdict is taken, so the release cannot be what
    // let the loop out.
    driver.release(Button1);
    INFO("wm stderr: " << fixture.wmStderr());
    REQUIRE(clean);
}


// ---------------------------------------------------------------------------
// Issue #3 / Codex P1: the frameless managed path. A no-decorate window that
// has lost focus carries a pointer-synchronous button grab, exactly as a frame
// does; a click on it must (1) focus it, (2) still reach the client -- the WM
// replays the press -- and (3) leave the root menu working, because the old
// code's grab/ungrab on parent() would have landed on the ROOT window.
namespace {

std::string seedNoDecorateConfig()
{
    return "rule-match-class=WmRuntimeFrameless\n"
           "rule-no-decorate=true\n";
}

Window createClassedWindow(Display* d, int x, int y, int w, int h,
                           const char* instance, const char* cls)
{
    const Window root = DefaultRootWindow(d);
    Window win = XCreateSimpleWindow(d, root, x, y, static_cast<unsigned>(w),
                                     static_cast<unsigned>(h), 0,
                                     BlackPixel(d, DefaultScreen(d)),
                                     WhitePixel(d, DefaultScreen(d)));
    XClassHint hint;
    hint.res_name  = const_cast<char*>(instance);
    hint.res_class = const_cast<char*>(cls);
    XSetClassHint(d, win, &hint);
    XSync(d, False);
    return win;
}

bool awaitInClientList(Display* d, Window win, int timeoutMs = 8000)
{
    return WmFixture::pollUntil([&] {
        pumpWm(d);
        const std::vector<Window> l = clientList(d);
        return std::find(l.begin(), l.end(), win) != l.end();
    }, timeoutMs);
}

Window rootActiveWindow(Display* d)
{
    const Atom prop = XInternAtom(d, "_NET_ACTIVE_WINDOW", False);
    Atom t = None; int f = 0; unsigned long n = 0, after = 0; unsigned char* raw = nullptr;
    Window out = None;
    if (XGetWindowProperty(d, DefaultRootWindow(d), prop, 0, 1, False, XA_WINDOW,
                           &t, &f, &n, &after, &raw) == Success && raw) {
        if (t == XA_WINDOW && f == 32 && n >= 1) out = *reinterpret_cast<Window*>(raw);
        XFree(raw);
    }
    return out;
}

} // namespace

TEST_CASE("A click on an unfocused no-decorate window focuses it, reaches it, and "
          "leaves the root menu working", "[wm_process]")
{
    WmFixture fixture(configuredFixture(seedNoDecorateConfig()));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    XTestDriver driver(fixture.display());
    parkPointer(d);

    // Frameless first (focused on map), then a framed sibling that takes the
    // focus away -- which is what installs the grab on the frameless client.
    Window frameless = createClassedWindow(d, 400, 300, 240, 160,
                                           "wmruntimeframeless", "WmRuntimeFrameless");
    XSelectInput(d, frameless, ButtonPressMask);
    XMapWindow(d, frameless);
    REQUIRE(awaitInClientList(d, frameless));
    Window framed = createClassedWindow(d, 40, 40, 200, 150, "wmruntimeframed", "WmRuntimeFramed");
    XMapWindow(d, framed);
    REQUIRE(awaitInClientList(d, framed));
    REQUIRE(WmFixture::pollUntil([&] { pumpWm(d); return rootActiveWindow(d) == framed; }, 8000));
    REQUIRE(parentOf(d, frameless) == DefaultRootWindow(d));

    // Drain anything queued on the frameless window before the click.
    XEvent drain; while (XCheckWindowEvent(d, frameless, ButtonPressMask, &drain)) {}

    driver.moveTo(400 + 120, 300 + 80);
    driver.click(Button1);

    // (1) focus moved
    const bool focused = WmFixture::pollUntil([&] { pumpWm(d); return rootActiveWindow(d) == frameless; }, 8000);
    // (2) the press reached the client itself
    XEvent got{}; bool delivered = false;
    WmFixture::pollUntil([&] {
        delivered = XCheckWindowEvent(d, frameless, ButtonPressMask, &got) == True;
        return delivered;
    }, 4000);
    const std::string err = fixture.wmStderr();
    INFO("wm stderr:\n" << err);
    INFO("active=" << rootActiveWindow(d) << " frameless=" << frameless << " framed=" << framed);
    REQUIRE(focused);
    REQUIRE(delivered);
    REQUIRE(err.find("bad parent") == std::string::npos);

    // (3) the root menu still opens: the grab bookkeeping never touched root.
    Window menu = None; Rect menuRect{}; std::string why;
    const bool opened = openRootMenuVerified(d, driver, kMenuPressX, kMenuPressY,
                                             menu, menuRect, why,
                                             namedPixel(d, kShippedMenuBackground));
    INFO("menu open diagnostics: " << why);
    REQUIRE(opened);
    driver.release(Button1);
}


// ---------------------------------------------------------------------------
// Codex P1: a category with more entries than the screen has rows was laid out
// at its full height and merely clamped to y = 0, so every row below the screen
// edge could be neither seen nor pointed at. The binary scanner routinely puts
// hundreds of programs in one category.
namespace {

// Seed an isolated XDG data tree with `count` desktop entries in ONE category
// whose name sorts before every real one, so it is the first category row.
std::string seedApplications(int count)
{
    static int counter = 0;
    const std::string base = std::string(WM2_TEST_WORKDIR) + "/runtime-apps-" +
                             std::to_string(::getpid()) + "-" + std::to_string(++counter);
    ::mkdir(base.c_str(), 0700);
    const std::string apps = base + "/applications";
    ::mkdir(apps.c_str(), 0700);
    for (int i = 0; i < count; ++i) {
        char name[32]; std::snprintf(name, sizeof name, "aaa-entry-%03d", i);
        std::ofstream out(apps + "/" + name + ".desktop");
        out << "[Desktop Entry]\nType=Application\nName=" << name
            << "\nExec=true\nCategories=AaaOverflow;\n";
    }
    return base;
}

} // namespace

TEST_CASE("A category submenu with more entries than fit stays inside the screen",
          "[wm_menulabel]")
{
    constexpr int kEntries = 60;
    WmFixtureOptions o = cleanFixture({"--menu-background=blue",
                                       "--menu-foreground=red",
                                       "--menu-highlight=green"});
    const std::string data = seedApplications(kEntries);
    o.childEnv["XDG_DATA_HOME"]  = data;
    o.childEnv["XDG_DATA_DIRS"]  = data + "/no-system-data";
    o.childEnv["XDG_CACHE_HOME"] = data + "/cache";
    WmFixture fixture(o);
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    XTestDriver driver(fixture.display());
    parkPointer(d);
    const unsigned long hl = namedPixel(d, "green");
    REQUIRE(hl != ~0UL);
    const int screenH = DisplayHeight(d, DefaultScreen(d));

    Window menu = None; Rect menuRect; std::string why;
    REQUIRE(openRootMenuVerified(d, driver, kMenuPressX, kMenuPressY,
                                 menu, menuRect, why, namedPixel(d, "blue")));
    INFO("menu open diagnostics: " << why);

    // entryHeight is measured from the row-0 highlight, as the other submenu
    // case does; it depends on the menu font.
    REQUIRE(nudgeUntil(driver, menuRect.x + menuRect.w / 2, menuRect.y + 14, [&] {
        return countAll(captureRootBitmap(d, menuRect), hl) > 0;
    }));
    int hx0 = 0, hy0 = 0, hx1 = 0, hy1 = 0;
    REQUIRE(pixelBounds(captureRootBitmap(d, menuRect), hl, hx0, hy0, hx1, hy1));
    const int entryHeight = hy1 - hy0 + 1;
    REQUIRE(entryHeight > 0);
    // Anti-vacuity: the seeded category must genuinely overflow this screen.
    REQUIRE(entryHeight * kEntries + 13 > screenH);

    const std::vector<Window> before = childrenOf(d, DefaultRootWindow(d));
    Window submenu = None;
    const bool opened = nudgeUntil(driver, menuRect.x + menuRect.w / 2,
                                   menuRect.y + 14 + entryHeight, [&] {
        for (Window w : childrenOf(d, DefaultRootWindow(d))) {
            if (w == menu) continue;
            Rect r;
            if (!isViewable(d, w) || !serverRect(d, w, r)) continue;
            if (r.w <= 1 || r.h <= 1) continue;
            submenu = w;
            return true;
        }
        return false;
    });
    INFO("wm stderr:\n" << fixture.wmStderr());
    REQUIRE(opened);
    Rect sub;
    REQUIRE(serverRect(d, submenu, sub));
    INFO("submenu " << describe(sub) << " on a " << screenH << " px tall screen, "
         << kEntries << " entries of " << entryHeight << " px");
    CHECK(sub.y >= 0);
    CHECK(sub.h <= screenH);
    CHECK(sub.y + sub.h <= screenH);
    dismissMenu(d, driver);
}


// ---------------------------------------------------------------------------
// [wm_config_runtime] -- D-11: the root menu's Configure entry
//
// The decision, in its own words: the window manager adds a "Configure..."
// entry to the top level of its root menu when a `wm2-config` binary is found
// on PATH **at startup**, and otherwise no entry at all. The probe running once
// is the decision, not a shortcut: putting the binary on PATH after the window
// manager started deliberately changes nothing until it is restarted.
//
// WHY THESE CASES LIVE AT THE END OF THE FILE rather than beside the rest of
// the [wm_config_runtime] group. They need the pixel machinery the
// [wm_menulabel] section defines further down -- captureRootBitmap(),
// pixelBounds(), nudgeUntil(), openRootMenuVerified() and seedApplications().
// Hoisting those to the top would reorder a section whose comments explain
// themselves where they sit; appending here does not move a line of it.
//
// HOW "ONE MORE ENTRY" IS OBSERVED. There is no readable label channel: the
// menu paints text with Xft and the server keeps pixels, not strings. So the
// row COUNT is derived from two things that are measured rather than assumed --
// the popup's own height, read back from the server, and the row height, read
// off the highlight band's bounding box exactly as the submenu-overflow case
// reads it. menu() lays the outer popup out as `entryHeight * n + 13`
// (src/Buttons.cpp), so n follows from those two measurements.
//
// WHAT MAKES THE COMPARISON FAIR. The two fixtures differ in exactly ONE
// environment variable. Both are handed the same seeded XDG data tree, so the
// same .desktop entry is discovered; both scan the same /usr/bin, so the binary
// scanner contributes the same "Other" category to both; both run with the same
// menu font, so entryHeight is the same measurement in each. The only free
// variable is whether a directory holding a `wm2-config` binary is on the child
// PATH.
// ---------------------------------------------------------------------------
namespace {

// Two directories: one holding an executable named `wm2-config`, one holding
// nothing. A PATH built from the first differs from a PATH built from the
// second in exactly one fact -- whether a binary of that name is reachable --
// which is the single variable these cases turn on.
struct ConfigGuiDirs {
    std::string withGui;
    std::string withoutGui;
};

ConfigGuiDirs makeConfigGuiDirs(const char* tag)
{
    static int counter = 0;
    const std::string base = std::string(WM2_TEST_WORKDIR) + "/configgui-" +
                             std::string(tag) + "-" +
                             std::to_string(::getpid()) + "-" +
                             std::to_string(++counter);
    ::mkdir(base.c_str(), 0700);

    ConfigGuiDirs dirs;
    dirs.withGui    = base + "/with";
    dirs.withoutGui = base + "/without";
    ::mkdir(dirs.withGui.c_str(), 0700);
    ::mkdir(dirs.withoutGui.c_str(), 0700);
    return dirs;
}

// Put the REAL built configuration GUI on the "with" side, by symlink rather
// than by copy. The point of using the shipped binary here instead of a
// stand-in is that the probe under test is `access(dir + "/wm2-config", X_OK)`
// against whatever the build actually produced -- a stand-in would prove the
// test's own file executable and nothing about the product.
bool linkBuiltConfigGui(const std::string& dir, const std::string& target)
{
    const std::string link = dir + "/wm2-config";
    ::unlink(link.c_str());
    return ::symlink(target.c_str(), link.c_str()) == 0;
}

// An executable named `wm2-config` that records having been run and exits at
// once. Used only by the SELECTION case, where what is being asserted is that
// the entry execs a binary of that name through the window manager's own spawn
// path -- not that GTK can open a window, which the [wm2_config_smoke] suite
// already owns. A shim also leaves nothing running to clean up, which keeps
// this case inside the project rule that every process is stopped by a PID its
// own starter created.
bool writeConfigGuiShim(const std::string& dir, const std::string& sentinel)
{
    const std::string path = dir + "/wm2-config";
    std::ofstream out(path);
    if (!out) return false;
    out << "#!/bin/sh\n"
        << ": > '" << sentinel << "'\n";
    out.close();
    return ::chmod(path.c_str(), 0700) == 0;
}

// The outer root menu's row count, from the popup geometry and the measured
// row height. Returns -1 if the highlight never appeared, so a caller can tell
// "no rows" from "never measured".
//
// The pointer is left ON row 0 with the button still held; the caller owns the
// release.
int measureMenuRows(Display* d, XTestDriver& driver, const Rect& menuRect,
                    unsigned long highlight, int& entryHeightOut)
{
    entryHeightOut = 0;
    if (!nudgeUntil(driver, menuRect.x + menuRect.w / 2, menuRect.y + 14, [&] {
            return countAll(captureRootBitmap(d, menuRect), highlight) > 0;
        })) {
        return -1;
    }

    int hx0 = 0, hy0 = 0, hx1 = 0, hy1 = 0;
    if (!pixelBounds(captureRootBitmap(d, menuRect), highlight, hx0, hy0, hx1, hy1)) {
        return -1;
    }

    const int entryHeight = hy1 - hy0 + 1;
    if (entryHeight <= 0) return -1;
    entryHeightOut = entryHeight;

    // menu(): outerH = entryHeight * n + 13.
    return (menuRect.h - 13) / entryHeight;
}

} // namespace


TEST_CASE("The root menu carries a Configure entry when wm2-config is on the "
          "window manager's PATH at startup, and not when it is not",
          "[wm_config_runtime]")
{
#ifndef WM2_CONFIG_PATH
    SKIP("this tree was configured without the settings window, so there is no "
         "wm2-config binary to put on a PATH");
#else
    const ConfigGuiDirs dirs = makeConfigGuiDirs("menu");
    REQUIRE(linkBuiltConfigGui(dirs.withGui, WM2_CONFIG_PATH));

    // One seeded data tree, used by BOTH runs, so the discovered-application
    // half of the menu is the same in each and the row-count difference can
    // only come from the entry under test.
    const std::string data = seedApplications(1);

    // Rows in the top level of the root menu, for a window manager started with
    // `path` as its whole PATH.
    auto rowsWithPath = [&](const std::string& path, int& entryHeightOut) -> int {
        WmFixtureOptions o = cleanFixture({"--menu-background=blue",
                                           "--menu-foreground=red",
                                           "--menu-highlight=green"});
        o.childEnv["XDG_DATA_HOME"]  = data;
        o.childEnv["XDG_DATA_DIRS"]  = data + "/no-system-data";
        o.childEnv["XDG_CACHE_HOME"] = data + "/cache";
        o.childEnv["PATH"]           = path;

        WmFixture fixture(o);
        x11::DisplayPtr dp = fixture.openDisplay();
        REQUIRE(dp != nullptr);
        Display* d = dp.get();
        XTestDriver driver(fixture.display());
        parkPointer(d);

        const unsigned long hl = namedPixel(d, "green");
        REQUIRE(hl != ~0UL);

        Window menu = None; Rect menuRect; std::string why;
        REQUIRE(openRootMenuVerified(d, driver, kMenuPressX, kMenuPressY,
                                     menu, menuRect, why, namedPixel(d, "blue")));
        INFO("menu open diagnostics: " << why);

        const int rows = measureMenuRows(d, driver, menuRect, hl, entryHeightOut);
        INFO("wm stderr:\n" << fixture.wmStderr());
        dismissMenu(d, driver);
        return rows;
    };

    int heightWithout = 0;
    const int rowsWithout = rowsWithPath(dirs.withoutGui, heightWithout);
    int heightWith = 0;
    const int rowsWith = rowsWithPath(dirs.withGui + ":" + dirs.withoutGui, heightWith);

    INFO("rows without the GUI on PATH: " << rowsWithout
         << " (entry height " << heightWithout << ")");
    INFO("rows with the GUI on PATH:    " << rowsWith
         << " (entry height " << heightWith << ")");

    // Anti-vacuity: a measurement that failed reports -1, and -1 == -1 would
    // otherwise satisfy a bare difference check.
    REQUIRE(rowsWithout > 0);
    REQUIRE(rowsWith > 0);
    // Same font, same fixture geometry: a differing row height would mean the
    // two runs are not comparable and the row counts below prove nothing.
    REQUIRE(heightWith == heightWithout);

    CHECK(rowsWith == rowsWithout + 1);
#endif
}


TEST_CASE("Selecting the root menu's Configure entry runs wm2-config and leaves "
          "no zombie behind", "[wm_config_runtime]")
{
    const ConfigGuiDirs dirs = makeConfigGuiDirs("spawn");
    const std::string sentinel = sentinelPath("configgui");
    ::unlink(sentinel.c_str());
    REQUIRE(writeConfigGuiShim(dirs.withGui, sentinel));

    const std::string data = seedApplications(1);

    WmFixtureOptions o = cleanFixture({"--menu-background=blue",
                                       "--menu-foreground=red",
                                       "--menu-highlight=green"});
    o.childEnv["XDG_DATA_HOME"]  = data;
    o.childEnv["XDG_DATA_DIRS"]  = data + "/no-system-data";
    o.childEnv["XDG_CACHE_HOME"] = data + "/cache";
    // /bin is on this PATH because the shim is a `#!/bin/sh` script and the
    // window manager's spawn path is execvp(), not a shell.
    o.childEnv["PATH"]           = dirs.withGui + ":/usr/bin:/bin";

    WmFixture fixture(o);
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    XTestDriver driver(fixture.display());
    parkPointer(d);

    const unsigned long hl = namedPixel(d, "green");
    REQUIRE(hl != ~0UL);

    Window menu = None; Rect menuRect; std::string why;
    REQUIRE(openRootMenuVerified(d, driver, kMenuPressX, kMenuPressY,
                                 menu, menuRect, why, namedPixel(d, "blue")));
    INFO("menu open diagnostics: " << why);

    int entryHeight = 0;
    const int rows = measureMenuRows(d, driver, menuRect, hl, entryHeight);
    REQUIRE(rows > 0);
    REQUIRE(entryHeight > 0);

    // D-11 puts the entry at the END of the top level, and the Exit slot is
    // absent here because this menu was opened at (300,5) rather than in the
    // bottom-right corner. So the last row is the Configure row.
    const int lastRowCentre = menuRect.y + 11 + (rows - 1) * entryHeight + entryHeight / 2;
    REQUIRE(nudgeUntil(driver, menuRect.x + menuRect.w / 2, lastRowCentre, [&] {
        int hx0 = 0, hy0 = 0, hx1 = 0, hy1 = 0;
        if (!pixelBounds(captureRootBitmap(d, menuRect), hl, hx0, hy0, hx1, hy1)) {
            return false;
        }
        return hy0 >= (rows - 1) * entryHeight;
    }));

    driver.release(Button1);
    XSync(d, False);

    const bool ran = WmFixture::pollUntil([&] {
        return ::access(sentinel.c_str(), F_OK) == 0;
    }, 20000);

    INFO("wm stderr:\n" << fixture.wmStderr());
    CHECK(ran);

    // The window manager double-forks, so the program it launched is orphaned
    // to init and can never be its zombie; what this counts is the intermediate
    // child, which spawnArgv() reaps itself. Selecting the entry must not have
    // introduced a second, unreaped path.
    settleWm(d);
    const int zombies = zombieChildrenOf(fixture.wm().pid());
    INFO("zombie children of the WM: " << zombies);
    CHECK(zombies == 0);

    ::unlink(sentinel.c_str());
}
