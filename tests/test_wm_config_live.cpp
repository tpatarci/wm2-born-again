// Live configuration against the REAL compiled window manager, driven through
// the REAL compiled wm2-ctl (CGUI-02, CGUI-04, plan 09-04, TEST-05).
//
// The claim this file exists to check: a command typed at a shell changes
// windows that are ALREADY OPEN. Not windows mapped afterwards -- those would
// pick up a new value at map time even from a window manager that merely stored
// it and never applied it, which is precisely the defect the central case here
// is shaped to catch. Every live case therefore maps its client FIRST, records
// the geometry, and only then issues the command.
//
// The client is the shipped wm2-ctl binary rather than a socket client written
// here. That is deliberate and is what D-17 asks for: the tool a user runs over
// SSH and the tool this suite proves the protocol with are one program, so the
// suite cannot pass against a client the user does not have.
//
// The rules this file inherits, none of them optional:
//
//   OUTCOMES ARE OBSERVED THROUGH THE SERVER OR THROUGH THE TOOL'S OWN EXIT
//   CODE AND OUTPUT. Never a claim about window-manager internals.
//
//   NO sleep()-BASED SYNCHRONISATION. Every wait is a deadline-bounded poll
//   whose exit condition is a real observation.
//
//   EVERY PROCESS IS TERMINATED BY A PID THIS FILE CREATED, never by a name
//   pattern and never by a scan of the process table.
//
//   NO CASE READS OR WRITES THE DEVELOPER'S REAL CONFIGURATION. Every fixture
//   here overrides XDG_CONFIG_HOME (and XDG_CONFIG_DIRS) into a directory it
//   made under the CMake binary tree.
//
//   THE WINDOW MANAGER'S STDERR IS BOUND TO A LOCAL AND EMITTED AS CONTEXT
//   BEFORE any assertion that may fail, never after (08.5-06's guard-ordering
//   defect: a CHECK that fails takes the case with it, and an INFO written
//   afterwards is never reached).

#include <catch2/catch_test_macros.hpp>

#include "support/WmFixture.h"
#include "support/PixelVerdict.h"
#include "support/XTestDriver.h"

#include "Config.h"
#include "ConfigFileWriter.h"
#include "SocketServer.h"
#include "ConfigProtocol.h"

#include "x11wrap.h"
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <cerrno>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifndef WM2_CTL_PATH
#error "WM2_CTL_PATH must be defined by the build system"
#endif

using namespace wm2test;

namespace {

using Clock = std::chrono::steady_clock;

// X protocol errors are a RACE in the geometry helpers below, not a failure
// (08.5-13's QuietXErrors, and the reason two [wm_socket] cases died outright
// on their first run). A child listed by XQueryTree can be destroyed before
// XGetGeometry names it, and Xlib's default handler kills the test process with
// no assertion output at all. Counted rather than merely swallowed, so a helper
// erroring systematically stays visible.
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

// The fixture already defines wm2test::pollSleep(); named differently here
// rather than shadowed, because `using namespace wm2test` above would make
// an identically named local an ambiguity rather than an override.
void settleTick() { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }

// ---------------------------------------------------------------------------
// Isolated configuration
// ---------------------------------------------------------------------------

// An XDG tree of this case's own, under the CMake binary directory rather than
// a bare /tmp name (threat T-8-TMP), unique per process and per call so two
// concurrent ctest workers cannot read each other's files. Returns the
// directory to hand the child as XDG_CONFIG_HOME.
std::string makeConfigHome(const std::string& contents)
{
    static int counter = 0;
    const std::string base = std::string(WM2_TEST_WORKDIR) + "/live-cfg-" +
                             std::to_string(::getpid()) + "-" +
                             std::to_string(++counter);
    ::mkdir(base.c_str(), 0700);
    ::mkdir((base + "/wm2-born-again").c_str(), 0700);

    std::ofstream out(base + "/wm2-born-again/config");
    out << contents;
    out.close();
    return base;
}

std::string configFileIn(const std::string& home)
{
    return home + "/wm2-born-again/config";
}

void writeConfigFile(const std::string& home, const std::string& contents)
{
    std::ofstream out(configFileIn(home));
    out << contents;
    out.close();
}

// XDG_CONFIG_DIRS is overridden as well as XDG_CONFIG_HOME, and that is not
// belt-and-braces: xdgConfigDirs() falls back to /etc/xdg when the variable is
// unset, so on a machine that happens to have a system-wide config the host's
// settings would be layered UNDER every case here and quietly change what it
// proves. Pointed at a directory that does not exist, which applyFile() skips.
WmFixtureOptions fixtureWithConfigHome(const std::string& home)
{
    WmFixtureOptions o;
    o.childEnv["XDG_CONFIG_HOME"] = home;
    o.childEnv["XDG_CONFIG_DIRS"] = home + "/no-system-config";
    return o;
}

// ---------------------------------------------------------------------------
// Running wm2-ctl
// ---------------------------------------------------------------------------

struct CtlResult {
    int exitCode = -1;
    std::string out;
    std::string err;

    std::string describe() const
    {
        return "wm2-ctl exit=" + std::to_string(exitCode) +
               "\n  stdout: " + out + "\n  stderr: " + err;
    }
};

std::string readWholeFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::string();
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    return content;
}

// Fork and exec the built wm2-ctl, with its stdout and stderr bound to files
// (never a pipe that could fill and deadlock the child), and reap it through
// the fixture's ChildProcess -- which terminates by the PID it created and
// escalates to SIGKILL on a deadline. `display` empty means "run with no
// DISPLAY at all", which is how the no-window-manager case is expressed.
CtlResult runCtl(const std::vector<std::string>& args,
                 const std::string& display,
                 const std::map<std::string, std::string>& extraEnv = {})
{
    static int counter = 0;
    const std::string stem = std::string(WM2_TEST_WORKDIR) + "/ctl-" +
                             std::to_string(::getpid()) + "-" +
                             std::to_string(++counter);
    const std::string outPath = stem + ".out";
    const std::string errPath = stem + ".err";

    CtlResult result;

    const pid_t pid = ::fork();
    if (pid < 0) return result;

    if (pid == 0) {
        const int outFd = ::open(outPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
        const int errFd = ::open(errPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (outFd >= 0) { ::dup2(outFd, STDOUT_FILENO); ::close(outFd); }
        if (errFd >= 0) { ::dup2(errFd, STDERR_FILENO); ::close(errFd); }

        if (display.empty()) ::unsetenv("DISPLAY");
        else                 ::setenv("DISPLAY", display.c_str(), 1);
        for (const auto& kv : extraEnv) ::setenv(kv.first.c_str(), kv.second.c_str(), 1);

        std::vector<std::string> owned;
        owned.push_back(WM2_CTL_PATH);
        for (const std::string& a : args) owned.push_back(a);

        std::vector<char*> argv;
        argv.reserve(owned.size() + 1);
        for (std::string& a : owned) argv.push_back(&a[0]);
        argv.push_back(nullptr);

        ::execv(WM2_CTL_PATH, argv.data());
        ::_exit(127);
    }

    ChildProcess child(pid);
    if (!child.waitForExit(20000)) {
        child.shutdown();          // by the PID this function created
        result.exitCode = -1;
    } else {
        result.exitCode = child.exitCode();
    }

    result.out = readWholeFile(outPath);
    result.err = readWholeFile(errPath);
    ::unlink(outPath.c_str());
    ::unlink(errPath.c_str());
    return result;
}

CtlResult ctl(const WmFixture& fixture, const std::vector<std::string>& args)
{
    return runCtl(args, fixture.display());
}

// The effective value, straight from the window manager. Trailing newline
// stripped, because `get` prints the bare value for a shell to capture.
std::string ctlGet(const WmFixture& fixture, const std::string& key)
{
    CtlResult r = ctl(fixture, {"get", key});
    if (r.exitCode != 0) return std::string("<get failed: ") + r.describe() + ">";
    std::string value = r.out;
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
    return value;
}

// ---------------------------------------------------------------------------
// Server observation
// ---------------------------------------------------------------------------

struct Rect { int x = 0, y = 0, w = 0, h = 0; };

bool operator==(const Rect& a, const Rect& b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

std::string describe(const Rect& r)
{
    return "(" + std::to_string(r.x) + "," + std::to_string(r.y) + " " +
           std::to_string(r.w) + "x" + std::to_string(r.h) + ")";
}

// Wake the window manager's event loop so it flushes its X output buffer. The
// nudge is override-redirect, so eventCreate() returns immediately for it and
// it can never be managed or perturb an assertion.
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
    out.x = absX; out.y = absY;
    out.w = static_cast<int>(width); out.h = static_cast<int>(height);
    return true;
}

Rect rectOf(Display* d, Window w) { Rect r; serverRect(d, w, r); return r; }

Window parentOf(Display* d, Window w)
{
    Window wroot = None, parent = None, *children = nullptr;
    unsigned int n = 0;
    if (!XQueryTree(d, w, &wroot, &parent, &children, &n)) return None;
    if (children) XFree(children);
    return parent;
}

Window createClient(Display* d, int x, int y, int w, int h, const char* name)
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
    return framed ? frame : None;
}

Window mapClientAndAwaitFrame(Display* d, int x, int y, int w, int h,
                              Window& clientOut, const char* name)
{
    Window win = createClient(d, x, y, w, h, name);
    clientOut = win;
    XMapWindow(d, win);
    XSync(d, False);
    return awaitFrameFor(d, win);
}

// Poll until the frame's geometry stops matching `before`, or the deadline
// expires. Returns the geometry seen last either way, so a failing assertion
// can print what it actually was.
Rect awaitFrameChange(Display* d, Window frame, const Rect& before, int timeoutMs = 8000)
{
    Rect now = before;
    WmFixture::pollUntil([&] {
        pumpWm(d);
        now = rectOf(d, frame);
        return !(now == before);
    }, timeoutMs);
    return now;
}

// The socket path this build resolves for a display, which is what wm2-ctl
// resolves too (DISC-01b). Used only by the cases that need to name a path that
// does NOT exist.
std::string socketPathFor(const std::string& display)
{
    return configSocketPath(display.c_str());
}


// ---------------------------------------------------------------------------
// Colour observation (plan 09-05)
//
// Read back from the SERVER with XGetImage over the ROOT window, covering the
// rectangle the window of interest occupies. Reading root rather than the
// window itself is deliberate and is the same reason tests/test_wm_runtime.cpp
// gives: the frame and the tab are SHAPED, and XGetImage's result outside a
// window's bounding shape is undefined. Root is never shaped, so what comes
// back is exactly what is on the screen.
// ---------------------------------------------------------------------------

// The Xvfb geometry WmFixture starts every display with, so a capture can be
// clamped to the screen rather than erroring on an off-screen rectangle.
constexpr int kScreenW = 1024;
constexpr int kScreenH = 768;

// The alias, rather than a second spelling, so classify() takes what
// captureRoot() returns with no conversion (08.5-13 Task 1).
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
        for (int ix = 0; ix < w; ++ix) ++h[XGetPixel(img, ix, iy)];
    }
    XDestroyImage(img);
    return h;
}

long countOf(const Histogram& h, unsigned long pixel)
{
    auto it = h.find(pixel);
    return it == h.end() ? 0 : it->second;
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

// The share of the sample the dominant value holds. Compared against
// PixelVerdict.h's kDominanceFloor so the colour cases here use the SAME
// positive criterion the menu paint decision uses, rather than a second
// threshold that could drift from it.
double dominantShare(const Histogram& h)
{
    long total = 0, best = -1;
    for (const auto& kv : h) { total += kv.second; if (kv.second > best) best = kv.second; }
    if (total <= 0) return 0.0;
    return static_cast<double>(best) / static_cast<double>(total);
}

// The pixel value the server resolves a colour NAME to on this display, which
// turns "the tab is red" into an absolute assertion rather than a comparison
// against another run.
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

std::string describeTop(const Histogram& h, std::size_t n = 4)
{
    std::vector<std::pair<long, unsigned long>> byCount;
    for (const auto& kv : h) byCount.push_back({kv.second, kv.first});
    std::sort(byCount.rbegin(), byCount.rend());
    std::string out = "{";
    for (std::size_t i = 0; i < byCount.size() && i < n; ++i) {
        out += hex(byCount[i].second) + "x" + std::to_string(byCount[i].first);
        if (i + 1 < byCount.size() && i + 1 < n) out += ", ";
    }
    out += "}";
    return out;
}

std::vector<Window> childrenOf(Display* d, Window w)
{
    Window wroot = None, parent = None, *children = nullptr;
    unsigned int n = 0;
    std::vector<Window> out;
    if (!XQueryTree(d, w, &wroot, &parent, &children, &n)) return out;
    if (children) { out.assign(children, children + n); XFree(children); }
    return out;
}

bool localRect(Display* d, Window w, Rect& out)
{
    Window rootRet = None;
    int x = 0, y = 0;
    unsigned int width = 0, height = 0, bw = 0, depth = 0;
    if (!XGetGeometry(d, w, &rootRet, &x, &y, &width, &height, &bw, &depth)) return false;
    out.x = x; out.y = y;
    out.w = static_cast<int>(width); out.h = static_cast<int>(height);
    return true;
}

// The frame's tab and button, separated by SIZE rather than by origin: the
// button covers the whole top square of the tab and so shares the tab's (0,0)
// origin, and an origin test finds whichever the server happens to list first.
Window findFrameChild(Display* d, Window frame, Window client, bool wantButton)
{
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

// The tab COLUMN of a frame, below the square the button occupies.
//
// The tab WINDOW spans the whole width of the frame -- it is the top band as
// well as the side column -- so its own rectangle is mostly the client window
// and says nothing about the tab's colour. The column's thickness is derived
// from the two origins rather than from a magic number: Border::xIndent() is
// m_tabWidth + FRAME_WIDTH + 1 and the client sits at that offset inside the
// frame, so the caller needs to know only the configured frame thickness and
// nothing about how a font is measured.
Rect tabColumnRect(const Rect& frameRect, const Rect& clientRect,
                   int frameThickness, int height = 80)
{
    const int tabW = (clientRect.x - frameRect.x) - frameThickness - 1;
    return Rect{frameRect.x, frameRect.y + tabW + 2, tabW, height};
}

// The tab's own TOP BAND: the strip of tab background that runs across the top
// of the frame above the client, between the one-pixel border rows
// Border::shapeTab() leaves at y=0 and y=TAB_TOP_HEIGHT+1.
//
// This is where the tab background is the ONLY thing on the screen. The column
// carries the label as well, and a long title puts enough ink on it that no
// single value dominates -- which is a fact about the label, not about whether
// the background was repainted. Note also that the band is painted by the
// SERVER from the tab window's background pixel rather than by drawLabel(), so
// a case asserting on it is asserting that the live path re-set that pixel and
// cleared the window, not merely that it redrew the label.
Rect tabBandRect(const Rect& frameRect, const Rect& clientRect, int frameThickness)
{
    const int tabW = (clientRect.x - frameRect.x) - frameThickness - 1;
    return Rect{frameRect.x + tabW + 4, frameRect.y + 1, clientRect.w - 8, 2};
}

// Where the pointer is parked so it is over neither a client nor the menu.
constexpr int kParkX = 5;
constexpr int kParkY = 5;

void parkPointer(Display* d)
{
    XWarpPointer(d, None, DefaultRootWindow(d), 0, 0, 0, 0, kParkX, kParkY);
    XSync(d, False);
}

void settleWm(Display* d)
{
    for (int i = 0; i < 15; ++i) { pumpWm(d); settleTick(); }
}

// Poll until `pixel` appears at least `atLeast` times in the capture of `r`,
// returning the last histogram seen either way -- so a failing assertion can
// print what was actually on the screen rather than only that it was wrong.
Histogram awaitPixelIn(Display* d, const Rect& r, unsigned long pixel,
                       long atLeast, int timeoutMs = 8000)
{
    Histogram last;
    WmFixture::pollUntil([&] {
        pumpWm(d);
        last = captureRoot(d, r);
        return countOf(last, pixel) >= atLeast;
    }, timeoutMs);
    return last;
}

// ---------------------------------------------------------------------------
// The root menu, opened with a real button press
//
// WindowManager::menu() runs a NESTED event loop with its own pointer grab, so
// while the menu is up the window manager is not in loop() and pumpWm() cannot
// wake it. Everything a menu case observes is read straight from the server
// while the press is still held.
// ---------------------------------------------------------------------------

bool isViewable(Display* d, Window w)
{
    XWindowAttributes a;
    if (!XGetWindowAttributes(d, w, &a)) return false;
    return a.map_state == IsViewable;
}

// Identified by the press point it is anchored on, never by "the first viewable
// child of root" -- 08.5-13 removed exactly that fallback after it silently
// sampled a client frame and made two different configurations produce
// byte-identical histograms.
Window findOpenMenu(Display* d, int pressX, int pressY)
{
    for (Window child : childrenOf(d, DefaultRootWindow(d))) {
        Rect r;
        if (!serverRect(d, child, r)) continue;
        if (r.w <= 1 || r.h <= 1) continue;
        if (!isViewable(d, child)) continue;
        if (pressX >= r.x && pressX < r.x + r.w &&
            pressY >= r.y && pressY < r.y + r.h) return child;
    }
    return None;
}

// Chosen so WindowManager::menu() does not need to clamp the menu to a screen
// edge: a clamp warps the pointer, the warp is a MotionNotify, and a
// MotionNotify over a category row opens a submenu nobody asked for.
constexpr int kMenuPressX = 300;
constexpr int kMenuPressY = 5;

// Open the root menu with a real press and return only once the window manager
// has actually DRAWN it in `expectedBg` -- not merely mapped it. The positive
// dominance criterion is PixelVerdict.h's, shared with the menu paint cases
// rather than restated here.
bool openRootMenu(Display* d, XTestDriver& driver, int x, int y,
                  Window& menuOut, Rect& rectOut, unsigned long expectedBg)
{
    driver.moveTo(x, y);
    driver.press(Button1);

    constexpr int kMenuStageMs = 20000;

    menuOut = None;
    if (!WmFixture::pollUntil([&] {
            menuOut = findOpenMenu(d, x, y);
            return menuOut != None;
        }, kMenuStageMs)) {
        UNSCOPED_INFO("openRootMenu: stage 1 (mapped) expired");
        return false;
    }
    if (!WmFixture::pollUntil([&] {
            return serverRect(d, menuOut, rectOut) && rectOut.w > 1 && rectOut.h > 1;
        }, kMenuStageMs)) {
        UNSCOPED_INFO("openRootMenu: stage 2 (geometry) expired");
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
        UNSCOPED_INFO("openRootMenu: stage 3 (painted) expired -- last verdict "
                      << describeVerdict(last) << ", expected " << hex(expectedBg)
                      << ", pixels " << describeTop(lastSeen));
    }
    return painted;
}

// The category flyout, if one is open. Identified as a viewable child of root
// that is larger than 1x1, is not the outer menu, and does NOT contain the
// press point -- WindowManager::menu() places the submenu to the SIDE, which is
// what distinguishes the two without either being named.
Window findOpenSubmenu(Display* d, Window outerMenu, int pressX, int pressY)
{
    for (Window child : childrenOf(d, DefaultRootWindow(d))) {
        if (child == outerMenu) continue;
        Rect r;
        if (!serverRect(d, child, r)) continue;
        if (r.w <= 1 || r.h <= 1) continue;
        if (!isViewable(d, child)) continue;
        if (pressX >= r.x && pressX < r.x + r.w &&
            pressY >= r.y && pressY < r.y + r.h) continue;
        return child;
    }
    return None;
}

// Walk the pointer down the open outer menu until a category row opens its
// flyout. Swept rather than computed: the row index of any given category
// depends on the entry height of the configured menu font and on how many
// categories the host's application scan produced, neither of which this file
// can know -- and a case that guessed would silently stop opening a submenu at
// all the day either changed.
Window openAnySubmenu(Display* d, XTestDriver& driver, Window outerMenu,
                      const Rect& menuRect)
{
    for (int y = menuRect.y + 12; y < menuRect.y + menuRect.h - 6; y += 6) {
        driver.moveTo(menuRect.x + menuRect.w / 2, y);
        Window sub = None;
        WmFixture::pollUntil([&] {
            sub = findOpenSubmenu(d, outerMenu, kMenuPressX, kMenuPressY);
            return sub != None;
        }, 400);
        if (sub != None) return sub;
    }
    return None;
}

// Release over the first menu row ("New"). WindowManager::menu() computes
// sel = (y - 11) / entryHeight from menu-relative coordinates, so a few pixels
// into the first row selects entry 0 whatever the font's entry height is.
void selectFirstMenuEntry(Display* d, XTestDriver& driver, const Rect& menuRect)
{
    driver.moveTo(menuRect.x + menuRect.w / 2, menuRect.y + 14);
    driver.release(Button1);
    XSync(d, False);
}

// ---------------------------------------------------------------------------
// Focus observation (plan 09-05)
// ---------------------------------------------------------------------------

// The one fixed wait in this file, and it is a wait for a NON-EVENT: every
// assertion of the form "this did not happen" has to give the window manager
// long enough to have done it. Named so each call site reads as what it is.
constexpr int kNonEventWaitMs = 1500;

void waitPastFocusDelays()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(kNonEventWaitMs));
}

Window activeWindow(Display* d)
{
    // Interned per call rather than cached in a static: WmFixture picks a fresh
    // display per fixture, and an atom id from one server is meaningless on the
    // next. Xlib keeps its own per-display cache, so this costs nothing.
    const Atom atom = XInternAtom(d, "_NET_ACTIVE_WINDOW", False);
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nItems = 0, bytesAfter = 0;
    unsigned char* raw = nullptr;
    if (XGetWindowProperty(d, DefaultRootWindow(d), atom, 0, 1, False, XA_WINDOW,
                           &actualType, &actualFormat, &nItems, &bytesAfter,
                           &raw) != Success) {
        return None;
    }
    Window out = None;
    if (raw && actualFormat == 32 && nItems >= 1) {
        out = *reinterpret_cast<Window*>(raw);
    }
    if (raw) XFree(raw);
    return out;
}

Window pumpedActiveWindow(Display* d) { pumpWm(d); return activeWindow(d); }

// Position of `w` in root's child list -- higher means nearer the top of the
// stack. -1 for a window that is not there.
int stackIndex(Display* d, Window w)
{
    const std::vector<Window> kids = childrenOf(d, DefaultRootWindow(d));
    for (std::size_t i = 0; i < kids.size(); ++i) {
        if (kids[i] == w) return static_cast<int>(i);
    }
    return -1;
}

int pumpedStackIndex(Display* d, Window w) { pumpWm(d); return stackIndex(d, w); }

// _NET_WM_USER_TIME, published BEFORE the window is mapped, the way a real
// application publishes it. A value of zero is the spec's explicit "do not
// focus me on map", which is what makes it the cheapest way to construct an
// unfocused window (Client::shouldFocusOnMap).
void setUserTime(Display* d, Window w, unsigned long value)
{
    const Atom atom = XInternAtom(d, "_NET_WM_USER_TIME", False);
    XChangeProperty(d, w, atom, XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&value), 1);
}

Window mapUnfocusedClient(Display* d, int x, int y, int w, int h,
                          Window& clientOut, const char* name)
{
    Window root = DefaultRootWindow(d);
    Window win = XCreateSimpleWindow(d, root, x, y,
                                     static_cast<unsigned>(w), static_cast<unsigned>(h), 0,
                                     BlackPixel(d, DefaultScreen(d)),
                                     WhitePixel(d, DefaultScreen(d)));
    if (name) XStoreName(d, win, name);
    setUserTime(d, win, 0);
    clientOut = win;
    XMapWindow(d, win);
    XSync(d, False);
    return awaitFrameFor(d, win);
}

// The width of a frame's decoration: everything the frame is minus the client
// inside it. It moves with the tab's thickness and with nothing else a
// tab-font case changes, which is what makes it the observable for "the tab was
// re-laid out" on a window that was ALREADY OPEN.
int decorationWidth(Display* d, Window frame, Window client)
{
    return rectOf(d, frame).w - rectOf(d, client).w;
}

// ---------------------------------------------------------------------------
// A raw protocol client (plan 09-05)
//
// The broadcast cases need something wm2-ctl cannot express: a connection that
// stays open across a reload, and a second connection that never completes the
// handshake. wm2-ctl performs one request and exits, by design, so those two
// are spoken here directly -- the same shape tests/test_wm_socket.cpp uses,
// with the same rule that every wait has a deadline and no read blocks.
// ---------------------------------------------------------------------------

class Conn {
public:
    explicit Conn(const std::string& path)
    {
        m_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (m_fd < 0) return;

        struct sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        if (path.size() + 1 > sizeof(addr.sun_path)) { closeFd(); return; }
        std::memcpy(addr.sun_path, path.c_str(), path.size());

        if (::connect(m_fd, reinterpret_cast<struct sockaddr*>(&addr),
                      sizeof(addr)) != 0) {
            closeFd();
        }
    }
    ~Conn() { closeFd(); }
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;

    bool connected() const { return m_fd >= 0; }

    bool sendRaw(const std::string& bytes)
    {
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const ssize_t n = ::send(m_fd, bytes.data() + sent,
                                     bytes.size() - sent, MSG_NOSIGNAL);
            if (n > 0) { sent += static_cast<std::size_t>(n); continue; }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                settleTick();
                continue;
            }
            return false;
        }
        return true;
    }

    bool send(const ConfigMessage& m) { return sendRaw(configProtocolEncode(m)); }

    bool readLine(std::string& out, int timeoutMs)
    {
        const auto until = Clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            const std::size_t nl = m_in.find('\n');
            if (nl != std::string::npos) {
                out = m_in.substr(0, nl + 1);
                m_in.erase(0, nl + 1);
                return true;
            }
            const auto left = until - Clock::now();
            if (left <= std::chrono::steady_clock::duration::zero()) return false;

            struct pollfd p;
            p.fd = m_fd;
            p.events = POLLIN;
            p.revents = 0;
            const int ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(left).count());
            const int r = ::poll(&p, 1, ms > 0 ? ms : 1);
            if (r < 0) { if (errno == EINTR) continue; return false; }
            if (r == 0) return false;

            char buf[4096];
            const ssize_t n = ::recv(m_fd, buf, sizeof(buf), 0);
            if (n == 0) return false;                 // peer closed
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
                return false;
            }
            m_in.append(buf, static_cast<std::size_t>(n));
        }
    }

    bool shakeHands()
    {
        ConfigMessage hello;
        hello.type = ConfigMessageType::Hello;
        hello.program = "test_wm_config_live";
        hello.protocol = kConfigProtocolVersion;
        if (!send(hello)) return false;

        std::string line;
        if (!readLine(line, 8000)) return false;
        ConfigMessage ack;
        return configProtocolDecode(line, ack) == ConfigDecodeResult::Ok &&
               ack.type == ConfigMessageType::HelloAck;
    }

private:
    void closeFd() { if (m_fd >= 0) ::close(m_fd); m_fd = -1; }
    int m_fd = -1;
    std::string m_in;
};

// Close a menu the press above left open, by releasing outside every row.
void closeRootMenu(Display* d, XTestDriver& driver)
{
    driver.moveTo(kParkX, kParkY);
    driver.release(Button1);
    XSync(d, False);
    settleWm(d);
}

// ---------------------------------------------------------------------------
// Fullscreen, driven the way a real client drives it
// ---------------------------------------------------------------------------

// _NET_WM_STATE_ADD / _NET_WM_STATE_REMOVE of _NET_WM_STATE_FULLSCREEN, sent to
// the ROOT window as the specification requires. The same five fields the two
// [wm_geometry] fullscreen cases fill in; spelled again here rather than
// shared, because the two files have no header in common and one more copy of
// a client message is cheaper than a header that exists to hold it.
void sendFullscreen(Display* d, Window client, bool on)
{
    Atom netWmState = XInternAtom(d, "_NET_WM_STATE", False);
    Atom fullscreen = XInternAtom(d, "_NET_WM_STATE_FULLSCREEN", False);
    if (netWmState == None || fullscreen == None) return;

    XEvent ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.type = ClientMessage;
    ev.xclient.window = client;
    ev.xclient.message_type = netWmState;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = on ? 1 : 0;          // _NET_WM_STATE_ADD / _REMOVE
    ev.xclient.data.l[1] = static_cast<long>(fullscreen);
    ev.xclient.data.l[2] = 0;
    ev.xclient.data.l[3] = 1;                   // source: application
    XSendEvent(d, DefaultRootWindow(d), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XSync(d, False);
}

// Poll until the client's own rectangle is exactly this size. Used on both
// sides of a fullscreen round trip, so each half of the case starts from an
// observation rather than from a wait.
bool awaitClientSize(Display* d, Window w, int width, int height, int timeoutMs = 8000)
{
    return WmFixture::pollUntil([&] {
        pumpWm(d);
        const Rect r = rectOf(d, w);
        return r.w == width && r.h == height;
    }, timeoutMs);
}

// The resize grabber. Border::configure() creates it as a child of the CLIENT
// window -- not of the frame -- and sizes it FRAME_WIDTH*2 square, so its size
// is a direct reading of the thickness this frame was last laid out with. The
// test client is created with no children of its own, so the frame's grabber is
// the only child there is.
Window findResizeHandle(Display* d, Window client)
{
    for (Window child : childrenOf(d, client)) {
        Rect r;
        if (!localRect(d, child, r)) continue;
        if (r.w > 0 && r.w == r.h) return child;
    }
    return None;
}

}  // namespace


// -----------------------------------------------------------------------------
// The tool alone: no window manager, no display, no toolkit
// -----------------------------------------------------------------------------

TEST_CASE("wm2-ctl exits 2 when there is no window manager to talk to",
          "[wm_config_live]")
{
    // A path under the test work directory that nothing is listening on. Named
    // explicitly rather than relying on an unused display number, so this case
    // is deterministic even on a machine running a real desktop.
    const std::string absent = std::string(WM2_TEST_WORKDIR) + "/no-such-socket-" +
                               std::to_string(::getpid());
    ::unlink(absent.c_str());

    CtlResult r = runCtl({"--socket", absent, "status"}, "");
    INFO(r.describe());

    // DISC-01a: 2 means "nothing to talk to", and it is a DIFFERENT code from
    // the refusal below, which is the entire point of having four of them -- a
    // shell script branches on "not running" separately from "refused".
    CHECK(r.exitCode == 2);
    CHECK(r.err.find("wm2:") != std::string::npos);
    CHECK(r.err.find(absent) != std::string::npos);
}

TEST_CASE("wm2-ctl gives up on a listener that never accepts, within its bound",
          "[wm_config_live]")
{
    // THE HANG THAT WAS REACHABLE WITHOUT A WINDOW MANAGER AT ALL. The tool's
    // descriptor was a BLOCKING one, and a blocking connect() to a unix socket
    // whose accept queue is full does not fail -- it waits for room, for ever.
    // A window manager wedged in a modal loop, or simply slow to accept while
    // several clients arrive at once, is enough: `wm2-ctl status` in a shell
    // script never returns and the 15-second exchange bound this tool
    // documents is not a bound at all.
    //
    // Nothing here is a window manager. A listening socket with a backlog of
    // one, saturated by connections nobody accepts, is the whole reproduction,
    // and every descriptor it opens is closed by the one that opened it.
    // A SHORT, unguessable directory from mkdtemp() (T-8-TMP) rather than the
    // test work directory: sun_path holds 108 bytes, and a build tree nested a
    // few directories deep spends more than that before it reaches a file name.
    char templ[] = "/tmp/wm2-ctl-backlog-XXXXXX";
    const char* made = ::mkdtemp(templ);
    REQUIRE(made != nullptr);
    const std::string dir  = made;
    const std::string path = dir + "/socket";
    ::unlink(path.c_str());

    const int listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    REQUIRE(listener >= 0);

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    REQUIRE(path.size() + 1 <= sizeof(addr.sun_path));
    std::memcpy(addr.sun_path, path.c_str(), path.size());
    REQUIRE(::bind(listener, reinterpret_cast<struct sockaddr*>(&addr),
                   sizeof(addr)) == 0);
    REQUIRE(::listen(listener, 1) == 0);

    // Saturate the queue with NON-BLOCKING connects, so filling it cannot hang
    // this process the way it is about to hang the one under test. The loop
    // stops at the first refusal, which is the kernel saying the queue is full.
    std::vector<int> fillers;
    for (int i = 0; i < 64; ++i) {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0) break;
        if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                      sizeof(addr)) != 0) {
            ::close(fd);
            break;
        }
        fillers.push_back(fd);
    }
    INFO("connections queued before the backlog filled: " << fillers.size());
    REQUIRE(!fillers.empty());

    const auto began = std::chrono::steady_clock::now();
    CtlResult r = runCtl({"--socket", path, "status"}, "");
    const long elapsedMs = static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - began).count());

    for (int fd : fillers) ::close(fd);
    ::close(listener);
    ::unlink(path.c_str());
    ::rmdir(dir.c_str());

    INFO(r.describe());
    INFO("elapsed " << elapsedMs << "ms");

    // DISC-01a: 2 is "nothing to talk to". runCtl's watchdog reports -1 for a
    // child it had to kill, which is what a blocking connect() produced here.
    CHECK(r.exitCode == 2);
    CHECK(r.err.find("wm2:") != std::string::npos);

    // And it gave up on its OWN deadline rather than on the harness's: the
    // tool's bound is 15 seconds, so anything beyond that plus a generous
    // margin means the wait was not the tool's.
    CHECK(elapsedMs < 19000);
}

TEST_CASE("wm2-ctl --help lists every subcommand it accepts",
          "[wm_config_live]")
{
    CtlResult r = runCtl({"--help"}, "");
    INFO(r.describe());

    CHECK(r.exitCode == 0);
    for (const char* verb : {"status", "get", "set", "reload"}) {
        INFO("subcommand: " << verb);
        CHECK(r.out.find(verb) != std::string::npos);
    }
    // And the settable keys it advertises come from the option table, so a key
    // named here is a key the parser accepts.
    CHECK(r.out.find("frame-thickness") != std::string::npos);
}

TEST_CASE("wm2-ctl refuses a malformed invocation with the usage code",
          "[wm_config_live]")
{
    // 3, not 2: nothing was attempted, so "no window manager" would be a lie.
    CHECK(runCtl({}, "").exitCode == 3);
    CHECK(runCtl({"nonsense"}, "").exitCode == 3);
    CHECK(runCtl({"get"}, "").exitCode == 3);
    CHECK(runCtl({"set", "frame-thickness"}, "").exitCode == 3);
    CHECK(runCtl({"status", "extra"}, "").exitCode == 3);
    CHECK(runCtl({"--socket"}, "").exitCode == 3);
}

TEST_CASE("the settable key list and the config file's managed key list agree",
          "[wm_config_live]")
{
    // Two lists exist -- configKeySpecs() (what `set` accepts) and
    // configFileManagedKeys() (what the GUI's file writer owns) -- and they must
    // name exactly the same settings, or a key would be settable over the socket
    // and unsavable to the file, or the reverse. Display-free; no fixture.
    std::vector<std::string> settable;
    for (const ConfigKeySpec& spec : configKeySpecs()) settable.push_back(spec.name);
    std::vector<std::string> managed = configFileManagedKeys();

    std::sort(settable.begin(), settable.end());
    std::sort(managed.begin(), managed.end());

    INFO("settable: " << settable.size() << ", managed: " << managed.size());
    CHECK(settable == managed);
}


// -----------------------------------------------------------------------------
// Asking a running window manager
// -----------------------------------------------------------------------------

TEST_CASE("wm2-ctl status prints the seven fields the window manager reports",
          "[wm_config_live]")
{
    WmFixture fixture;
    CtlResult r = ctl(fixture, {"status"});

    const std::string stderrText = fixture.wmStderr();
    INFO("wm stderr:\n" << stderrText);
    INFO(r.describe());

    REQUIRE(r.exitCode == 0);
    for (const char* field : {"version", "protocol", "uptime", "screen-width",
                              "screen-height", "managed", "hidden"}) {
        INFO("field: " << field);
        CHECK(r.out.find(field) != std::string::npos);
    }
}

TEST_CASE("wm2-ctl get prints the value the window manager is actually using",
          "[wm_config_live]")
{
    // The file says 9. A window manager answering from its built-in default
    // would say 7, and one answering from the file it read would say 9 -- which
    // is what makes this case able to tell the two apart.
    const std::string home = makeConfigHome("frame-thickness=9\n");
    WmFixture fixture(fixtureWithConfigHome(home));

    const std::string value = ctlGet(fixture, "frame-thickness");
    const std::string stderrText = fixture.wmStderr();
    INFO("wm stderr:\n" << stderrText);
    INFO("get frame-thickness -> '" << value << "'");

    CHECK(value == "9");

    // And a key nobody has: refused by name, with the connection kept open.
    CtlResult unknown = ctl(fixture, {"get", "no-such-setting"});
    INFO(unknown.describe());
    CHECK(unknown.exitCode == 1);
    CHECK(unknown.err.find("no-such-setting") != std::string::npos);
}


// -----------------------------------------------------------------------------
// The central claim: a command changes windows that are ALREADY OPEN
// -----------------------------------------------------------------------------

TEST_CASE("set frame-thickness re-frames a window that was already mapped",
          "[wm_config_live]")
{
    const std::string home = makeConfigHome("frame-thickness=7\n");
    WmFixture fixture(fixtureWithConfigHome(home));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();

    // MAPPED FIRST. A window mapped after the command would pick the new
    // thickness up at map time even from a window manager that stored the value
    // and never applied it -- the exact defect this case exists to catch.
    Window client = None;
    Window frame = mapClientAndAwaitFrame(d, 200, 150, 300, 220, client, "live-thickness");
    REQUIRE(frame != None);

    const Rect frameBefore  = rectOf(d, frame);
    const Rect clientBefore = rectOf(d, client);

    CtlResult r = ctl(fixture, {"set", "frame-thickness", "21"});

    const Rect frameAfter = awaitFrameChange(d, frame, frameBefore);
    const Rect clientAfter = rectOf(d, client);
    const std::string stderrText = fixture.wmStderr();

    INFO("wm stderr:\n" << stderrText);
    INFO(r.describe());
    INFO("frame  before " << describe(frameBefore)  << " after " << describe(frameAfter));
    INFO("client before " << describe(clientBefore) << " after " << describe(clientAfter));

    CHECK(r.exitCode == 0);

    // The decoration moved and grew: the frame's origin is further up and left
    // by the extra indent, and it is that much bigger.
    CHECK_FALSE(frameAfter == frameBefore);
    CHECK(frameAfter.w > frameBefore.w);
    CHECK(frameAfter.h > frameBefore.h);

    // And the user's own window is untouched -- same size, same place on the
    // screen. A thickness change moves decoration, not content.
    CHECK(clientAfter.w == clientBefore.w);
    CHECK(clientAfter.h == clientBefore.h);
    CHECK(clientAfter.x == clientBefore.x);
    CHECK(clientAfter.y == clientBefore.y);

    // The window manager agrees with itself about what it is now using.
    CHECK(ctlGet(fixture, "frame-thickness") == "21");
}

TEST_CASE("applying the same set twice does nothing the second time",
          "[wm_config_live]")
{
    const std::string home = makeConfigHome("frame-thickness=7\n");
    WmFixture fixture(fixtureWithConfigHome(home));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();

    Window client = None;
    Window frame = mapClientAndAwaitFrame(d, 200, 150, 300, 220, client, "live-idempotent");
    REQUIRE(frame != None);

    const Rect before = rectOf(d, frame);
    CtlResult first = ctl(fixture, {"set", "frame-thickness", "17"});
    const Rect afterFirst = awaitFrameChange(d, frame, before);

    // Watch the frame for RE-SHAPING, from this connection, and drain
    // everything the first application produced before the second command is
    // issued.
    //
    // ShapeNotify, not ConfigureNotify, and the distinction is the whole
    // instrument: the X server suppresses a ConfigureNotify for a
    // XConfigureWindow that changes nothing, so counting those would report
    // zero for a window manager that dutifully re-laid every frame out on
    // every message -- a case that cannot fail is not a case. The shape
    // extension makes no such comparison: every rectangle-combining request
    // the window manager issues produces a ShapeNotify whether or not the
    // resulting region differs, so this counter sees the WORK rather than only
    // its visible effect. Confirmed by mutation: with applyConfig()'s diff
    // removed, this count goes to a positive number and the case reddens.
    int shapeEventBase = 0, shapeErrorBase = 0;
    REQUIRE(XShapeQueryExtension(d, &shapeEventBase, &shapeErrorBase));
    XShapeSelectInput(d, frame, ShapeNotifyMask);
    XSync(d, False);
    for (int i = 0; i < 10; ++i) { pumpWm(d); settleTick(); }
    while (XPending(d)) { XEvent e; XNextEvent(d, &e); }

    CtlResult second = ctl(fixture, {"set", "frame-thickness", "17"});
    for (int i = 0; i < 15; ++i) { pumpWm(d); settleTick(); }

    int reconfigurations = 0;
    while (XPending(d)) {
        XEvent e;
        XNextEvent(d, &e);
        if (e.type == shapeEventBase + ShapeNotify) ++reconfigurations;
    }
    const Rect afterSecond = rectOf(d, frame);
    const std::string stderrText = fixture.wmStderr();

    INFO("wm stderr:\n" << stderrText);
    INFO("first: "  << first.describe());
    INFO("second: " << second.describe());
    INFO("frame before " << describe(before)
         << " after first " << describe(afterFirst)
         << " after second " << describe(afterSecond));
    INFO("re-shapes after the second set: " << reconfigurations);

    // Same acknowledgement...
    CHECK(first.exitCode == 0);
    CHECK(second.exitCode == 0);
    // ...identical geometry...
    CHECK(afterSecond == afterFirst);
    // ...and no second re-frame at all. applyConfig() diffs before it acts, so
    // an unchanged value performs no work (T-9-22).
    CHECK(reconfigurations == 0);
}

TEST_CASE("a value the parser would clamp is refused and changes nothing",
          "[wm_config_live]")
{
    const std::string home = makeConfigHome("frame-thickness=11\n");
    WmFixture fixture(fixtureWithConfigHome(home));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();

    Window client = None;
    Window frame = mapClientAndAwaitFrame(d, 200, 150, 300, 220, client, "live-refusal");
    REQUIRE(frame != None);
    const Rect before = rectOf(d, frame);

    // Both bounds, plus a value that is not a number at all, plus a key that
    // does not exist. The parser would have CLAMPED the first two to 1 and 50
    // and acknowledged them; over the socket a clamp is a refusal, because a
    // client waiting on an answer would otherwise be told "yes" to something it
    // did not ask for (T-9-20).
    CtlResult low     = ctl(fixture, {"set", "frame-thickness", "0"});
    CtlResult high    = ctl(fixture, {"set", "frame-thickness", "500"});
    CtlResult rubbish = ctl(fixture, {"set", "frame-thickness", "seven"});
    CtlResult unknown = ctl(fixture, {"set", "no-such-setting", "x"});

    for (int i = 0; i < 15; ++i) { pumpWm(d); settleTick(); }
    const Rect after = rectOf(d, frame);
    const std::string effective = ctlGet(fixture, "frame-thickness");
    const std::string stderrText = fixture.wmStderr();

    INFO("wm stderr:\n" << stderrText);
    INFO("low: "     << low.describe());
    INFO("high: "    << high.describe());
    INFO("rubbish: " << rubbish.describe());
    INFO("unknown: " << unknown.describe());
    INFO("frame before " << describe(before) << " after " << describe(after));

    CHECK(low.exitCode == 1);
    CHECK(high.exitCode == 1);
    CHECK(rubbish.exitCode == 1);
    CHECK(unknown.exitCode == 1);

    // Each refusal NAMES THE KEY, which is what makes the message actionable
    // when a script sets several things in a row.
    CHECK(low.err.find("frame-thickness") != std::string::npos);
    CHECK(high.err.find("frame-thickness") != std::string::npos);
    CHECK(unknown.err.find("no-such-setting") != std::string::npos);

    // And nothing moved.
    CHECK(after == before);
    CHECK(effective == "11");
}

TEST_CASE("a boolean set accepts the spellings the config file accepts",
          "[wm_config_live]")
{
    WmFixture fixture;

    CtlResult t = ctl(fixture, {"set", "click-to-focus", "true"});
    const std::string afterTrue = ctlGet(fixture, "click-to-focus");
    CtlResult zero = ctl(fixture, {"set", "click-to-focus", "0"});
    const std::string afterZero = ctlGet(fixture, "click-to-focus");
    CtlResult nonsense = ctl(fixture, {"set", "click-to-focus", "perhaps"});
    const std::string afterNonsense = ctlGet(fixture, "click-to-focus");

    const std::string stderrText = fixture.wmStderr();
    INFO("wm stderr:\n" << stderrText);
    INFO("true: " << t.describe() << " -> " << afterTrue);
    INFO("0: " << zero.describe() << " -> " << afterZero);
    INFO("perhaps: " << nonsense.describe() << " -> " << afterNonsense);

    CHECK(t.exitCode == 0);
    CHECK(afterTrue == "true");
    CHECK(zero.exitCode == 0);
    CHECK(afterZero == "false");

    // The config file maps every unrecognised spelling to false SILENTLY. Over
    // the socket that would acknowledge the opposite of what was typed, so it
    // is refused instead -- stricter than the file, never looser.
    CHECK(nonsense.exitCode == 1);
    CHECK(afterNonsense == "false");
}

TEST_CASE("the largest frame thickness with several windows open leaves the "
          "window manager responsive", "[wm_config_live]")
{
    WmFixture fixture;
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();

    std::vector<Window> clients;
    for (int i = 0; i < 4; ++i) {
        Window c = None;
        Window f = mapClientAndAwaitFrame(d, 100 + i * 40, 80 + i * 40, 240, 180, c,
                                          "live-stress");
        REQUIRE(f != None);
        clients.push_back(c);
    }

    // The top of the range the parser clamps to (T-9-21). The re-layout walks
    // the client list once and reparents nothing, so this is bounded work.
    CtlResult r = ctl(fixture, {"set", "frame-thickness", "50"});

    // The proof of responsiveness is that the window manager still FRAMES a new
    // window afterwards. A boundary that protects by wedging is not a
    // mitigation.
    Window late = None;
    Window lateFrame = mapClientAndAwaitFrame(d, 500, 400, 200, 160, late, "live-after");

    const std::string stderrText = fixture.wmStderr();
    INFO("wm stderr:\n" << stderrText);
    INFO(r.describe());

    CHECK(r.exitCode == 0);
    CHECK(lateFrame != None);
    CHECK(ctlGet(fixture, "frame-thickness") == "50");
}


// -----------------------------------------------------------------------------
// reload: the two configurations the window manager holds (DISC-07)
// -----------------------------------------------------------------------------

TEST_CASE("reload re-reads the file and applies it to windows already open",
          "[wm_config_live]")
{
    const std::string home = makeConfigHome("frame-thickness=7\n");
    WmFixture fixture(fixtureWithConfigHome(home));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();

    Window client = None;
    Window frame = mapClientAndAwaitFrame(d, 200, 150, 300, 220, client, "live-reload");
    REQUIRE(frame != None);
    const Rect before = rectOf(d, frame);

    // The file changes UNDER a running window manager, which is exactly the
    // situation a user is in after editing it by hand.
    writeConfigFile(home, "frame-thickness=23\n");
    CtlResult r = ctl(fixture, {"reload"});

    const Rect after = awaitFrameChange(d, frame, before);
    const std::string stderrText = fixture.wmStderr();

    INFO("wm stderr:\n" << stderrText);
    INFO(r.describe());
    INFO("frame before " << describe(before) << " after " << describe(after));

    CHECK(r.exitCode == 0);
    CHECK_FALSE(after == before);
    CHECK(ctlGet(fixture, "frame-thickness") == "23");
}

TEST_CASE("a reload discards a set, because a set writes no file",
          "[wm_config_live]")
{
    const std::string home = makeConfigHome("frame-thickness=13\n");
    WmFixture fixture(fixtureWithConfigHome(home));

    CtlResult set = ctl(fixture, {"set", "frame-thickness", "31"});
    const std::string afterSet = ctlGet(fixture, "frame-thickness");
    CtlResult reload = ctl(fixture, {"reload"});
    const std::string afterReload = ctlGet(fixture, "frame-thickness");
    const std::string fileText = readWholeFile(configFileIn(home));

    const std::string stderrText = fixture.wmStderr();
    INFO("wm stderr:\n" << stderrText);
    INFO("set: " << set.describe());
    INFO("reload: " << reload.describe());
    INFO("config file now:\n" << fileText);

    CHECK(set.exitCode == 0);
    CHECK(afterSet == "31");
    CHECK(reload.exitCode == 0);

    // D-01: `set` changes the running desktop and nothing else. The file still
    // says what it said, and the reload therefore puts the window manager back
    // on it.
    CHECK(afterReload == "13");
    CHECK(fileText.find("frame-thickness=13") != std::string::npos);
    CHECK(fileText.find("31") == std::string::npos);
}

TEST_CASE("a command-line override still wins after a reload",
          "[wm_config_live]")
{
    // The file and the command line disagree, and the command line is the top
    // layer. A reload that merged the file onto current state instead of
    // re-running the whole layered load would let the file win here -- silently
    // undoing a flag the user started the window manager with.
    const std::string home = makeConfigHome("frame-thickness=9\n");
    WmFixtureOptions options = fixtureWithConfigHome(home);
    options.wmArgs.push_back("--frame-thickness=29");
    WmFixture fixture(options);

    const std::string beforeReload = ctlGet(fixture, "frame-thickness");
    writeConfigFile(home, "frame-thickness=11\n");
    CtlResult r = ctl(fixture, {"reload"});
    const std::string afterReload = ctlGet(fixture, "frame-thickness");

    const std::string stderrText = fixture.wmStderr();
    INFO("wm stderr:\n" << stderrText);
    INFO(r.describe());
    INFO("before reload: " << beforeReload << ", after: " << afterReload);

    CHECK(beforeReload == "29");
    CHECK(r.exitCode == 0);
    CHECK(afterReload == "29");
}

TEST_CASE("a reload that cannot read the user file changes nothing and names it",
          "[wm_config_live]")
{
    if (::geteuid() == 0) {
        // root can read a mode-000 file, so the condition under test cannot be
        // created. Skipped by construction rather than asserted falsely.
        WARN("running as root: the unreadable-file case cannot be constructed");
        return;
    }

    const std::string home = makeConfigHome("frame-thickness=15\n");
    WmFixture fixture(fixtureWithConfigHome(home));

    const std::string before = ctlGet(fixture, "frame-thickness");

    const std::string path = configFileIn(home);
    writeConfigFile(home, "frame-thickness=41\n");
    REQUIRE(::chmod(path.c_str(), 0000) == 0);

    CtlResult r = ctl(fixture, {"reload"});
    const std::string after = ctlGet(fixture, "frame-thickness");

    // Restored immediately, so a failure later in this case cannot leave an
    // unreadable file behind for the fixture's teardown to trip over.
    ::chmod(path.c_str(), 0600);

    const std::string stderrText = fixture.wmStderr();
    INFO("wm stderr:\n" << stderrText);
    INFO(r.describe());
    INFO("before " << before << ", after " << after);

    CHECK(before == "15");
    CHECK(r.exitCode == 1);
    CHECK(r.err.find(path) != std::string::npos);
    // The window manager kept running on the configuration it already had --
    // it did not fall back to defaults and did not half-apply the new file.
    CHECK(after == "15");
}


// -----------------------------------------------------------------------------
// Discovery, and the boundary the tool inherits
// -----------------------------------------------------------------------------

TEST_CASE("wm2-ctl finds the socket from DISPLAY, and honours --socket",
          "[wm_config_live]")
{
    WmFixture fixture;

    // DISC-01b: the path is RESOLVED from the display name rather than read off
    // the root window, because reading a property would mean linking X11 into a
    // tool whose whole value is that it does not. This case asserts the two
    // spellings agree rather than merely both working.
    const std::string resolved = socketPathFor(fixture.display());
    CtlResult viaDisplay = ctl(fixture, {"status"});
    CtlResult viaPath = runCtl({"--socket", resolved, "status"}, "");

    const std::string stderrText = fixture.wmStderr();
    INFO("wm stderr:\n" << stderrText);
    INFO("resolved path: " << resolved);
    INFO("via DISPLAY: " << viaDisplay.describe());
    INFO("via --socket: " << viaPath.describe());

    CHECK(viaDisplay.exitCode == 0);
    CHECK(viaPath.exitCode == 0);
    CHECK(viaPath.out.find("protocol") != std::string::npos);

    // With neither a display nor a path there is nothing to resolve, and that
    // is a usage error rather than a claim that no window manager is running.
    CtlResult blind = runCtl({"status"}, "");
    INFO("no DISPLAY: " << blind.describe());
    CHECK(blind.exitCode == 3);
}


// -----------------------------------------------------------------------------
// Colours change the screen, now (CGUI-04, plan 09-05)
//
// The claim: every one of the nine colour keys repaints what is already on the
// desktop the moment it is set, with no window closing and no restart. Each
// case therefore MAPS FIRST and captures BEFORE, exactly as the frame-thickness
// case above does, so a window manager that merely stored the value could not
// pass.
//
// Every colour a case sets is FAR FROM THE DEFAULT IN EVERY CHANNEL. A colour
// close to the shipped silver would let an assertion pass on a repaint that
// never happened, because the sample would contain the wanted value anyway.
// -----------------------------------------------------------------------------

TEST_CASE("every frame colour set over the socket repaints a window already on screen",
          "[wm_config_live]")
{
    // The starting palette is written into the config file rather than left at
    // the shipped defaults, so each "was not there before" assertion is against
    // a value this case chose. tab-foreground in particular starts at a colour
    // nothing else in the frame uses, which is what lets the label's OLD ink be
    // asserted absent afterwards -- the shipped #000000 is also the border, and
    // black disappearing from a tab would mean nothing.
    const std::string home = makeConfigHome(
        "frame-thickness=7\n"
        "tab-foreground=#ff00ff\n"
        "tab-background=#c8cacc\n"
        "frame-background=#dcdee0\n"
        "button-background=#dcdee0\n"
        "borders=#ff8000\n");
    WmFixture fixture(fixtureWithConfigHome(home));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    parkPointer(d);

    // A long run of narrow glyphs, so the label puts a usable amount of ink on
    // the tab: Border::drawLabel() returns before drawing anything at all for a
    // window with no name, and the foreground colour would never reach a pixel.
    Window client = None;
    Window frame = mapClientAndAwaitFrame(d, 260, 180, 320, 240, client,
                                          "IIIIIIIIIIIIIIIIIIII");
    REQUIRE(frame != None);
    settleWm(d);

    const Window tab    = findFrameChild(d, frame, client, false);
    const Window button = findFrameChild(d, frame, client, true);
    REQUIRE(tab != None);
    REQUIRE(button != None);

    const Rect frameRect  = rectOf(d, frame);
    const Rect clientRect = rectOf(d, client);
    const Rect buttonRect = rectOf(d, button);
    (void)tab;

    // EACH KEY'S OWN RECTANGLE, derived from the two geometries above and the
    // frame thickness this case's config file sets -- never from a magic
    // number. The whole frame rectangle is dominated by the CLIENT's own white,
    // which says nothing about any of these colours, so each one is sampled
    // where it is the only thing on the screen.
    //
    // Border::xIndent() is m_tabWidth + FRAME_WIDTH + 1 and the client sits at
    // that offset inside the frame, so the tab's thickness follows from the two
    // origins without the test knowing how a font is measured.
    const int kThickness = 7;                                   // the file's value
    const Rect tabBand   = tabBandRect(frameRect, clientRect, kThickness);
    const Rect tabColumn = tabColumnRect(frameRect, clientRect, kThickness);
    REQUIRE(tabColumn.w > 4);

    // The frame background's own rectangle: the vertical strip the frame's
    // clip region leaves between the tab and the client -- FRAME_WIDTH - 2
    // pixels wide, running the client's height (Border::setFrameVisibility).
    Rect frameStrip{clientRect.x - (kThickness - 1), clientRect.y + 20,
                    kThickness - 2, 100};

    // The button's own rectangle is its INTERIOR. The one-pixel ring around it
    // is the difference between the parent's bounding and clip regions, which
    // the server paints from the parent's BORDER pixel -- that ring belongs to
    // `borders`, not to `button-background`.
    Rect buttonInner{buttonRect.x + 1, buttonRect.y + 1,
                     buttonRect.w - 2, buttonRect.h - 2};

    // The border's own rectangle: the tab window's top row. A shaped window's
    // area between its bounding and its clip region is painted by the server
    // from the window's BORDER pixel, and Border::shapeTab() makes that row
    // exactly one pixel tall across the whole width of the frame.
    Rect borderStrip{frameRect.x, frameRect.y, frameRect.w - 2, 1};

    const unsigned long oldInk   = namedPixel(d, "#ff00ff");
    const unsigned long newTabBg = namedPixel(d, "#ff0000");
    const unsigned long newFrmBg = namedPixel(d, "#00cc00");
    const unsigned long newBtnBg = namedPixel(d, "#0000ff");
    const unsigned long newBord  = namedPixel(d, "#00ffff");
    const unsigned long newInk   = namedPixel(d, "#ffff00");
    REQUIRE(oldInk   != ~0UL);
    REQUIRE(newTabBg != ~0UL);

    const Histogram bandBefore   = captureRoot(d, tabBand);
    const Histogram tabBefore    = captureRoot(d, tabColumn);
    const Histogram frameBefore  = captureRoot(d, frameStrip);
    const Histogram buttonBefore = captureRoot(d, buttonInner);
    const Histogram borderBefore = captureRoot(d, borderStrip);

    // The label's ink is on the tab before anything is set. Without this the
    // "old ink is gone" assertion below could pass on a tab that never had any.
    INFO("tab before: " << describeTop(tabBefore));
    REQUIRE(countOf(tabBefore, oldInk) > 0);

    CtlResult setTabBg = ctl(fixture, {"set", "tab-background", "#ff0000"});
    CtlResult setFrmBg = ctl(fixture, {"set", "frame-background", "#00cc00"});
    CtlResult setBtnBg = ctl(fixture, {"set", "button-background", "#0000ff"});
    CtlResult setBord  = ctl(fixture, {"set", "borders", "#00ffff"});
    CtlResult setInk   = ctl(fixture, {"set", "tab-foreground", "#ffff00"});

    const Histogram bandAfter   = awaitPixelIn(d, tabBand, newTabBg, 100);
    const Histogram tabAfter    = awaitPixelIn(d, tabColumn, newInk, 1);
    const Histogram frameAfter  = awaitPixelIn(d, frameStrip, newFrmBg, 50);
    const Histogram buttonAfter = awaitPixelIn(d, buttonInner, newBtnBg, 15);
    const Histogram borderAfter = awaitPixelIn(d, borderStrip, newBord, 5);

    const std::string stderrText = fixture.wmStderr();
    INFO("wm stderr:\n" << stderrText);
    INFO("tab-background:    " << setTabBg.describe());
    INFO("frame-background:  " << setFrmBg.describe());
    INFO("button-background: " << setBtnBg.describe());
    INFO("borders:           " << setBord.describe());
    INFO("tab-foreground:    " << setInk.describe());
    INFO("band   before " << describeTop(bandBefore)   << " after " << describeTop(bandAfter));
    INFO("tab    before " << describeTop(tabBefore)    << " after " << describeTop(tabAfter));
    INFO("frame  before " << describeTop(frameBefore)  << " after " << describeTop(frameAfter));
    INFO("button before " << describeTop(buttonBefore) << " after " << describeTop(buttonAfter));
    INFO("border before " << describeTop(borderBefore) << " after " << describeTop(borderAfter));

    CHECK(setTabBg.exitCode == 0);
    CHECK(setFrmBg.exitCode == 0);
    CHECK(setBtnBg.exitCode == 0);
    CHECK(setBord.exitCode == 0);
    CHECK(setInk.exitCode == 0);

    // Each key's own rectangle is now DOMINATED by the value that key names,
    // at or above the share PixelVerdict.h already uses as its positive paint
    // criterion -- not merely "the colour appears somewhere".
    CHECK(dominantPixel(bandAfter) == newTabBg);
    CHECK(dominantShare(bandAfter) >= kDominanceFloor);
    CHECK(dominantPixel(frameAfter) == newFrmBg);
    CHECK(dominantShare(frameAfter) >= kDominanceFloor);
    CHECK(dominantPixel(buttonAfter) == newBtnBg);
    CHECK(dominantShare(buttonAfter) >= kDominanceFloor);
    CHECK(dominantPixel(borderAfter) == newBord);
    CHECK(dominantShare(borderAfter) >= kDominanceFloor);

    // ...and none of them was there before, so the assertion cannot be passing
    // on a colour the frame already happened to contain.
    CHECK(countOf(bandBefore, newTabBg) == 0);
    CHECK(countOf(frameBefore, newFrmBg) == 0);
    CHECK(countOf(buttonBefore, newBtnBg) == 0);
    CHECK(countOf(borderBefore, newBord) == 0);

    // The label was redrawn in the new ink and none of the old ink survives
    // inside the tab. Xft antialiases, so the count is small; the glyph cores
    // are the exact colour.
    CHECK(countOf(tabAfter, newInk) > 0);
    CHECK(countOf(tabAfter, oldInk) == 0);

    // And the window manager agrees with itself about what it is drawing with.
    CHECK(ctlGet(fixture, "tab-background") == "#ff0000");
    CHECK(ctlGet(fixture, "tab-foreground") == "#ffff00");
}

TEST_CASE("a live tab-background change repaints the tab and puts no bevel "
          "shade on screen either side of it", "[wm_config_live]")
{
    // WHAT THIS CASE USED TO MEASURE, and the decision that changed it.
    //
    // Plan 08.5-02 derived the bevel shades from the configured tab background,
    // and this case proved the derivation ran again on a live change: the
    // shipped highlight was on screen before, the dark palette's own highlight
    // after. Quick task 261004-vp6 restored the flat look, so nothing draws
    // with those GCs any more.
    //
    // The GCs ARE still allocated and still re-derived on this very change
    // (src/Border.cpp, the live-apply path) -- the handoff kept them
    // deliberately. But that re-derivation is NOT externally observable: no
    // pixel anywhere depends on it. A case cannot assert a structural fact it
    // has no way to reach, and asserting it through a stand-in would be
    // asserting something else. So this case asserts the OBSERVABLE truth it
    // can still reach: the tab repaints in the new body colour, and no bevel
    // shade -- of either palette -- is on screen before OR after.
    //
    // The body colour change is the non-vacuity anchor: a case that only
    // asserted absences would pass against a blank screen.
    const std::string home = makeConfigHome(
        "frame-thickness=7\ntab-background=#c8cacc\n");
    WmFixture fixture(fixtureWithConfigHome(home));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    parkPointer(d);

    Window client = None;
    Window frame = mapClientAndAwaitFrame(d, 260, 180, 320, 240, client,
                                          "IIIIIIIIIIIIIIIIIIII");
    REQUIRE(frame != None);
    settleWm(d);

    // The column, not the tab window: this is the rectangle a bevel highlight
    // ran down the left edge of and a shadow down the right of, so it is the
    // rectangle where a returning bevel would appear.
    const Rect tabRect = tabColumnRect(rectOf(d, frame), rectOf(d, client), 7);
    REQUIRE(tabRect.w > 4);

    // The body colours: the configured #c8cacc before, #400000 after.
    const unsigned long shippedBody = namedPixel(d, "#c8cacc");
    const unsigned long darkBody     = namedPixel(d, "#400000");
    // The two shades WindowManager::allocateShadeOf() produces at +0.76 toward
    // white -- from the SHIPPED #C8CACC and from #400000 respectively. Both
    // MEASURED on this host and written down as literals rather than
    // recomputed here: doing the blend again in the test would let the same
    // arithmetic error pass on both sides. They are the values a returning
    // bevel would be drawn in, and the right answer for both is zero.
    const unsigned long shippedHighlight = namedPixel(d, "#f2f3f3");
    const unsigned long darkHighlight    = namedPixel(d, "#d1c2c2");
    REQUIRE(shippedBody      != ~0UL);
    REQUIRE(darkBody         != ~0UL);
    REQUIRE(shippedHighlight != ~0UL);
    REQUIRE(darkHighlight    != ~0UL);

    const Histogram before = captureRoot(d, tabRect);
    INFO("tab before: " << describeTop(before));
    // Non-vacuity: the tab is really painted in the configured body colour.
    REQUIRE(countOf(before, shippedBody) > 0);
    // Flat before the change.
    CHECK(countOf(before, shippedHighlight) == 0);
    CHECK(countOf(before, darkHighlight)    == 0);

    CtlResult r = ctl(fixture, {"set", "tab-background", "#400000"});
    // Wait for the BODY colour, not for a shade: the shade never arrives.
    const Histogram after = awaitPixelIn(d, tabRect, darkBody, 1);

    INFO("wm stderr:\n" << fixture.wmStderr());
    INFO(r.describe());
    INFO("tab after: " << describeTop(after, 12));

    CHECK(r.exitCode == 0);
    // The tab really repainted: the new body colour is on screen...
    CHECK(countOf(after, darkBody) > 0);
    // ...the old one is gone...
    CHECK(countOf(after, shippedBody) == 0);
    // ...and still flat afterwards, in either palette's shade.
    CHECK(countOf(after, shippedHighlight) == 0);
    CHECK(countOf(after, darkHighlight)    == 0);
}

TEST_CASE("every menu colour set over the socket reaches the next menu opened",
          "[wm_config_live]")
{
    const std::string home = makeConfigHome(
        "menu-background=#c8cacc\n"
        "menu-foreground=#000000\n"
        "menu-highlight=#a8acb0\n"
        "menu-borders=#ff00ff\n");
    WmFixture fixture(fixtureWithConfigHome(home));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    XTestDriver driver(fixture.display());
    parkPointer(d);

    const unsigned long oldBg   = namedPixel(d, "#c8cacc");
    const unsigned long oldBord = namedPixel(d, "#ff00ff");
    const unsigned long newBg   = namedPixel(d, "#00cc00");
    const unsigned long newFg   = namedPixel(d, "#ff0000");
    const unsigned long newHl   = namedPixel(d, "#0000ff");
    const unsigned long newBord = namedPixel(d, "#00ffff");
    REQUIRE(oldBg != ~0UL);

    // --- before: the menu is drawn in the file's colours --------------------
    Window menu = None;
    Rect menuRect;
    REQUIRE(openRootMenu(d, driver, kMenuPressX, kMenuPressY, menu, menuRect, oldBg));
    const Rect borderRect{menuRect.x - 1, menuRect.y - 1, menuRect.w + 2, menuRect.h + 2};
    const Histogram before       = captureRoot(d, menuRect);
    const Histogram borderBefore = captureRoot(d, borderRect);
    closeRootMenu(d, driver);

    INFO("menu before: " << describeTop(before));
    REQUIRE(countOf(borderBefore, oldBord) > 0);

    // --- the four menu colours change on a running desktop ------------------
    CtlResult setBg   = ctl(fixture, {"set", "menu-background", "#00cc00"});
    CtlResult setFg   = ctl(fixture, {"set", "menu-foreground", "#ff0000"});
    CtlResult setHl   = ctl(fixture, {"set", "menu-highlight", "#0000ff"});
    CtlResult setBord = ctl(fixture, {"set", "menu-borders", "#00ffff"});

    // --- after: the NEXT menu is drawn in them ------------------------------
    // The pointer is moved onto the first row before the capture, so the
    // highlight band is actually painted; a menu with no row selected would
    // contain no highlight pixel at all and the assertion would be vacuous.
    Window menu2 = None;
    Rect menuRect2;
    const bool reopened =
        openRootMenu(d, driver, kMenuPressX, kMenuPressY, menu2, menuRect2, newBg);
    driver.moveTo(kMenuPressX, menuRect2.y + 14);
    Histogram after;
    WmFixture::pollUntil([&] {
        after = captureRoot(d, menuRect2);
        return countOf(after, newHl) > 0 && countOf(after, newFg) > 0;
    }, 8000);
    const Rect borderRect2{menuRect2.x - 1, menuRect2.y - 1, menuRect2.w + 2, menuRect2.h + 2};
    const Histogram borderAfter = captureRoot(d, borderRect2);
    closeRootMenu(d, driver);

    const std::string stderrText = fixture.wmStderr();
    INFO("wm stderr:\n" << stderrText);
    INFO("menu-background: " << setBg.describe());
    INFO("menu-foreground: " << setFg.describe());
    INFO("menu-highlight:  " << setHl.describe());
    INFO("menu-borders:    " << setBord.describe());
    INFO("menu after: " << describeTop(after));
    INFO("border before " << describeTop(borderBefore) << " after " << describeTop(borderAfter));

    CHECK(setBg.exitCode == 0);
    CHECK(setFg.exitCode == 0);
    CHECK(setHl.exitCode == 0);
    CHECK(setBord.exitCode == 0);

    CHECK(reopened);
    CHECK(dominantPixel(after) == newBg);
    CHECK(dominantShare(after) >= kDominanceFloor);
    CHECK(countOf(before, newBg) == 0);
    CHECK(countOf(after, newFg) > 0);
    CHECK(countOf(after, newHl) > 0);
    CHECK(countOf(borderAfter, newBord) > 0);
    CHECK(countOf(borderAfter, oldBord) == 0);
}

TEST_CASE("a colour the X server cannot parse is refused and the screen is unchanged",
          "[wm_config_live]")
{
    // T-9-26. The prohibition this plan carries: a failed resolution must never
    // leave the window manager without a usable colour, which it cannot avoid
    // if the old value was released before the new one was known to succeed.
    const std::string home = makeConfigHome(
        "frame-thickness=7\ntab-background=#c8cacc\n");
    WmFixture fixture(fixtureWithConfigHome(home));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    parkPointer(d);

    Window client = None;
    Window frame = mapClientAndAwaitFrame(d, 260, 180, 320, 240, client, "refusal");
    REQUIRE(frame != None);
    settleWm(d);
    const Rect tabRect = tabColumnRect(rectOf(d, frame), rectOf(d, client), 7);
    REQUIRE(tabRect.w > 4);

    const Histogram before = captureRoot(d, tabRect);

    CtlResult bad     = ctl(fixture, {"set", "tab-background", "#nonsense"});
    CtlResult badMenu = ctl(fixture, {"set", "menu-background", "not a colour at all"});
    settleWm(d);
    const Histogram after = captureRoot(d, tabRect);

    const std::string stderrText = fixture.wmStderr();
    INFO("wm stderr:\n" << stderrText);
    INFO("bad tab-background: " << bad.describe());
    INFO("bad menu-background: " << badMenu.describe());
    INFO("tab before " << describeTop(before) << " after " << describeTop(after));

    CHECK(bad.exitCode == 1);
    CHECK(bad.err.find("tab-background") != std::string::npos);
    CHECK(badMenu.exitCode == 1);
    CHECK(badMenu.err.find("menu-background") != std::string::npos);

    // Nothing moved and nothing was repainted: the window keeps the colour it
    // had, byte for byte.
    CHECK(after == before);
    CHECK(ctlGet(fixture, "tab-background") == "#c8cacc");

    // And the window manager is still framing windows -- a colour path that
    // freed before it allocated would have left it without one.
    Window late = None;
    Window lateFrame = mapClientAndAwaitFrame(d, 600, 400, 200, 160, late, "after-refusal");
    CHECK(lateFrame != None);
    CHECK(fixture.wmAlive());
}

TEST_CASE("setting the same colour twice leaves the capture byte-identical",
          "[wm_config_live]")
{
    // The idempotency guarantee, asserted for a colour rather than for a
    // geometry. There is no counter and no stderr line to read from outside, so
    // the observable is the SCREEN: identical output after a second application
    // is what "no second reallocation" looks like from here.
    const std::string home = makeConfigHome(
        "frame-thickness=7\ntab-background=#c8cacc\n");
    WmFixture fixture(fixtureWithConfigHome(home));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    parkPointer(d);

    Window client = None;
    Window frame = mapClientAndAwaitFrame(d, 260, 180, 320, 240, client, "repeat");
    REQUIRE(frame != None);
    settleWm(d);
    const Rect tabRect = tabColumnRect(rectOf(d, frame), rectOf(d, client), 7);
    REQUIRE(tabRect.w > 4);

    const unsigned long wanted = namedPixel(d, "#ff0000");
    REQUIRE(wanted != ~0UL);

    CtlResult first = ctl(fixture, {"set", "tab-background", "#ff0000"});
    const Histogram afterFirst = awaitPixelIn(d, tabRect, wanted, 100);
    settleWm(d);
    const Histogram settled = captureRoot(d, tabRect);

    CtlResult second = ctl(fixture, {"set", "tab-background", "#ff0000"});
    settleWm(d);
    const Histogram afterSecond = captureRoot(d, tabRect);

    INFO("wm stderr:\n" << fixture.wmStderr());
    INFO("first: "  << first.describe());
    INFO("second: " << second.describe());
    INFO("after first " << describeTop(settled) << " after second " << describeTop(afterSecond));

    CHECK(first.exitCode == 0);
    CHECK(second.exitCode == 0);
    CHECK(dominantPixel(afterFirst) == wanted);
    CHECK(afterSecond == settled);
}


// -----------------------------------------------------------------------------
// Fonts, focus policy, delays and commands (CGUI-04, plan 09-05)
//
// Everything below asserts an OBSERVABLE BEHAVIOUR CHANGE on a desktop that is
// already running: the tab of a window mapped before the command gets wider,
// the pointer stops focusing, the menu's New entry runs something else. Not one
// of them reads a value back and calls that proof -- `get` returning the new
// value would be equally true of a window manager that stored it and never
// looked at it again, which is exactly the class of claim plan 08-07 found to
// be false for three of these very booleans.
// -----------------------------------------------------------------------------

TEST_CASE("setting tab-font re-lays out every tab already on screen",
          "[wm_config_live]")
{
    const std::string home = makeConfigHome(
        "frame-thickness=7\ntab-font=Sans:bold:size=10\n");
    WmFixture fixture(fixtureWithConfigHome(home));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    parkPointer(d);

    // TWO windows, and both are checked. The re-layout has to reach every
    // managed client, not only the active one: a loop over the active client
    // alone would pass a one-window case and leave a real desktop with tabs of
    // two different widths.
    Window firstClient = None, secondClient = None;
    Window firstFrame  = mapClientAndAwaitFrame(d, 120, 120, 260, 200, firstClient, "one");
    Window secondFrame = mapClientAndAwaitFrame(d, 500, 120, 260, 200, secondClient, "two");
    REQUIRE(firstFrame != None);
    REQUIRE(secondFrame != None);
    settleWm(d);

    const int firstBefore  = decorationWidth(d, firstFrame, firstClient);
    const int secondBefore = decorationWidth(d, secondFrame, secondClient);

    CtlResult r = ctl(fixture, {"set", "tab-font", "Sans:bold:size=28"});

    int firstAfter = firstBefore, secondAfter = secondBefore;
    WmFixture::pollUntil([&] {
        pumpWm(d);
        firstAfter  = decorationWidth(d, firstFrame, firstClient);
        secondAfter = decorationWidth(d, secondFrame, secondClient);
        return firstAfter != firstBefore && secondAfter != secondBefore;
    }, 8000);

    const std::string stderrText = fixture.wmStderr();
    INFO("wm stderr:\n" << stderrText);
    INFO(r.describe());
    INFO("first  decoration " << firstBefore  << " -> " << firstAfter);
    INFO("second decoration " << secondBefore << " -> " << secondAfter);

    CHECK(r.exitCode == 0);
    CHECK(firstAfter > firstBefore);
    CHECK(secondAfter > secondBefore);
    // Both moved by the SAME amount: the tab's thickness is shared, so a
    // per-client recomputation that drifted would show up here.
    CHECK(firstAfter - firstBefore == secondAfter - secondBefore);
    CHECK(ctlGet(fixture, "tab-font") == "Sans:bold:size=28");
    CHECK(fixture.wmAlive());
}

TEST_CASE("a tab-font with no usable face is refused and every tab keeps its width",
          "[wm_config_live]")
{
    // WHY A LEVER RATHER THAN A NONSENSE FAMILY NAME. fontconfig SUBSTITUTES
    // for a family it does not have rather than failing, so no string a user
    // can type reliably reaches the bottom of the ladder -- which is exactly
    // what XDIS-04 wants of it, and exactly what makes the failure path
    // untestable from outside. WM2_FORCE_TAB_FONT_RELOAD_FAILURE forces the
    // reload's ladder to yield nothing, in the same shape and for the same
    // reason as WM2_FORCE_NO_TAB_FONT and WM2_FORCE_NO_ROTATED_TAB_FONT: an
    // internal test lever, read once, with no config key and no command-line
    // flag. It is deliberately a DIFFERENT lever from those two, so the window
    // manager still starts with a real face -- there would be nothing to prove
    // "the previous face stays loaded" about otherwise.
    WmFixtureOptions options =
        fixtureWithConfigHome(makeConfigHome("frame-thickness=7\n"));
    options.childEnv["WM2_FORCE_TAB_FONT_RELOAD_FAILURE"] = "1";
    WmFixture fixture(options);

    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    parkPointer(d);

    Window client = None;
    Window frame = mapClientAndAwaitFrame(d, 200, 150, 300, 220, client, "keeps-width");
    REQUIRE(frame != None);
    settleWm(d);

    const int before = decorationWidth(d, frame, client);
    const std::string fontBefore = ctlGet(fixture, "tab-font");

    CtlResult r = ctl(fixture, {"set", "tab-font", "Monospace:size=30"});
    settleWm(d);
    const int after = decorationWidth(d, frame, client);

    const std::string stderrText = fixture.wmStderr();
    INFO("wm stderr:\n" << stderrText);
    INFO(r.describe());
    INFO("decoration " << before << " -> " << after);

    CHECK(r.exitCode == 1);
    CHECK(r.err.find("tab-font") != std::string::npos);

    // The previous face is still loaded: the tab kept its width, and the
    // window manager still reports the value it was actually drawing with.
    CHECK(after == before);
    CHECK(ctlGet(fixture, "tab-font") == fontBefore);

    // And it still frames. A reload that closed the old face before opening the
    // new one would have left it with none.
    Window late = None;
    Window lateFrame = mapClientAndAwaitFrame(d, 560, 400, 200, 160, late, "after-refusal");
    CHECK(lateFrame != None);
    CHECK(fixture.wmAlive());
}

TEST_CASE("setting menu-font changes the next root menu's row height",
          "[wm_config_live]")
{
    const std::string home = makeConfigHome("menu-font=Sans:size=10\n");
    WmFixture fixture(fixtureWithConfigHome(home));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    XTestDriver driver(fixture.display());
    parkPointer(d);

    const unsigned long bg = namedPixel(d, "#c8cacc");
    REQUIRE(bg != ~0UL);

    Window menu = None;
    Rect before;
    REQUIRE(openRootMenu(d, driver, kMenuPressX, kMenuPressY, menu, before, bg));
    closeRootMenu(d, driver);

    CtlResult r = ctl(fixture, {"set", "menu-font", "Sans:size=22"});

    Window menu2 = None;
    Rect after;
    const bool reopened =
        openRootMenu(d, driver, kMenuPressX, kMenuPressY, menu2, after, bg);
    closeRootMenu(d, driver);

    const std::string stderrText = fixture.wmStderr();
    INFO("wm stderr:\n" << stderrText);
    INFO(r.describe());
    INFO("menu before " << describe(before) << " after " << describe(after));

    CHECK(r.exitCode == 0);
    CHECK(reopened);
    // WindowManager::menu() derives the row height from the face's ascent and
    // descent and the popup's height from the row height, so a taller face is a
    // taller menu with the same number of entries.
    CHECK(after.h > before.h);
    CHECK(fixture.wmAlive());
}

TEST_CASE("setting click-to-focus stops the pointer alone from focusing",
          "[wm_config_live]")
{
    const std::string home = makeConfigHome(
        "click-to-focus=false\nauto-raise=true\nauto-raise-delay=50\n");
    WmFixture fixture(fixtureWithConfigHome(home));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    XTestDriver driver(fixture.display());
    driver.moveTo(kParkX, kParkY);

    // The CONTROL, on the same running window manager: with the file's
    // click-to-focus=false the pointer alone focuses. Without this half the
    // negative below could pass on a window manager whose pointer focus never
    // worked at all.
    Window first = None;
    Window firstFrame = mapUnfocusedClient(d, 160, 140, 280, 200, first, "pointer");
    REQUIRE(firstFrame != None);
    REQUIRE(pumpedActiveWindow(d) != first);

    const Rect firstRect = rectOf(d, first);
    driver.moveTo(firstRect.x + firstRect.w / 2, firstRect.y + firstRect.h / 2);
    const bool focusedByPointer =
        WmFixture::pollUntil([&] { return pumpedActiveWindow(d) == first; }, 8000);
    INFO("wm stderr:\n" << fixture.wmStderr());
    REQUIRE(focusedByPointer);

    driver.moveTo(kParkX, kParkY);
    settleWm(d);

    // --- the flip, on the running desktop -----------------------------------
    CtlResult r = ctl(fixture, {"set", "click-to-focus", "true"});

    Window second = None;
    Window secondFrame = mapUnfocusedClient(d, 520, 340, 280, 200, second, "click");
    REQUIRE(secondFrame != None);
    REQUIRE(pumpedActiveWindow(d) != second);

    const Rect secondRect = rectOf(d, second);
    driver.moveTo(secondRect.x + secondRect.w / 2, secondRect.y + secondRect.h / 2);
    waitPastFocusDelays();
    settleWm(d);
    const Window afterEnter = activeWindow(d);

    // ...and a CLICK on the same spot still does focus it, so what changed is
    // the route and not the window manager's ability to focus anything.
    driver.press(Button1);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    driver.release(Button1);
    const bool focusedByClick =
        WmFixture::pollUntil([&] { return pumpedActiveWindow(d) == second; }, 8000);

    INFO("wm stderr:\n" << fixture.wmStderr());
    INFO(r.describe());
    INFO("active after pointer entry: " << afterEnter << " second: " << second);

    CHECK(r.exitCode == 0);
    CHECK(afterEnter != second);
    CHECK(focusedByClick);
}

TEST_CASE("setting raise-on-focus false focuses a window without restacking it",
          "[wm_config_live]")
{
    const std::string home = makeConfigHome(
        "click-to-focus=false\nauto-raise=true\nauto-raise-delay=50\n"
        "raise-on-focus=true\n");
    WmFixture fixture(fixtureWithConfigHome(home));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    XTestDriver driver(fixture.display());
    driver.moveTo(kParkX, kParkY);

    Window a = None, b = None;
    Window frameA = mapUnfocusedClient(d, 140, 140, 260, 190, a, "a");
    Window frameB = mapUnfocusedClient(d, 540, 360, 260, 190, b, "b");
    REQUIRE(frameA != None);
    REQUIRE(frameB != None);
    settleWm(d);

    const Rect rectA = rectOf(d, a);
    const Rect rectB = rectOf(d, b);

    // CONTROL: with the file's raise-on-focus=true, focusing A lifts it above B.
    driver.moveTo(rectA.x + rectA.w / 2, rectA.y + rectA.h / 2);
    const bool raisedA = WmFixture::pollUntil([&] {
        return pumpedActiveWindow(d) == a &&
               pumpedStackIndex(d, frameA) > pumpedStackIndex(d, frameB);
    }, 8000);
    INFO("wm stderr:\n" << fixture.wmStderr());
    REQUIRE(raisedA);

    driver.moveTo(kParkX, kParkY);
    settleWm(d);

    // --- the flip -----------------------------------------------------------
    CtlResult r = ctl(fixture, {"set", "raise-on-focus", "false"});

    const int aBefore = pumpedStackIndex(d, frameA);
    const int bBefore = pumpedStackIndex(d, frameB);
    REQUIRE(bBefore < aBefore);

    driver.moveTo(rectB.x + rectB.w / 2, rectB.y + rectB.h / 2);
    const bool focusedB =
        WmFixture::pollUntil([&] { return pumpedActiveWindow(d) == b; }, 8000);

    // Waited out past the delays, so a raise that was merely slower than the
    // focus cannot slip past the assertion below.
    waitPastFocusDelays();
    settleWm(d);
    const int aAfter = stackIndex(d, frameA);
    const int bAfter = stackIndex(d, frameB);

    INFO("wm stderr:\n" << fixture.wmStderr());
    INFO(r.describe());
    INFO("stack before a=" << aBefore << " b=" << bBefore
         << "  after a=" << aAfter << " b=" << bAfter);

    CHECK(r.exitCode == 0);
    CHECK(focusedB);
    CHECK(bAfter < aAfter);
}

TEST_CASE("setting auto-raise false stops the pointer consulting focus at all",
          "[wm_config_live]")
{
    const std::string home = makeConfigHome(
        "click-to-focus=false\nauto-raise=true\nauto-raise-delay=50\n");
    WmFixture fixture(fixtureWithConfigHome(home));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    XTestDriver driver(fixture.display());
    driver.moveTo(kParkX, kParkY);

    Window a = None, b = None;
    Window frameA = mapUnfocusedClient(d, 140, 140, 260, 190, a, "a");
    Window frameB = mapUnfocusedClient(d, 540, 360, 260, 190, b, "b");
    REQUIRE(frameA != None);
    REQUIRE(frameB != None);
    const Rect rectA = rectOf(d, a);
    const Rect rectB = rectOf(d, b);

    // CONTROL: auto-raise on, so pointer entry arms the deadline and focus
    // follows.
    driver.moveTo(rectA.x + rectA.w / 2, rectA.y + rectA.h / 2);
    const bool focusedA =
        WmFixture::pollUntil([&] { return pumpedActiveWindow(d) == a; }, 8000);
    INFO("wm stderr:\n" << fixture.wmStderr());
    REQUIRE(focusedA);

    driver.moveTo(kParkX, kParkY);
    settleWm(d);

    CtlResult r = ctl(fixture, {"set", "auto-raise", "false"});

    driver.moveTo(rectB.x + rectB.w / 2, rectB.y + rectB.h / 2);
    waitPastFocusDelays();
    settleWm(d);
    const Window afterEnter = activeWindow(d);

    INFO("wm stderr:\n" << fixture.wmStderr());
    INFO(r.describe());
    INFO("active after entering b: " << afterEnter << "  b: " << b << "  a: " << a);

    CHECK(r.exitCode == 0);
    // Nothing consults the pointer any more, so B never becomes active.
    CHECK(afterEnter != b);
}

TEST_CASE("setting focus-stealing-prevention false grants focus to a window that asked not to have it",
          "[wm_config_live]")
{
    const std::string home = makeConfigHome("focus-stealing-prevention=true\n");
    WmFixture fixture(fixtureWithConfigHome(home));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    parkPointer(d);

    // A user-time of zero is the EWMH's explicit "do not focus me on map", so
    // with prevention on it is refused -- mapped and framed, just not focused.
    Window refused = None;
    Window refusedFrame = mapUnfocusedClient(d, 160, 140, 280, 200, refused, "refused");
    REQUIRE(refusedFrame != None);
    settleWm(d);
    const Window afterRefused = activeWindow(d);

    CtlResult r = ctl(fixture, {"set", "focus-stealing-prevention", "false"});

    Window granted = None;
    Window grantedFrame = mapUnfocusedClient(d, 520, 340, 280, 200, granted, "granted");
    REQUIRE(grantedFrame != None);
    const bool focused =
        WmFixture::pollUntil([&] { return pumpedActiveWindow(d) == granted; }, 8000);

    INFO("wm stderr:\n" << fixture.wmStderr());
    INFO(r.describe());
    INFO("active after the refused map: " << afterRefused << " (client " << refused << ")");
    INFO("active after the granted map: " << activeWindow(d) << " (client " << granted << ")");

    CHECK(r.exitCode == 0);
    CHECK(afterRefused != refused);
    CHECK(focused);
}

TEST_CASE("setting auto-raise-delay changes how long the pointer must rest",
          "[wm_config_live]")
{
    // BOTH delays start at the parser's maximum, and only ONE of them is set
    // during the case. The other stays at a minute throughout, so it cannot be
    // what made the focus happen -- which is the whole difficulty with a timing
    // assertion and the reason the two delay cases are shaped this way.
    const std::string home = makeConfigHome(
        "click-to-focus=false\nauto-raise=true\n"
        "auto-raise-delay=60000\npointer-stopped-delay=60000\n");
    WmFixture fixture(fixtureWithConfigHome(home));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    XTestDriver driver(fixture.display());
    driver.moveTo(kParkX, kParkY);

    // TWO windows, and the second entry is a FIRST entry into a window that has
    // never been a focus candidate. That is not decoration: this is the branch
    // reached only when NO MotionNotify has been seen, and once a window has
    // been tracked once the window manager keeps motion selected on it, so
    // re-entering the same window delivers a motion event and hands the
    // decision to the pointer-stopped branch instead -- which the sibling case
    // below is about. MEASURED: written as a park-and-re-enter, this case never
    // focused at all.
    Window slow = None, fast = None;
    Window slowFrame = mapUnfocusedClient(d, 160, 140, 280, 200, slow, "slow");
    Window fastFrame = mapUnfocusedClient(d, 540, 360, 280, 200, fast, "fast");
    REQUIRE(slowFrame != None);
    REQUIRE(fastFrame != None);
    const Rect slowRect = rectOf(d, slow);
    const Rect fastRect = rectOf(d, fast);

    // A minute's delay: entering does not focus within the non-event wait.
    driver.moveTo(slowRect.x + slowRect.w / 2, slowRect.y + slowRect.h / 2);
    waitPastFocusDelays();
    settleWm(d);
    const Window afterSlow = activeWindow(d);

    CtlResult r = ctl(fixture, {"set", "auto-raise-delay", "50"});

    driver.moveTo(fastRect.x + fastRect.w / 2, fastRect.y + fastRect.h / 2);
    const bool focusedFast =
        WmFixture::pollUntil([&] { return pumpedActiveWindow(d) == fast; }, 8000);

    INFO("wm stderr:\n" << fixture.wmStderr());
    INFO(r.describe());
    INFO("active after the slow entry: " << afterSlow << " slow: " << slow);

    CHECK(r.exitCode == 0);
    CHECK(afterSlow != slow);
    CHECK(focusedFast);
}

TEST_CASE("setting pointer-stopped-delay changes how long stillness must last",
          "[wm_config_live]")
{
    // The sibling of the case above, and the same isolation: auto-raise-delay
    // stays at a minute for the whole case, so only the pointer-stopped branch
    // can produce a focus change. That branch is the one taken once a
    // MotionNotify has been seen INSIDE the candidate window, which is why each
    // entry below is two moves rather than one.
    const std::string home = makeConfigHome(
        "click-to-focus=false\nauto-raise=true\n"
        "auto-raise-delay=60000\npointer-stopped-delay=60000\n");
    WmFixture fixture(fixtureWithConfigHome(home));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    XTestDriver driver(fixture.display());
    driver.moveTo(kParkX, kParkY);

    Window client = None;
    Window frame = mapUnfocusedClient(d, 200, 150, 300, 220, client, "stopped");
    REQUIRE(frame != None);
    const Rect rect = rectOf(d, client);
    const int cx = rect.x + rect.w / 2;
    const int cy = rect.y + rect.h / 2;

    driver.moveTo(cx, cy);
    settleTick();
    driver.moveTo(cx + 8, cy + 8);
    waitPastFocusDelays();
    settleWm(d);
    const Window afterSlow = activeWindow(d);

    driver.moveTo(kParkX, kParkY);
    settleWm(d);

    CtlResult r = ctl(fixture, {"set", "pointer-stopped-delay", "20"});

    driver.moveTo(cx, cy);
    settleTick();
    driver.moveTo(cx + 8, cy + 8);
    const bool focusedFast =
        WmFixture::pollUntil([&] { return pumpedActiveWindow(d) == client; }, 8000);

    INFO("wm stderr:\n" << fixture.wmStderr());
    INFO(r.describe());
    INFO("active after the slow entry: " << afterSlow << " client: " << client);

    CHECK(r.exitCode == 0);
    CHECK(afterSlow != client);
    CHECK(focusedFast);
}

TEST_CASE("setting destroy-window-delay changes what a held tab button does",
          "[wm_config_live]")
{
    // The tab button HIDES on a short press and DELETES on a long one, and the
    // threshold is the setting. The window advertises WM_DELETE_WINDOW, so a
    // delete arrives here as a client message this connection can observe --
    // and, crucially, the window manager never reaches XKillClient, which would
    // take down the test's own X connection along with the window.
    const std::string home = makeConfigHome(
        "frame-thickness=7\ndestroy-window-delay=60000\n");
    WmFixture fixture(fixtureWithConfigHome(home));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    XTestDriver driver(fixture.display());
    parkPointer(d);

    const Atom wmProtocols = XInternAtom(d, "WM_PROTOCOLS", False);
    const Atom wmDelete    = XInternAtom(d, "WM_DELETE_WINDOW", False);

    auto mapDeletableClient = [&](int x, int y, const char* name,
                                  Window& clientOut) -> Window {
        Window root = DefaultRootWindow(d);
        Window win = XCreateSimpleWindow(d, root, x, y, 260, 200, 0,
                                         BlackPixel(d, DefaultScreen(d)),
                                         WhitePixel(d, DefaultScreen(d)));
        XStoreName(d, win, name);
        Atom protocols[1] = {wmDelete};
        XSetWMProtocols(d, win, protocols, 1);
        XSelectInput(d, win, StructureNotifyMask);
        clientOut = win;
        XMapWindow(d, win);
        XSync(d, False);
        return awaitFrameFor(d, win);
    };

    // Drain and report whether a WM_DELETE_WINDOW message arrived for `win`.
    auto sawDelete = [&](Window win) {
        bool seen = false;
        while (XPending(d)) {
            XEvent e;
            XNextEvent(d, &e);
            if (e.type == ClientMessage && e.xclient.window == win &&
                e.xclient.message_type == wmProtocols &&
                static_cast<Atom>(e.xclient.data.l[0]) == wmDelete) {
                seen = true;
            }
        }
        return seen;
    };

    // The button's target square is the tab's whole top square, which sits at
    // the frame's own origin.
    auto holdTabButton = [&](Window frame, int ms) {
        const Rect f = rectOf(d, frame);
        driver.moveTo(f.x + 5, f.y + 5);
        settleTick();
        driver.press(Button1);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        driver.release(Button1);
        XSync(d, False);
    };

    Window first = None;
    Window firstFrame = mapDeletableClient(160, 140, "hide-me", first);
    REQUIRE(firstFrame != None);
    settleWm(d);
    while (XPending(d)) { XEvent e; XNextEvent(d, &e); }

    // A minute's threshold: a 500 ms hold is a HIDE, so no delete message.
    holdTabButton(firstFrame, 500);
    settleWm(d);
    const bool deletedWithLongThreshold = sawDelete(first);

    CtlResult r = ctl(fixture, {"set", "destroy-window-delay", "100"});

    Window second = None;
    Window secondFrame = mapDeletableClient(540, 360, "delete-me", second);
    REQUIRE(secondFrame != None);
    settleWm(d);
    while (XPending(d)) { XEvent e; XNextEvent(d, &e); }

    holdTabButton(secondFrame, 500);
    bool deletedWithShortThreshold = false;
    WmFixture::pollUntil([&] {
        pumpWm(d);
        if (sawDelete(second)) deletedWithShortThreshold = true;
        return deletedWithShortThreshold;
    }, 8000);

    INFO("wm stderr:\n" << fixture.wmStderr());
    INFO(r.describe());

    CHECK(r.exitCode == 0);
    CHECK_FALSE(deletedWithLongThreshold);
    CHECK(deletedWithShortThreshold);
}

TEST_CASE("setting new-window-command and exec-using-shell changes what the menu's New entry runs",
          "[wm_config_live]")
{
    // The observable is a FILE ON DISK the spawned process creates. Nothing
    // about the window manager's internal state is read: an assertion that
    // `get new-window-command` returns the new value would be equally true of a
    // build that stored it and never executed anything.
    const std::string home = makeConfigHome("new-window-command=/bin/true\n");
    WmFixture fixture(fixtureWithConfigHome(home));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    XTestDriver driver(fixture.display());
    parkPointer(d);

    const std::string directWitness = home + "/direct-witness";
    const std::string shellWitness  = home + "/shell-witness";
    const std::string script        = home + "/spawn-witness.sh";
    {
        std::ofstream out(script);
        out << "#!/bin/sh\nexec /usr/bin/touch " << directWitness << "\n";
    }
    REQUIRE(::chmod(script.c_str(), 0700) == 0);

    const unsigned long bg = namedPixel(d, "#c8cacc");
    auto chooseNew = [&]() {
        Window menu = None;
        Rect menuRect;
        if (!openRootMenu(d, driver, kMenuPressX, kMenuPressY, menu, menuRect, bg)) {
            driver.release(Button1);
            return false;
        }
        selectFirstMenuEntry(d, driver, menuRect);
        settleWm(d);
        return true;
    };
    auto witnessAppeared = [](const std::string& path) {
        return WmFixture::pollUntil([&] {
            return ::access(path.c_str(), F_OK) == 0;
        }, 8000);
    };

    // --- new-window-command, run directly (no shell) -------------------------
    CtlResult setCommand = ctl(fixture, {"set", "new-window-command", script});
    REQUIRE(chooseNew());
    const bool directRan = witnessAppeared(directWitness);

    // --- exec-using-shell: a command with an ARGUMENT, which execlp cannot run
    //
    // "touch <path>" is not the name of any executable, so with the shell flag
    // off the spawn fails and no witness appears; with it on, /bin/sh parses
    // the same string into a command and an argument and it runs. That is the
    // flag's whole meaning, and the pair of observations is what separates it
    // from a value merely being stored.
    CtlResult setShellCommand =
        ctl(fixture, {"set", "new-window-command", "/usr/bin/touch " + shellWitness});
    REQUIRE(chooseNew());
    const bool ranWithoutShell = ::access(shellWitness.c_str(), F_OK) == 0;

    CtlResult setShell = ctl(fixture, {"set", "exec-using-shell", "true"});
    REQUIRE(chooseNew());
    const bool ranWithShell = witnessAppeared(shellWitness);

    INFO("wm stderr:\n" << fixture.wmStderr());
    INFO("set new-window-command: " << setCommand.describe());
    INFO("set shell command: " << setShellCommand.describe());
    INFO("set exec-using-shell: " << setShell.describe());
    INFO("direct witness: " << directWitness);
    INFO("shell witness: " << shellWitness);

    CHECK(setCommand.exitCode == 0);
    CHECK(setShellCommand.exitCode == 0);
    CHECK(setShell.exitCode == 0);
    CHECK(directRan);
    CHECK_FALSE(ranWithoutShell);
    CHECK(ranWithShell);
}


// -----------------------------------------------------------------------------
// The root menu rebuilds, and every connected client learns the file changed
// (CGUI-04, D-08, D-12, plan 09-05)
// -----------------------------------------------------------------------------

// WHAT THE ROOT MENU ACTUALLY SHOWS, and why the two cases below assert on a
// CATEGORY label rather than on an entry name.
//
// WindowManager::menu()'s outer rows are: the New entry, one row per hidden
// client, then one row per CATEGORY -- `m_appCategories[i].first`, the category
// name. An entry's own name is only ever drawn in that category's submenu,
// which opens on a MotionNotify over its row and whose row index cannot be
// computed from outside without knowing the entry height and how many
// categories the host's application scan produced.
//
// So "the manual entry is present in the next root menu" is observed here as
// the row the entry brings INTO that menu: a manual entry in a category nothing
// else populates adds exactly one row, of exactly that label's width. That is a
// property of the entry -- remove the entry and the row goes with it, which is
// the second case below -- and it is measurable without a submenu.

TEST_CASE("a manual menu entry set over the socket appears in the next root menu",
          "[wm_config_live]")
{
    const std::string home = makeConfigHome("frame-thickness=7\n");
    WmFixture fixture(fixtureWithConfigHome(home));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    XTestDriver driver(fixture.display());
    parkPointer(d);

    const unsigned long bg = namedPixel(d, "#c8cacc");
    REQUIRE(bg != ~0UL);

    // No manual entries at all to begin with, so what the menu gains is
    // entirely the entry this case adds.
    const std::string before = ctlGet(fixture, "menu-entries");
    INFO("menu-entries before: '" << before << "'");
    REQUIRE(before.empty());

    Window menu = None;
    Rect firstRect;
    REQUIRE(openRootMenu(d, driver, kMenuPressX, kMenuPressY, menu, firstRect, bg));
    closeRootMenu(d, driver);

    CtlResult r = ctl(fixture, {"set", "menu-entries",
        "menu-entry-name=SetOverTheSocket;"
        "menu-entry-command=/bin/true;"
        "menu-entry-category=ZzzAnExtremelyLongCategoryLabelIndeed"});

    Window menu2 = None;
    Rect secondRect;
    const bool reopened =
        openRootMenu(d, driver, kMenuPressX, kMenuPressY, menu2, secondRect, bg);
    closeRootMenu(d, driver);

    const std::string after = ctlGet(fixture, "menu-entries");

    const std::string stderrText = fixture.wmStderr();
    INFO("wm stderr:\n" << stderrText);
    INFO(r.describe());
    INFO("menu-entries after: " << after);
    INFO("menu before " << describe(firstRect) << " after " << describe(secondRect));

    CHECK(r.exitCode == 0);
    CHECK(reopened);

    // The window manager reports the list it was given, rendered in the same
    // grammar the request used -- so the two ends are checked against each
    // other rather than against a literal written twice.
    CHECK(after == "menu-entry-name=SetOverTheSocket;"
                   "menu-entry-command=/bin/true;"
                   "menu-entry-category=ZzzAnExtremelyLongCategoryLabelIndeed");

    // ...and the menu the USER sees changed: one row taller for the row the
    // entry brought with it, and wider because WindowManager::menu() measures
    // every label and sizes the popup to the widest one.
    CHECK(secondRect.h > firstRect.h);
    CHECK(secondRect.w > firstRect.w);
}

TEST_CASE("removing every manual entry removes its category from the next menu",
          "[wm_config_live]")
{
    // The entry's category is one nothing else populates, so emptying the list
    // must take the whole category row out of the menu with it -- which is a
    // narrower menu, because that row is the widest label in it.
    const std::string home = makeConfigHome(
        "menu-entry-name=SoleEntry\n"
        "menu-entry-command=/bin/true\n"
        "menu-entry-category=ZzzUniqueCategoryNameThatNothingElseUses\n");
    WmFixture fixture(fixtureWithConfigHome(home));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    XTestDriver driver(fixture.display());
    parkPointer(d);

    const unsigned long bg = namedPixel(d, "#c8cacc");

    Window menu = None;
    Rect withCategory;
    REQUIRE(openRootMenu(d, driver, kMenuPressX, kMenuPressY, menu, withCategory, bg));
    closeRootMenu(d, driver);

    // The empty value is how a list is cleared.
    CtlResult r = ctl(fixture, {"set", "menu-entries", ""});

    Window menu2 = None;
    Rect withoutCategory;
    const bool reopened =
        openRootMenu(d, driver, kMenuPressX, kMenuPressY, menu2, withoutCategory, bg);
    closeRootMenu(d, driver);

    INFO("wm stderr:\n" << fixture.wmStderr());
    INFO(r.describe());
    INFO("menu-entries after: '" << ctlGet(fixture, "menu-entries") << "'");
    INFO("menu with category " << describe(withCategory)
         << " without " << describe(withoutCategory));

    CHECK(r.exitCode == 0);
    CHECK(reopened);
    CHECK(ctlGet(fixture, "menu-entries").empty());
    // One fewer row, and the widest label gone with it.
    CHECK(withoutCategory.h < withCategory.h);
    CHECK(withoutCategory.w < withCategory.w);
}

TEST_CASE("a menu held open across a menu-entry change is not disturbed",
          "[wm_config_live]")
{
    // T-9-32. WindowManager::menu() runs a modal loop holding a pointer INTO
    // the category list, so rebuilding that list underneath it is the defect
    // class 08-13 and 08-14 already fixed once. The rebuild is deferred to the
    // next opening instead, and this case is what says so from outside: the
    // open menu keeps its geometry, the window manager keeps answering, and the
    // change appears the next time the menu is opened.
    // The change made mid-loop REMOVES a category rather than adding one, and
    // that direction is chosen deliberately. menu() reads the category count
    // into a local before it enters its loop and indexes m_appCategories with
    // it on every repaint, so a list that GREW underneath it is merely stale
    // while a list that SHRANK is an out-of-bounds read. Testing the dangerous
    // direction is the point of testing at all.
    const std::string home = makeConfigHome(
        "menu-entry-name=OpenAcrossTheChange\n"
        "menu-entry-command=/bin/true\n"
        "menu-entry-category=ZzzAnExtremelyLongCategoryLabelIndeed\n");
    WmFixture fixture(fixtureWithConfigHome(home));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    XTestDriver driver(fixture.display());
    parkPointer(d);

    const unsigned long bg = namedPixel(d, "#c8cacc");

    Window menu = None;
    Rect openRect;
    REQUIRE(openRootMenu(d, driver, kMenuPressX, kMenuPressY, menu, openRect, bg));

    // A CATEGORY FLYOUT IS OPENED FIRST, and that is what gives this case its
    // teeth. The modal loop keeps a pointer INTO m_appCategories -- the entry
    // vector the flyout is drawn from -- for exactly as long as one is open, so
    // it is only with a flyout up that rebuilding the list underneath the loop
    // is a use-after-free rather than merely untidy. Without this the case
    // stays green with the deferral removed, which was MEASURED before it was
    // added.
    const Window submenu = openAnySubmenu(d, driver, menu, openRect);
    Rect submenuBefore;
    const bool haveSubmenu = submenu != None && serverRect(d, submenu, submenuBefore);
    INFO("submenu: " << submenu << " " << (haveSubmenu ? describe(submenuBefore) : "none"));

    // The menu is UP and the button is still held. The socket answers anyway --
    // 09-03's shared descriptor set is what makes that true even inside a
    // modal grab.
    CtlResult r = ctl(fixture, {"set", "menu-entries", ""});
    settleTick();
    settleTick();

    // MOVE INSIDE THE FLYOUT AFTER THE CHANGE. A dangling pointer is only a
    // fault when it is followed, and the modal loop follows this one when it
    // repaints a row -- which a selection change is what causes. Without this
    // motion the list can be swapped underneath the loop and nothing ever
    // reads it again, so the case would pass on a window manager that had just
    // freed the vector it is drawing from. MEASURED: without it, removing the
    // deferral left this case green.
    if (haveSubmenu) {
        driver.moveTo(submenuBefore.x + submenuBefore.w / 2, submenuBefore.y + 14);
        settleTick();
        driver.moveTo(submenuBefore.x + submenuBefore.w / 2, submenuBefore.y + 34);
        settleTick();
        settleTick();
    }

    Rect stillOpen;
    const bool measured = serverRect(d, menu, stillOpen);
    Rect submenuAfter{};
    const bool submenuMeasured = haveSubmenu && serverRect(d, submenu, submenuAfter);
    closeRootMenu(d, driver);

    Window menu2 = None;
    Rect afterRect;
    const bool reopened =
        openRootMenu(d, driver, kMenuPressX, kMenuPressY, menu2, afterRect, bg);
    closeRootMenu(d, driver);

    INFO("wm stderr:\n" << fixture.wmStderr());
    INFO(r.describe());
    INFO("menu while open " << describe(openRect) << " -> " << describe(stillOpen)
         << ", reopened " << describe(afterRect));

    CHECK(r.exitCode == 0);
    // The open menu was not resized, remeasured or redrawn under the loop.
    CHECK(measured);
    CHECK(stillOpen == openRect);
    // The flyout the loop was drawing from is where the danger is, so it is
    // asserted separately from the outer menu rather than folded into it.
    CHECK(haveSubmenu);
    CHECK(submenuMeasured);
    CHECK(submenuAfter == submenuBefore);
    // ...and the next one picked the change up: one fewer category row, and
    // the widest label gone with it.
    CHECK(reopened);
    CHECK(afterRect.h < openRect.h);
    CHECK(afterRect.w < openRect.w);

    // Still responsive: it frames a window afterwards.
    Window late = None;
    Window lateFrame = mapClientAndAwaitFrame(d, 500, 400, 200, 160, late, "after-menu");
    CHECK(lateFrame != None);
    CHECK(fixture.wmAlive());
}

TEST_CASE("a reload tells every client that completed a hello, and no stranger",
          "[wm_config_live]")
{
    // D-08. Two connections: one that completed the handshake and one that
    // connected and said nothing. Exactly the first is told.
    const std::string home = makeConfigHome("frame-thickness=7\n");
    WmFixture fixture(fixtureWithConfigHome(home));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);

    const std::string socketPath = socketPathFor(fixture.display());
    INFO("socket: " << socketPath);

    Conn greeted(socketPath);
    REQUIRE(greeted.connected());
    REQUIRE(greeted.shakeHands());

    Conn stranger(socketPath);
    REQUIRE(stranger.connected());

    // The file changes under the running window manager, then a reload is asked
    // for through the shipped tool -- a THIRD connection, so neither of the two
    // above is the one that requested it.
    writeConfigFile(home, "frame-thickness=23\n");
    CtlResult r = ctl(fixture, {"reload"});

    std::string noticeLine;
    const bool greetedHeard = greeted.readLine(noticeLine, 8000);

    ConfigMessage notice;
    ConfigDecodeResult decoded = ConfigDecodeResult::Malformed;
    if (greetedHeard) decoded = configProtocolDecode(noticeLine, notice);

    // A shorter deadline than the handshake one, because this is proving a
    // NON-event; a stranger is also dropped on the hello deadline, and either
    // way it never receives a line.
    std::string strangerLine;
    const bool strangerHeard = stranger.readLine(strangerLine, 1500);

    const std::string stderrText = fixture.wmStderr();
    INFO("wm stderr:\n" << stderrText);
    INFO(r.describe());
    INFO("greeted heard: " << greetedHeard << " '" << noticeLine << "'");
    INFO("stranger heard: " << strangerHeard << " '" << strangerLine << "'");

    CHECK(r.exitCode == 0);
    CHECK(greetedHeard);
    CHECK(decoded == ConfigDecodeResult::Ok);
    CHECK(notice.type == ConfigMessageType::Reloaded);

    // T-9-31: the notice carries its type and NOTHING else. A client that wants
    // a value asks for it, so the notice can never become a second, drifting
    // copy of the settings.
    CHECK(notice.key.empty());
    CHECK(notice.value.empty());
    CHECK(notice.reason.empty());
    CHECK(notice.program.empty());
    CHECK(notice.protocol == 0);
    CHECK(notice.fields.empty());

    // D-15: a connection that never said hello is not a client, and is told
    // nothing.
    CHECK_FALSE(strangerHeard);
}

TEST_CASE("a client that never reads does not stop the window manager reloading",
          "[wm_config_live]")
{
    // T-9-30. The broadcast writes to descriptors whose far end this process
    // does not control. A connection that completes the handshake and then
    // never reads a byte must not be able to block the window manager: the
    // proof of that is that it keeps working afterwards.
    const std::string home = makeConfigHome("frame-thickness=7\n");
    WmFixture fixture(fixtureWithConfigHome(home));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();

    Conn idle(socketPathFor(fixture.display()));
    REQUIRE(idle.connected());
    REQUIRE(idle.shakeHands());

    Window client = None;
    Window frame = mapClientAndAwaitFrame(d, 200, 150, 300, 220, client, "unread");
    REQUIRE(frame != None);
    const Rect before = rectOf(d, frame);

    // Several reloads in a row, each producing a notice nobody on that
    // connection is collecting.
    for (int i = 0; i < 5; ++i) {
        writeConfigFile(home, "frame-thickness=" + std::to_string(9 + i * 2) + "\n");
        CtlResult r = ctl(fixture, {"reload"});
        INFO("reload " << i << ": " << r.describe());
        CHECK(r.exitCode == 0);
    }

    const Rect after = awaitFrameChange(d, frame, before);

    INFO("wm stderr:\n" << fixture.wmStderr());
    INFO("frame before " << describe(before) << " after " << describe(after));

    CHECK_FALSE(after == before);
    CHECK(ctlGet(fixture, "frame-thickness") == "17");
    CHECK(fixture.wmAlive());
}

TEST_CASE("a malformed menu-entries value is refused and the menu is unchanged",
          "[wm_config_live]")
{
    const std::string home = makeConfigHome(
        "menu-entry-name=KeepMe\nmenu-entry-command=/bin/true\n");
    WmFixture fixture(fixtureWithConfigHome(home));

    const std::string before = ctlGet(fixture, "menu-entries");
    REQUIRE(before.find("KeepMe") != std::string::npos);

    // A record that is not a key=value pair; a key that is not one of the
    // three; and a command with no name to attach to. The FILE parser answers
    // the last of those with a warning to a stderr nobody is reading, which
    // over the socket would acknowledge a list that silently lost a record.
    CtlResult noEquals = ctl(fixture, {"set", "menu-entries", "menu-entry-name"});
    CtlResult wrongKey = ctl(fixture, {"set", "menu-entries", "frame-thickness=9"});
    CtlResult orphan   = ctl(fixture, {"set", "menu-entries",
                                       "menu-entry-command=/bin/true"});

    const std::string after = ctlGet(fixture, "menu-entries");

    INFO("wm stderr:\n" << fixture.wmStderr());
    INFO("no equals: " << noEquals.describe());
    INFO("wrong key: " << wrongKey.describe());
    INFO("orphan: " << orphan.describe());
    INFO("menu-entries before '" << before << "' after '" << after << "'");

    CHECK(noEquals.exitCode == 1);
    CHECK(wrongKey.exitCode == 1);
    CHECK(orphan.exitCode == 1);
    CHECK(after == before);
}

TEST_CASE("setting the same menu-entries value twice changes nothing the second time",
          "[wm_config_live]")
{
    const std::string home = makeConfigHome("frame-thickness=7\n");
    WmFixture fixture(fixtureWithConfigHome(home));

    const std::string value =
        "menu-entry-name=Twice;menu-entry-command=/bin/true;menu-entry-category=Custom";

    CtlResult first  = ctl(fixture, {"set", "menu-entries", value});
    const std::string afterFirst = ctlGet(fixture, "menu-entries");
    CtlResult second = ctl(fixture, {"set", "menu-entries", value});
    const std::string afterSecond = ctlGet(fixture, "menu-entries");

    INFO("wm stderr:\n" << fixture.wmStderr());
    INFO("first: " << first.describe());
    INFO("second: " << second.describe());
    INFO("after first '" << afterFirst << "' after second '" << afterSecond << "'");

    CHECK(first.exitCode == 0);
    CHECK(second.exitCode == 0);
    // Wholesale replacement is what makes this idempotent: a second identical
    // list is the same list, never the list twice over.
    CHECK(afterFirst == value);
    CHECK(afterSecond == afterFirst);
}


// ---------------------------------------------------------------------------
// The two font swaps commit together or not at all (CR-04)
// ---------------------------------------------------------------------------
//
// applyConfig()'s own comment argued the two-stage swap was safe because "a
// `set` names ONE key, so at most one of these two branches ever runs". A
// RELOAD applies a whole Config from disk, and a file that changes both fonts
// runs both -- so a tab face that opened followed by a menu face that did not
// left the shared tab face and the tab geometry constant swapped while
// m_config was never updated. `get tab-font` then reported a pattern nothing on
// screen was drawn with, and no `set` could repair it: setting the old value
// back computes tabFontChanged == false and never relayouts.
//
// WM2_FORCE_MENU_FONT_RELOAD_FAILURE is the sibling of the tab lever above and
// exists for the same reason: fontconfig substitutes for a family it does not
// have rather than failing, so no string a user can type reaches the bottom of
// either ladder.

TEST_CASE("a reload whose menu-font has no usable face changes neither font",
          "[wm_config_live]")
{
    const std::string home = makeConfigHome(
        "tab-font=Sans:bold:size=12\n"
        "menu-font=Sans:size=10\n");
    WmFixtureOptions options = fixtureWithConfigHome(home);
    options.childEnv["WM2_FORCE_MENU_FONT_RELOAD_FAILURE"] = "1";
    WmFixture fixture(options);

    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    parkPointer(d);

    Window client = None;
    Window frame = mapClientAndAwaitFrame(d, 200, 150, 300, 220, client, "both-fonts");
    REQUIRE(frame != None);
    settleWm(d);

    const int widthBefore = decorationWidth(d, frame, client);
    const std::string tabBefore = ctlGet(fixture, "tab-font");
    const std::string menuBefore = ctlGet(fixture, "menu-font");
    REQUIRE(tabBefore == "Sans:bold:size=12");

    // BOTH fonts in one edit, which is the whole scenario: a size the tab
    // ladder will happily open, and a menu pattern that will not.
    writeConfigFile(home,
                    "tab-font=Sans:bold:size=28\n"
                    "menu-font=Sans:size=22\n");
    CtlResult r = ctl(fixture, {"reload"});
    settleWm(d);

    const int widthAfter = decorationWidth(d, frame, client);
    const std::string stderrText = fixture.wmStderr();
    INFO("wm stderr:\n" << stderrText);
    INFO(r.describe());
    INFO("decoration " << widthBefore << " -> " << widthAfter);

    // The reload is refused whole.
    CHECK(r.exitCode == 1);
    CHECK(r.err.find("menu-font") != std::string::npos);

    // And "refused whole" is asserted on the STATE, not on the exit code.
    CHECK(ctlGet(fixture, "tab-font") == tabBefore);
    CHECK(ctlGet(fixture, "menu-font") == menuBefore);

    // The frames already open keep their width either way -- the early return
    // is upstream of the relayout, so an existing frame is not where the
    // half-applied state shows. WHERE IT SHOWS is m_tabWidth: it is the shared
    // constant every NEW frame is built from, and Border::reloadTabFont()
    // recomputes it as part of the swap. So the question that catches CR-04 is
    // "does a window mapped after the refusal wear the same tab as one mapped
    // before it?" -- which is also the question a user asks when half their
    // windows have thick tabs and `get tab-font` insists nothing changed.
    Window late = None;
    Window lateFrame = mapClientAndAwaitFrame(d, 560, 400, 200, 160, late, "after-refusal");
    REQUIRE(lateFrame != None);
    settleWm(d);
    const int lateWidth = decorationWidth(d, lateFrame, late);

    INFO("existing decoration " << widthAfter << ", new decoration " << lateWidth);
    CHECK(widthAfter == widthBefore);
    CHECK(lateWidth == widthBefore);

    // Not merely unchanged but still WORKING: a swap that closed the old face
    // before the refusal would leave the next frame with none.
    CHECK(fixture.wmAlive());
}

TEST_CASE("the same reload with a usable menu-font changes both fonts",
          "[wm_config_live]")
{
    // The other side of the refusal above, and the reason it is a separate
    // case: a guard that refused every two-font reload would satisfy the
    // assertions there and break the feature.
    const std::string home = makeConfigHome(
        "tab-font=Sans:bold:size=12\n"
        "menu-font=Sans:size=10\n");
    WmFixture fixture(fixtureWithConfigHome(home));

    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    parkPointer(d);

    Window client = None;
    Window frame = mapClientAndAwaitFrame(d, 200, 150, 300, 220, client, "both-fonts-ok");
    REQUIRE(frame != None);
    settleWm(d);

    const int widthBefore = decorationWidth(d, frame, client);

    writeConfigFile(home,
                    "tab-font=Sans:bold:size=28\n"
                    "menu-font=Sans:size=22\n");
    CtlResult r = ctl(fixture, {"reload"});

    int widthAfter = widthBefore;
    WmFixture::pollUntil([&] {
        pumpWm(d);
        widthAfter = decorationWidth(d, frame, client);
        return widthAfter != widthBefore;
    }, 8000);

    const std::string stderrText = fixture.wmStderr();
    INFO("wm stderr:\n" << stderrText);
    INFO(r.describe());
    INFO("decoration " << widthBefore << " -> " << widthAfter);

    CHECK(r.exitCode == 0);
    CHECK(ctlGet(fixture, "tab-font") == "Sans:bold:size=28");
    CHECK(ctlGet(fixture, "menu-font") == "Sans:size=22");
    CHECK(widthAfter > widthBefore);
    CHECK(fixture.wmAlive());
}

TEST_CASE("a reload refused for its font leaves the palette it also carried "
          "entirely alone",
          "[wm_config_live]")
{
    // THE COMMIT ORDER, asserted on the screen. A reload applies a whole file,
    // so one edit can carry a colour AND a font -- and the palette used to be
    // reloaded (allocated AND swapped) before either face was opened. A face
    // that would not open then returned false with m_config untouched, having
    // already moved the shared palette every frame is built from: `get
    // tab-background` named the old colour and the next window to open wore the
    // new one. Sticky, too, in the same way CR-04's font defect was: setting
    // the old colour back computes coloursChanged == false and reloads nothing.
    //
    // The forced font failure is the same internal lever the two cases above
    // use, and for the same reason -- fontconfig substitutes rather than fails,
    // so no pattern a user can type reaches the bottom of the ladder.
    const std::string home = makeConfigHome(
        "frame-thickness=7\n"
        "tab-font=Sans:bold:size=12\n"
        "tab-background=#c8cacc\n");
    WmFixtureOptions options = fixtureWithConfigHome(home);
    options.childEnv["WM2_FORCE_TAB_FONT_RELOAD_FAILURE"] = "1";
    WmFixture fixture(options);

    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    parkPointer(d);

    Window client = None;
    Window frame = mapClientAndAwaitFrame(d, 200, 150, 320, 240, client,
                                          "palette-before-font");
    REQUIRE(frame != None);
    settleWm(d);

    const int kThickness = 7;                       // the file's value
    const Rect band = tabBandRect(rectOf(d, frame), rectOf(d, client), kThickness);

    const unsigned long oldBg = namedPixel(d, "#c8cacc");
    const unsigned long newBg = namedPixel(d, "#ff0000");
    REQUIRE(oldBg != ~0UL);
    REQUIRE(newBg != ~0UL);

    const Histogram bandBefore = captureRoot(d, band);
    INFO("band before: " << describeTop(bandBefore));
    REQUIRE(dominantPixel(bandBefore) == oldBg);

    // ONE edit carrying both: a colour the server parses happily, and a font
    // the lever makes unopenable.
    writeConfigFile(home,
                    "frame-thickness=7\n"
                    "tab-font=Sans:bold:size=28\n"
                    "tab-background=#ff0000\n");
    CtlResult r = ctl(fixture, {"reload"});
    settleWm(d);

    const Histogram bandAfter = captureRoot(d, band);

    // THE NEW FRAME is where a committed palette shows, exactly as the new
    // frame is where a committed tab width showed for CR-04: every window
    // built after the refusal reads the shared pixels the refused reload left
    // behind.
    Window late = None;
    Window lateFrame = mapClientAndAwaitFrame(d, 560, 380, 240, 180, late,
                                              "after-refusal");
    REQUIRE(lateFrame != None);
    settleWm(d);
    const Rect lateBandRect =
        tabBandRect(rectOf(d, lateFrame), rectOf(d, late), kThickness);
    const Histogram lateBand = captureRoot(d, lateBandRect);

    const std::string stderrText = fixture.wmStderr();
    INFO("wm stderr:\n" << stderrText);
    INFO(r.describe());
    INFO("band after: " << describeTop(bandAfter));
    INFO("late band:  " << describeTop(lateBand));

    // The reload is refused whole, and says which key refused it.
    CHECK(r.exitCode == 1);
    CHECK(r.err.find("tab-font") != std::string::npos);

    // The window manager still reports the old colour...
    CHECK(ctlGet(fixture, "tab-background") == "#c8cacc");

    // ...and so does the screen, for the frame that was already open...
    CHECK(dominantPixel(bandAfter) == oldBg);
    CHECK(countOf(bandAfter, newBg) == 0);

    // ...and for the one opened afterwards, which is the assertion that fails
    // when the palette was committed before the font was validated.
    CHECK(dominantPixel(lateBand) == oldBg);
    CHECK(countOf(lateBand, newBg) == 0);

    CHECK(fixture.wmAlive());
}


// ---------------------------------------------------------------------------
// A live apply does not move geometry a modal grab has already cached (WR-13)
// ---------------------------------------------------------------------------

TEST_CASE("a geometry setting is refused while the root menu is open, and "
          "applies the moment it closes",
          "[wm_config_live]")
{
    // DISC-06 services the configuration socket from modalWait() on purpose,
    // so a `set` really is applied while the root menu is held open. That is
    // the feature -- and it is also how a menu ends up painting labels at the
    // new face's baselines in rows measured from the old one, with the
    // pointer highlighting a different row from the one it activates:
    // WindowManager::menu() computes its entry height ONCE, from m_menuFont.
    const std::string home = makeConfigHome(
        "menu-font=Sans:size=10\n"
        "frame-thickness=7\n");
    WmFixture fixture(fixtureWithConfigHome(home));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    XTestDriver driver(fixture.display());
    parkPointer(d);

    const unsigned long bg = namedPixel(d, "#c8cacc");
    REQUIRE(bg != ~0UL);

    Window menu = None;
    Rect menuRect;
    REQUIRE(openRootMenu(d, driver, kMenuPressX, kMenuPressY, menu, menuRect, bg));

    // The grab is held right now. Each of the three settings that move
    // geometry a grab has cached is refused, and says why.
    CtlResult menuFont = ctl(fixture, {"set", "menu-font", "Sans:size=20"});
    CtlResult tabFont  = ctl(fixture, {"set", "tab-font", "Sans:bold:size=20"});
    CtlResult thick    = ctl(fixture, {"set", "frame-thickness", "17"});

    // A setting that moves nothing a grab cached still applies instantly under
    // one -- the guard is narrow on purpose, and D-06's "nothing waits" holds
    // for everything else.
    CtlResult delay = ctl(fixture, {"set", "auto-raise-delay", "600"});

    closeRootMenu(d, driver);

    const std::string stderrText = fixture.wmStderr();
    INFO("wm stderr:\n" << stderrText);
    INFO("menu-font "       << menuFont.describe());
    INFO("tab-font "        << tabFont.describe());
    INFO("frame-thickness " << thick.describe());
    INFO("auto-raise-delay " << delay.describe());

    CHECK(menuFont.exitCode == 1);
    CHECK(tabFont.exitCode == 1);
    CHECK(thick.exitCode == 1);
    CHECK(menuFont.err.find("menu or a drag") != std::string::npos);

    // Refused WHOLE: the reported values are the ones the grab was drawn with.
    CHECK(ctlGet(fixture, "menu-font") == "Sans:size=10");
    CHECK(ctlGet(fixture, "frame-thickness") == "7");

    CHECK(delay.exitCode == 0);
    CHECK(ctlGet(fixture, "auto-raise-delay") == "600");

    // And "not now" really did mean not now: with the grab gone, the same
    // request succeeds. A guard that refused for ever would satisfy every
    // assertion above and break CGUI-04.
    CtlResult again = ctl(fixture, {"set", "frame-thickness", "17"});
    INFO("after the menu closed: " << again.describe());
    CHECK(again.exitCode == 0);
    CHECK(ctlGet(fixture, "frame-thickness") == "17");

    CHECK(fixture.wmAlive());
}

// -----------------------------------------------------------------------------
// The frame that was not there when the change arrived (PR review, P2)
// -----------------------------------------------------------------------------

TEST_CASE("a client that was fullscreen while the configuration moved comes back "
          "wearing the new one", "[wm_config_live]")
{
    // A FULLSCREEN client has had its frame STRIPPED: every frame component is
    // unmapped and the client window itself is a direct child of root. The
    // three live-change entry points on Client therefore have nothing to lay
    // out at the moment the change arrives, and each returns early.
    //
    // What they must NOT do is drop the change. Border::restoreFromFullscreen()
    // reparents and re-configures the PARENT at the current indents, so the
    // client comes back at the right offset -- but it re-runs no geometry for
    // the tab, the button or the resize grabber, no shape, and no background:
    // those keep whatever the thickness and the palette were when the window
    // went fullscreen. This case is the round trip, with a thickness change and
    // a colour change applied while there was no frame to apply them to.
    const std::string home = makeConfigHome(
        "frame-thickness=7\n"
        "tab-foreground=#ff00ff\n"
        "tab-background=#c8cacc\n"
        "frame-background=#dcdee0\n"
        "button-background=#dcdee0\n"
        "borders=#ff8000\n");
    WmFixture fixture(fixtureWithConfigHome(home));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    parkPointer(d);

    // A long run of narrow glyphs, for the same reason the palette case gives:
    // drawLabel() draws nothing at all for a window with no name.
    Window client = None;
    Window frame = mapClientAndAwaitFrame(d, 260, 180, 320, 240, client,
                                          "IIIIIIIIIIIIIIIIIIII");
    REQUIRE(frame != None);
    settleWm(d);

    const Window tab = findFrameChild(d, frame, client, false);
    REQUIRE(tab != None);
    const Window grabber = findResizeHandle(d, client);
    REQUIRE(grabber != None);

    const Rect frameBefore  = rectOf(d, frame);
    const Rect clientBefore = rectOf(d, client);
    Rect tabBefore, grabberBefore;
    REQUIRE(localRect(d, tab, tabBefore));
    REQUIRE(localRect(d, grabber, grabberBefore));

    // The two readings this case turns into assertions later, taken now so the
    // "it moved" half is against a measured starting point rather than an
    // assumed one. Both follow from the file's frame-thickness of 7:
    // Border::configure() sizes the tab w + xIndent() wide and creates the
    // grabber FRAME_WIDTH*2 square.
    const int xIndentBefore = clientBefore.x - frameBefore.x;
    REQUIRE(tabBefore.w == clientBefore.w + xIndentBefore);
    REQUIRE(grabberBefore.w == 7 * 2);

    const unsigned long newTabBg = namedPixel(d, "#ff0000");
    REQUIRE(newTabBg != ~0UL);
    const Rect bandBeforeRect = tabBandRect(frameBefore, clientBefore, 7);
    const Histogram bandBefore = captureRoot(d, bandBeforeRect);
    INFO("tab band before: " << describeTop(bandBefore));
    REQUIRE(countOf(bandBefore, newTabBg) == 0);

    // --- Fullscreen, then the two changes, then back ------------------------
    sendFullscreen(d, client, true);
    REQUIRE(awaitClientSize(d, client, kScreenW, kScreenH));

    CtlResult thickness = ctl(fixture, {"set", "frame-thickness", "15"});
    CtlResult colour    = ctl(fixture, {"set", "tab-background", "#ff0000"});

    sendFullscreen(d, client, false);
    REQUIRE(awaitClientSize(d, client, clientBefore.w, clientBefore.h));
    settleWm(d);

    const Rect frameAfter  = rectOf(d, frame);
    const Rect clientAfter = rectOf(d, client);
    Rect tabAfter, grabberAfter;
    const bool haveTab     = localRect(d, tab, tabAfter);
    const bool haveGrabber = localRect(d, grabber, grabberAfter);
    const int xIndentAfter = clientAfter.x - frameAfter.x;
    const int yIndentAfter = clientAfter.y - frameAfter.y;

    const Rect bandAfterRect = tabBandRect(frameAfter, clientAfter, 15);
    const Histogram bandAfter = awaitPixelIn(d, bandAfterRect, newTabBg, 20);

    const std::string stderrText = fixture.wmStderr();
    INFO("wm stderr:\n" << stderrText);
    INFO("frame-thickness " << thickness.describe());
    INFO("tab-background  " << colour.describe());
    INFO("frame  before " << describe(frameBefore)  << " after " << describe(frameAfter));
    INFO("client before " << describe(clientBefore) << " after " << describe(clientAfter));
    INFO("tab    before " << describe(tabBefore)    << " after " << describe(tabAfter));
    INFO("grabber before " << describe(grabberBefore) << " after " << describe(grabberAfter));
    INFO("x indent before " << xIndentBefore << " after " << xIndentAfter);
    INFO("tab band before " << describeTop(bandBefore)
         << " after " << describeTop(bandAfter));

    CHECK(thickness.exitCode == 0);
    CHECK(colour.exitCode == 0);
    CHECK(ctlGet(fixture, "frame-thickness") == "15");
    CHECK(ctlGet(fixture, "tab-background") == "#ff0000");

    // The user's own window is back where and how big it was: a thickness
    // change moves decoration, and a fullscreen round trip moves nothing at
    // all.
    CHECK(clientAfter.w == clientBefore.w);
    CHECK(clientAfter.h == clientBefore.h);

    // (a) THE INSET IS THE NEW THICKNESS. yIndent() is FRAME_WIDTH + 1 and
    // xIndent() is that plus the tab's width, so the vertical inset reads the
    // thickness directly and the horizontal one moved by exactly the same
    // amount -- the tab's width is a property of the FONT, which this case
    // never touches.
    REQUIRE(haveTab);
    REQUIRE(haveGrabber);
    CHECK(yIndentAfter == 15 + 1);
    CHECK(xIndentAfter == xIndentBefore + (15 - 7));

    // ...and the frame's own components agree with that inset, which is the
    // half restoreFromFullscreen() does not do: the tab is w + xIndent() wide
    // and the grabber is FRAME_WIDTH*2 square, both of them re-laid out from
    // the thickness in force NOW.
    CHECK(tabAfter.w == clientAfter.w + xIndentAfter);
    CHECK(grabberAfter.w == 15 * 2);
    CHECK(grabberAfter.h == 15 * 2);

    // (b) AND THE PALETTE. The tab's top band is painted by the SERVER from
    // the tab window's background pixel, so this asserts the restored frame
    // was re-backgrounded and cleared, not merely that a label was redrawn.
    CHECK(dominantPixel(bandAfter) == newTabBg);
    CHECK(dominantShare(bandAfter) >= kDominanceFloor);

    CHECK(fixture.wmAlive());
}


// -----------------------------------------------------------------------------
// A THICKNESS AND A TAB FONT IN ONE APPLICATION (CodeRabbit F3, 2026-09-07)
// -----------------------------------------------------------------------------

TEST_CASE("a thickness and a tab font applied together leave a tab that was "
          "already open looking like one opened afterwards", "[wm_config_live]")
{
    // THE DEFECT. applyConfig() had two branches. The thickness branch walked
    // both client lists with relayoutFrame(), and the tab-font branch was
    // SKIPPED whenever the thickness had also moved -- on the reasoning that
    // the first branch had already re-laid every frame out. But
    // Border::relayoutForFrameThickness() deliberately does not repaint the
    // label (a thickness change does not alter the FACE), and only
    // relayoutForTabFont() adds that repaint. So a reload carrying BOTH gave
    // every open window the new thickness wearing the OLD glyphs, until
    // something else happened to expose the tab.
    //
    // THE OBSERVABLE IS SELF-CALIBRATING, which is what makes it a claim about
    // the live path rather than about font metrics this test would otherwise
    // have to predict. A second window is mapped AFTER the change: it is framed
    // from scratch, so its tab is what the new configuration is SUPPOSED to
    // look like. The window that was already open must carry the same amount of
    // ink -- same title, same size, same face, same tab width. Nothing here
    // needs to know how wide a glyph is at either size.
    //
    // tab-foreground is a colour nothing else in the frame uses, so counting it
    // counts the LABEL and not the bevel, the border or the background; and it
    // is held FIXED across the reload, so the colour branch of applyConfig()
    // does no work and cannot repaint the tab on this case's behalf.
    const std::string home = makeConfigHome(
        "frame-thickness=15\n"
        "tab-font=Sans:bold:size=28\n"
        "tab-foreground=#ff00ff\n"
        "tab-background=#c8cacc\n"
        "frame-background=#dcdee0\n"
        "button-background=#dcdee0\n"
        "borders=#ff8000\n");
    WmFixture fixture(fixtureWithConfigHome(home));
    x11::DisplayPtr dp = fixture.openDisplay();
    REQUIRE(dp != nullptr);
    Display* d = dp.get();
    parkPointer(d);

    // A long run of narrow glyphs, for the reason the palette cases give:
    // Border::drawLabel() draws nothing at all for a window with no name, and
    // the foreground colour would never reach a pixel.
    const char* kTitle = "IIIIIIIIIIIIIIIIIIII";

    Window openedBefore = None;
    Window frameBefore = mapClientAndAwaitFrame(d, 140, 200, 300, 400,
                                                openedBefore, kTitle);
    REQUIRE(frameBefore != None);
    settleWm(d);

    const unsigned long ink = namedPixel(d, "#ff00ff");
    REQUIRE(ink != ~0UL);

    const int kThickBefore = 15;
    const int kThickAfter  = 7;

    const Rect fBefore = rectOf(d, frameBefore);
    const Rect cBefore = rectOf(d, openedBefore);
    const Histogram beforeHist =
        captureRoot(d, tabColumnRect(fBefore, cBefore, kThickBefore, cBefore.h));
    const long inkBefore = countOf(beforeHist, ink);

    // Without this the "it was repainted" assertions could pass on a tab that
    // never had any ink on it.
    INFO("tab before: " << describeTop(beforeHist));
    REQUIRE(inkBefore > 0);

    const int decorationBefore = decorationWidth(d, frameBefore, openedBefore);

    // WHY THIS CASE IS GREEN AT THE ROUND BASE, MEASURED RATHER THAN ASSERTED.
    // ExposureMask is not an exclusive selection, so the tab the window manager
    // owns can be watched from here without disturbing it. The reshape that the
    // thickness path performs moves the stair-stepped diagonal of the tab's
    // clip region, which puts regions of the tab back inside it -- and every
    // one of those is an Expose the window manager answers with
    // Border::expose() -> drawLabel(), in the NEW face. So on this X server the
    // missing repaint is painted over by the server's own accounting within a
    // single turn of the event loop, and no capture taken after a settle can
    // see the stale label. The count is printed on failure so a reader can tell
    // that is still what is happening.
    //
    // The defect is therefore guarded in TWO places: here, behaviourally, that
    // the combined application produces a correct tab at all; and in
    // tests/test_wm_socket.cpp, at the source, that the label repaint is
    // performed by the window manager rather than left to an Expose it does not
    // control. The second is the one that was red. A VNC server with backing
    // store -- this project's stated deployment -- restores a shrinking window's
    // contents instead of asking for them back, and generates no Expose to be
    // rescued by.
    const Window probeTab = findFrameChild(d, frameBefore, openedBefore, false);
    REQUIRE(probeTab != None);
    XSelectInput(d, probeTab, ExposureMask | StructureNotifyMask);
    XSync(d, False);
    int exposeCount = 0, configureCount = 0;

    // --- BOTH KEYS, ONE APPLICATION -----------------------------------------
    //
    // A reload rather than two `set` messages, because two `set`s are two
    // applyConfig() calls and each would take its own branch: the defect is
    // reachable only when both diffs are true at the SAME time.
    writeConfigFile(home,
        "frame-thickness=7\n"
        "tab-font=Sans:bold:size=10\n"
        "tab-foreground=#ff00ff\n"
        "tab-background=#c8cacc\n"
        "frame-background=#dcdee0\n"
        "button-background=#dcdee0\n"
        "borders=#ff8000\n");
    CtlResult reloaded = ctl(fixture, {"reload"});

    // The geometry moved, so the reload really was applied to the open window.
    int decorationAfter = decorationBefore;
    WmFixture::pollUntil([&] {
        pumpWm(d);
        decorationAfter = decorationWidth(d, frameBefore, openedBefore);
        return decorationAfter != decorationBefore;
    }, 8000);
    settleWm(d);
    {
        XEvent pev;
        while (XCheckWindowEvent(d, probeTab, ExposureMask | StructureNotifyMask, &pev)) {
            if (pev.type == Expose) ++exposeCount;
            if (pev.type == ConfigureNotify) ++configureCount;
        }
    }

    // --- THE REFERENCE, framed from scratch under the new configuration -----
    Window openedAfter = None;
    Window frameAfter = mapClientAndAwaitFrame(d, 600, 200, 300, 400,
                                               openedAfter, kTitle);
    REQUIRE(frameAfter != None);
    settleWm(d);

    const Rect fOld = rectOf(d, frameBefore);
    const Rect cOld = rectOf(d, openedBefore);
    const Rect fNew = rectOf(d, frameAfter);
    const Rect cNew = rectOf(d, openedAfter);

    const Rect oldColumn = tabColumnRect(fOld, cOld, kThickAfter, cOld.h);
    const Rect newColumn = tabColumnRect(fNew, cNew, kThickAfter, cNew.h);

    const Histogram oldHist = captureRoot(d, oldColumn);
    const Histogram newHist = captureRoot(d, newColumn);
    const long inkOld = countOf(oldHist, ink);
    const long inkNew = countOf(newHist, ink);

    const std::string stderrText = fixture.wmStderr();
    INFO("wm stderr:\n" << stderrText);
    INFO(reloaded.describe());
    INFO("decoration " << decorationBefore << " -> " << decorationAfter);
    INFO("old column " << describe(oldColumn) << " new column " << describe(newColumn));
    INFO("ink: before " << inkBefore << ", already-open " << inkOld
         << ", opened-afterwards " << inkNew);
    INFO("tab Expose events " << exposeCount << ", ConfigureNotify " << configureCount);
    INFO("already-open tab: " << describeTop(oldHist));
    INFO("opened-after tab: " << describeTop(newHist));

    CHECK(reloaded.exitCode == 0);
    CHECK(ctlGet(fixture, "frame-thickness") == "7");
    CHECK(ctlGet(fixture, "tab-font") == "Sans:bold:size=10");

    // The two columns are the same rectangle, so the comparison below is
    // between like and like rather than between two different amounts of tab.
    CHECK(oldColumn.w == newColumn.w);
    CHECK(oldColumn.h == newColumn.h);

    // The reference really has a label on it.
    CHECK(inkNew > 0);

    // THE CLAIM. The window that was already open wears the new face, so it
    // carries the same ink as the one framed from scratch under it.
    CHECK(inkOld == inkNew);

    // ...and it is no longer wearing the old one. Stated separately so a
    // failure says WHICH way it went: a stale 28-point label leaves more ink
    // behind than a fresh 10-point one puts down.
    CHECK(inkOld < inkBefore);

    CHECK(fixture.wmAlive());
}
