#include "Border.h"
#include "Client.h"
#include "Manager.h"
#include <X11/Xft/Xft.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// Static member definitions (degenerate initializations -- don't change)
int FRAME_WIDTH = 7;  // Default, overwritten in constructor from config
int Border::m_tabWidth = -1;
int Border::m_tabBaseline = -1;
XftFont *Border::m_tabFont = nullptr;
Border::TabFontRung Border::m_tabFontRung = Border::TabFontRung::NoFont;
bool Border::m_staticsInitialised = false;
x11::GCPtr Border::m_drawGC;
x11::GCPtr Border::m_bevelLightGC;
x11::GCPtr Border::m_bevelShadowGC;
unsigned long Border::m_frameBackgroundPixel = 0;
unsigned long Border::m_buttonBackgroundPixel = 0;
unsigned long Border::m_borderPixel = 0;
int Border::m_borderCount = 0;
XftColor Border::m_xftForeground = {};
XftColor Border::m_xftBackground = {};
bool Border::m_xftColorsAllocated = false;


Border::Border(Client *client, Window child)
    : m_client(client)
    , m_parent(root())
    , m_tab(None)
    , m_child(child)
    , m_button(None)
    , m_resize(None)
    , m_prevW(-1)
    , m_prevH(-1)
    , m_tabHeight(-1)
{
    if (!m_staticsInitialised) {
        m_staticsInitialised = true;

        // Initialize FRAME_WIDTH from config (runtime, replaces constexpr)
        FRAME_WIDTH = windowManager()->config().frameThickness;

        // XDIS-04: the tab font is loaded through a degradation ladder that
        // cannot terminate the process. See loadTabFont().
        loadTabFont();

        m_frameBackgroundPixel = windowManager()->allocateColour(windowManager()->config().frameBackground.c_str(), "frame background");
        m_buttonBackgroundPixel = windowManager()->allocateColour(windowManager()->config().buttonBackground.c_str(), "button background");
        m_borderPixel = windowManager()->allocateColour(windowManager()->config().borders.c_str(), "border");

        // Allocate Xft colors for tab label rendering
        allocateXftColors();

        // Retain m_drawGC for button fill rectangle (not text)
        XGCValues values;
        values.foreground = windowManager()->allocateColour(windowManager()->config().tabForeground.c_str(), "tab foreground");
        values.background = windowManager()->allocateColour(windowManager()->config().tabBackground.c_str(), "tab background");
        values.function = GXcopy;
        values.line_width = 0;
        values.subwindow_mode = IncludeInferiors;

        m_drawGC = x11::make_gc(display(), root(),
            GCForeground | GCBackground | GCFunction | GCLineWidth | GCSubwindowMode,
            &values);

        if (!m_drawGC) {
            windowManager()->fatal("couldn't allocate border GC");
        }

        // Bevel GCs (plan 08.5-02). The shades are DERIVED from the configured
        // tab background, so a user who sets a dark palette gets shades that
        // belong to it rather than a fixed near-white line that would read as a
        // rendering fault. The fractions reproduce the shipped silver's
        // #F2F4F6 / #898C8F against a #C8CACC body.
        //
        // AS OF THE FLAT LOOK (quick task 261004-vp6) THESE SHADES NO LONGER
        // REACH A PIXEL. They are still allocated here and still re-derived
        // when the tab background changes on a running desktop, because the
        // design handoff kept them rather than ripping them out -- but the two
        // draw functions that consumed them draw nothing now. Nothing
        // observable from outside depends on this derivation any more; see the
        // comment above Border::drawBevel's definition.
        const char *tabBg = windowManager()->config().tabBackground.c_str();
        const unsigned long lightPixel =
            windowManager()->allocateShadeOf(tabBg,  0.76, "bevel highlight");
        const unsigned long shadowPixel =
            windowManager()->allocateShadeOf(tabBg, -0.315, "bevel shadow");

        // A zero pixel means the allocation failed and the GC stays null, which
        // was always harmless -- it meant no raised edge, and under the flat
        // look there is none to miss. Decoration must not be able to stop the
        // window manager starting.
        if (lightPixel != 0) {
            XGCValues bv;
            bv.foreground = lightPixel;
            bv.line_width = 0;
            bv.function = GXcopy;
            bv.subwindow_mode = IncludeInferiors;
            m_bevelLightGC = x11::make_gc(display(), root(),
                GCForeground | GCLineWidth | GCFunction | GCSubwindowMode, &bv);
        }
        if (shadowPixel != 0) {
            XGCValues bv;
            bv.foreground = shadowPixel;
            bv.line_width = 0;
            bv.function = GXcopy;
            bv.subwindow_mode = IncludeInferiors;
            m_bevelShadowGC = x11::make_gc(display(), root(),
                GCForeground | GCLineWidth | GCFunction | GCSubwindowMode, &bv);
        }
    }

    ++m_borderCount;
}


Border::~Border()
{
    // The per-instance XftDraw is released FIRST, before the windows below.
    //
    // It is created bound to m_tab (see drawLabel), and XftDrawDestroy frees the
    // RENDER Picture it holds for that drawable. Destroying m_tab first destroys
    // the Picture along with it, so the subsequent free names an id the server no
    // longer knows -- `RenderBadPicture (invalid Picture parameter)` on stderr for
    // every single managed window that is ever closed. Found by the destroy
    // lifecycle cases in tests/test_wm_lifecycle.cpp (plan 08-11) and reproduced
    // against a bare WM with one client, so it is not a test artefact.
    m_tabDraw.reset();  // destroy per-instance XftDraw (Pitfall 2)

    if (m_parent != root()) {
        if (!m_parent) {
            std::fprintf(stderr, "wm2: zero parent in Border::~Border\n");
        } else {
            XDestroyWindow(display(), m_tab);
            XDestroyWindow(display(), m_button);
            XDestroyWindow(display(), m_parent);
            XDestroyWindow(display(), m_resize);
        }
    }

    if (--m_borderCount == 0) {
        m_drawGC.reset();
        // Released with the other statics rather than leaked for the process
        // lifetime; either may already be null when the colormap was full.
        m_bevelLightGC.reset();
        m_bevelShadowGC.reset();

        // Null is a legitimate outcome of the ladder's last rung, so the
        // teardown asks rather than assumes.
        if (m_tabFont) {
            XftFontClose(display(), m_tabFont);
            m_tabFont = nullptr;
        }
        m_tabFontRung = TabFontRung::NoFont;

        // Every static the constructor established has now been released, so
        // the next Border must build them again. Without this the block would
        // be skipped forever and the WM would run with a destroyed GC.
        m_staticsInitialised = false;

        if (m_xftColorsAllocated) {
            Display* d = display();
            Visual* visual = DefaultVisual(d, DefaultScreen(d));
            Colormap cmap = DefaultColormap(d, DefaultScreen(d));
            XftColorFree(d, visual, cmap, &m_xftForeground);
            XftColorFree(d, visual, cmap, &m_xftBackground);
            m_xftColorsAllocated = false;
        }
    }
}


// ---------------------------------------------------------------------------
// What the rotated tab strip is measured from (quick task 260906-ldw)
//
// m_tabWidth is the THICKNESS of the strip and m_tabBaseline is the one column
// every label's baseline sits on. Both are properties of the FONT, computed
// once when a face is loaded, never of the title -- a baseline derived from the
// label's own extents slides sideways every time the title changes, which is
// what it used to do.
//
// So the sample has to bound the across-strip glyph box of any title the WM may
// be handed, and that is why it is the whole printable-ASCII repertoire rather
// than a letter or two. MEASURED on this host, rotated face at the shipped
// pattern, size 12, in pixels above and below the baseline:
//
//     "M"                 12 above,  0 below      (no descender at all)
//     "Mg"                12 above,  3 below
//     "gjpqy settings"    13 above,  3 below      (the dot of "i" reaches 13)
//     "Hello"             14 above,  0 below      (the stem of "l" reaches 14)
//     printable ASCII     14 above,  4 below      ("(" is both extremes)
//
// Sizing from "M" is what let every descender run into the frame's black line:
// the strip was built with no room below the baseline whatsoever. Sizing from
// "Mg" would have fixed the descenders and still put the "(" of a title like
// "notes.txt (modified)" hard on the tab's outer edge. The repertoire bounds
// both edges for every ASCII title, and costs one extents call per font load.
// ---------------------------------------------------------------------------

// Clear tab either side of the glyph box, in pixels. The frame-side figure is
// the one the operator asked for after measuring the 09-06 screenshot: five
// pixels of tab between the letter bottoms and the frame line. The outer figure
// is the two pixels the label already had on the ascender side.
static const int kTabOuterClearance = 2;
static const int kTabFrameClearance = 5;

static const char kTabSample[] =
    "!\"#$%&'()*+,-./0123456789:;<=>?@"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
    "abcdefghijklmnopqrstuvwxyz{|}~";
static const int kTabSampleLen = static_cast<int>(sizeof kTabSample - 1);


// ---------------------------------------------------------------------------
// XDIS-04 / XDIS-05: the tab-font degradation ladder
//
// No rung here may terminate the process. Before this existed, a failure to
// produce the rotated tab font took the unrecoverable-initialisation exit path
// -- which on a degraded remote display means the user gets no window manager
// at all rather than one with plainer labels. That is precisely the outcome
// XDIS-04 and XDIS-05 exist to forbid, so the rungs below cover every failure
// with something still usable.
//
// XDIS-04 is satisfied at the FONTCONFIG-FALLBACK level, deliberately: the
// preferred chain already names three families and fontconfig substitutes
// further, the generic sans chain is a second net under it, and an unrotated
// face is a third. Reviving core X server fonts as a fourth would undo Phase 4,
// which removed them and the bundled rotation library by decision, and would
// reintroduce a font model that modern remote servers ship without. The 08-06
// spike ([xft_norender_spike]) measured the reason that trade is safe: with
// XRender absent, libXft renders the rotated face through its core X11 glyph
// path with identical metrics, so a RENDER-less remote server still gets the
// sideways tab.
//
// The two levers read below are internal test levers in the same shape as the
// extension levers in src/Manager.cpp -- read exactly once, never documented
// for users, no config key and no command-line flag. They exist because rungs
// 3 and 4 cannot otherwise be reached on any host where fontconfig resolves a
// font, which would leave them as untested code claiming to be a fallback.
// ---------------------------------------------------------------------------

void Border::loadTabFont()
{
    const char *forceNoFont = std::getenv("WM2_FORCE_NO_TAB_FONT");
    const char *forceNoRotated = std::getenv("WM2_FORCE_NO_ROTATED_TAB_FONT");

    const bool skipEveryRung =
        (forceNoFont != nullptr && std::strcmp(forceNoFont, "1") == 0);
    const bool skipRotatedRungs = skipEveryRung ||
        (forceNoRotated != nullptr && std::strcmp(forceNoRotated, "1") == 0);

    // Worded distinctly from the genuine-degradation lines further down, so a
    // captured transcript can tell "we forced it" apart from "this display
    // could not produce it" -- the same discrimination the Shape and RANDR
    // levers provide.
    if (skipEveryRung) {
        std::fprintf(stderr, "wm2: warning: tab font forced off, "
                             "frames will be drawn without labels\n");
    } else if (skipRotatedRungs) {
        std::fprintf(stderr, "wm2: warning: rotated tab font forced off, "
                             "tab labels will read horizontally\n");
    }

    x11::XftFontPtr font;

    // The PREFERRED pattern, and the only rung the `tab-font` key reaches
    // (plan 09-01). Its default is the literal this rung used to spell inline,
    // so a user with no config file lands on exactly the face they landed on
    // before.
    //
    // The rungs BELOW are deliberately left as literals. A fallback the user can
    // also break is not a fallback: if `tab-font` fed rung 2 or rung 3 as well,
    // one bad value would take out the preferred face and every net under it at
    // once, which is the outcome the XDIS-04 ladder exists to prevent.
    const std::string &preferred = windowManager()->config().tabFont;

    // Rung 1 -- the normal path (D-04 rotation, D-02 preferred chain). Silent
    // on success: this is what every healthy display does.
    if (!skipRotatedRungs) {
        font = x11::make_xft_font_rotated(display(), preferred.c_str());
        if (font) m_tabFontRung = TabFontRung::RotatedPreferred;
    }

    // Rung 2 -- still sideways, but from the generic sans chain.
    if (!font && !skipRotatedRungs) {
        font = x11::make_xft_font_rotated(display(), "sans-serif:bold:size=12");
        if (font) {
            m_tabFontRung = TabFontRung::RotatedGeneric;
            std::fprintf(stderr, "wm2: warning: preferred rotated tab font "
                                 "unavailable, using the generic sans chain\n");
        }
    }

    // Rung 3 -- an unrotated face from the same preferred chain. Labels read
    // horizontally across the tab instead of running down it: degraded, but
    // present and readable.
    if (!font && !skipEveryRung) {
        font = x11::make_xft_font_name(display(), preferred.c_str());
        if (font) {
            m_tabFontRung = TabFontRung::Unrotated;
            std::fprintf(stderr, "wm2: warning: no rotated tab font on this "
                                 "display, tab labels will read horizontally\n");
        }
    }

    // Rung 4 -- no font at all. The WM keeps running with unlabelled tabs.
    if (!font) {
        m_tabFontRung = TabFontRung::NoFont;
        std::fprintf(stderr, "wm2: warning: no usable tab font on this display, "
                             "frames will be drawn without labels\n");
    }

    // Transfer ownership from RAII to the raw static pointer
    // (managed via m_borderCount refcount in the destructor). Releasing a null
    // holder yields a null pointer, which is exactly rung 4's state.
    m_tabFont = font.release();

    if (!hasTabFont()) {
        // Nothing to measure. Fall back to the minimum width the code already
        // derives from the tab-top-height constant, so frames still get a
        // sensibly proportioned tab.
        m_tabWidth = TAB_TOP_HEIGHT * 2 + 8;
        return;
    }

    XGlyphInfo extents;
    if (tabFontRotated()) {
        // Rotated Xft fonts have zero height (Plan 01 finding).
        // Use XftTextExtentsUtf8 to measure the actual glyph extent.
        //
        // AXIS (deferred item 11, fixed in plan 08-14). For a 90-degree rotated
        // face the string runs along XGlyphInfo::height and its THICKNESS is
        // XGlyphInfo::width. MEASURED on this host at size 12:
        //
        //     "M"        width 12   height  13
        //     "Hello"    width 12   height  39
        //     35 chars   width 16   height 286
        //
        // m_tabWidth is the thickness of the tab strip, so it comes from
        // `width`. It previously read `height`, which for a ONE-CHARACTER
        // sample is numerically almost the same (13 against 12) -- which is
        // exactly why this read survived: it is wrong by one pixel and looks
        // right. Its sibling reads, measuring the whole LABEL on the same wrong
        // axis, were wrong by an order of magnitude.
        //
        // BASELINE (quick task 260906-ldw). XGlyphInfo places the ink at
        // [origin.x - x, origin.x - x + width), so for this rotated face `x` is
        // the distance from the draw origin to the ASCENDER edge and
        // `width - x` is the descender depth -- MEASURED, not assumed: "M",
        // which has no descender, comes back with x == width (12 and 12) while
        // "g" comes back with x 9 against width 12. The ascender edge faces
        // away from the frame and the descenders point at it, so the outer
        // clearance is added on the `x` side and the frame clearance beyond the
        // far edge. See the sample's own commentary above.
        XftTextExtentsUtf8(display(), m_tabFont,
            reinterpret_cast<const FcChar8*>(kTabSample), kTabSampleLen, &extents);
        m_tabWidth    = extents.width + kTabOuterClearance + kTabFrameClearance;
        m_tabBaseline = kTabOuterClearance + extents.x;
    } else {
        // Rung 3: an unrotated face has its advance on the other axis, so
        // measuring a glyph the rotated way would size the tab from the
        // string direction instead of the line direction. Use the face's own
        // line height, which is the unrotated equivalent of what the rotated
        // branch above is reaching for.
        //
        // Deliberately NOT sized from a multi-character sample, tempting as
        // that is for readability: m_tabWidth is shared by every frame and by
        // the tab's shape geometry, and a tab several times wider than normal
        // overflows small windows badly. Legibility is bought back by
        // truncation in drawLabelHorizontal(), not by a wider tab.
        const char* sample = "M";
        XftTextExtentsUtf8(display(), m_tabFont,
            reinterpret_cast<const FcChar8*>(sample), 1, &extents);
        m_tabWidth = m_tabFont->ascent + m_tabFont->descent + 4;
        if (m_tabWidth < extents.height + 4) m_tabWidth = extents.height + 4;
    }

    if (m_tabWidth < TAB_TOP_HEIGHT * 2 + 8) {
        m_tabWidth = TAB_TOP_HEIGHT * 2 + 8;
    }
}


// ---------------------------------------------------------------------------
// Live colour reload (CGUI-04, plan 09-05)
//
// ALLOCATE, THEN SWAP -- and the two halves are two FUNCTIONS, which is the
// whole safety property. Every new value -- five pixels, two Xft colours, two
// derived bevel shades and three graphics contexts -- is obtained into a
// staged Palette by openPalette(), and nothing that draws sees any of it until
// installPalette() takes it. A failure at any point therefore leaves the
// window manager drawing with exactly the palette it had, which is what threat
// T-9-26 and this plan's standing prohibition require; freeing first and
// hoping would leave a frame with no colour at all.
//
// The split is not decoration. applyConfig() applies a whole Config, so a
// reload can carry a colour and a font in one edit, and a palette that
// installed itself before the font was opened committed half a refused
// configuration -- see the comment on Border::Palette in include/Border.h.
//
// A note on what is NOT released here. The five pixel values come from
// XAllocNamedColor and are never freed, in this function or anywhere else in
// this codebase -- see WindowManager::allocateColour(), whose results have the
// same lifetime. On the TrueColor visuals every target of this project uses
// (Xvfb, TigerVNC, TightVNC, XRDP and any modern X server) a named-colour
// allocation consumes no colormap cell at all: the pixel is computed from the
// visual's masks, so there is nothing to leak. Freeing them would also be
// wrong as the code stands, because two keys set to the same colour share one
// allocation and a single free would release it for both.
// ---------------------------------------------------------------------------

Border::Palette::~Palette()
{
    // Only what was never handed over. installPalette() clears the flag as it
    // takes the two colours, so an installed palette frees nothing here and an
    // abandoned one frees exactly what it allocated.
    if (xftHeld && display != nullptr) {
        XftColorFree(display, visual, colormap, &background);
        XftColorFree(display, visual, colormap, &foreground);
    }
}


bool Border::openPalette(WindowManager *wm, const Config &next, Palette &out,
                         std::string &keyOut)
{
    // Before the first frame exists the statics block has not run, and it
    // reads whatever is in the config when it does. Nothing to allocate
    // against, and nothing to fail.
    if (!m_staticsInitialised) {
        out.nothingToInstall = true;
        return true;
    }

    Display *d = wm->display();
    Visual *visual = DefaultVisual(d, DefaultScreen(d));
    Colormap cmap  = DefaultColormap(d, DefaultScreen(d));

    // --- allocate: pixels ---------------------------------------------------
    unsigned long framePixel = 0, buttonPixel = 0, borderPixel = 0;
    unsigned long fgPixel = 0, bgPixel = 0;

    const struct { const char *key; const std::string *value; unsigned long *out; }
    wanted[] = {
        {"frame-background",  &next.frameBackground,  &framePixel},
        {"button-background", &next.buttonBackground, &buttonPixel},
        {"borders",           &next.borders,          &borderPixel},
        {"tab-foreground",    &next.tabForeground,    &fgPixel},
        {"tab-background",    &next.tabBackground,    &bgPixel},
    };
    for (const auto &w : wanted) {
        if (!wm->tryAllocateColour(w.value->c_str(), *w.out)) {
            keyOut = w.key;
            return false;
        }
    }

    // --- allocate: the two Xft colours the tab is drawn with -----------------
    //
    // Handed to `out` the moment both are allocated, so from here on the
    // staged palette owns them: every early return below destroys it, and the
    // destructor frees exactly these two.
    XftColor newForeground, newBackground;
    if (!XftColorAllocName(d, visual, cmap, next.tabForeground.c_str(),
                           &newForeground)) {
        keyOut = "tab-foreground";
        return false;
    }
    if (!XftColorAllocName(d, visual, cmap, next.tabBackground.c_str(),
                           &newBackground)) {
        XftColorFree(d, visual, cmap, &newForeground);
        keyOut = "tab-background";
        return false;
    }
    out.display    = d;
    out.visual     = visual;
    out.colormap   = cmap;
    out.foreground = newForeground;
    out.background = newBackground;
    out.xftHeld    = true;

    // --- allocate: the bevel shades, DERIVED from the new tab background -----
    //
    // Re-derived rather than carried over, which was the point of deriving them
    // at all: a user who set a dark palette live got shades that belonged to
    // it, instead of the previous palette's near-white highlight sitting on the
    // new body colour and reading as a rendering fault. The two fractions are
    // the ones the constructor uses, spelled once here and once there because
    // they are the same design decision seen from two entry points.
    //
    // Under the flat look (quick task 261004-vp6) this re-derivation is kept
    // and is no longer observable: nothing draws with the result. It still
    // participates in the allocate-before-release discipline below, so a
    // tab-background value whose shades will not allocate is still refused as
    // a whole rather than half-applied.
    unsigned long lightPixel = 0, shadowPixel = 0;
    if (!wm->tryAllocateShadeOf(next.tabBackground.c_str(), 0.76, lightPixel) ||
        !wm->tryAllocateShadeOf(next.tabBackground.c_str(), -0.315, shadowPixel)) {
        keyOut = "tab-background";
        return false;
    }

    // --- allocate: the graphics contexts ------------------------------------
    x11::GCPtr newDrawGC, newLightGC, newShadowGC;
    {
        XGCValues values;
        values.foreground = fgPixel;
        values.background = bgPixel;
        values.function = GXcopy;
        values.line_width = 0;
        values.subwindow_mode = IncludeInferiors;
        newDrawGC = x11::make_gc(d, wm->root(),
            GCForeground | GCBackground | GCFunction | GCLineWidth | GCSubwindowMode,
            &values);
    }
    if (!newDrawGC) {
        keyOut = "tab-foreground";
        return false;
    }

    // A zero pixel means the shade would not allocate, which leaves the GC
    // null. That was always harmless -- it meant no raised edge, and under the
    // flat look there is none to miss. Not an error: decoration must not be
    // able to refuse a colour change.
    if (lightPixel != 0) {
        XGCValues bv;
        bv.foreground = lightPixel;
        bv.line_width = 0;
        bv.function = GXcopy;
        bv.subwindow_mode = IncludeInferiors;
        newLightGC = x11::make_gc(d, wm->root(),
            GCForeground | GCLineWidth | GCFunction | GCSubwindowMode, &bv);
    }
    if (shadowPixel != 0) {
        XGCValues bv;
        bv.foreground = shadowPixel;
        bv.line_width = 0;
        bv.function = GXcopy;
        bv.subwindow_mode = IncludeInferiors;
        newShadowGC = x11::make_gc(d, wm->root(),
            GCForeground | GCLineWidth | GCFunction | GCSubwindowMode, &bv);
    }

    // --- staged: nothing above installed anything ---------------------------
    out.framePixel  = framePixel;
    out.buttonPixel = buttonPixel;
    out.borderPixel = borderPixel;
    out.fgPixel     = fgPixel;
    out.bgPixel     = bgPixel;
    out.drawGC      = std::move(newDrawGC);
    out.lightGC     = std::move(newLightGC);
    out.shadowGC    = std::move(newShadowGC);
    return true;
}


void Border::installPalette(WindowManager *wm, Palette &palette)
{
    if (palette.nothingToInstall || !palette.drawGC) return;

    Display *d = wm->display();
    Visual *visual = DefaultVisual(d, DefaultScreen(d));
    Colormap cmap  = DefaultColormap(d, DefaultScreen(d));

    // The swap, and the only place the old values are released. Exactly one
    // set is freed and exactly one installed, so a repeated reload cannot
    // accumulate colours (T-9-27's argument, applied to the palette).
    if (m_xftColorsAllocated) {
        XftColorFree(d, visual, cmap, &m_xftForeground);
        XftColorFree(d, visual, cmap, &m_xftBackground);
    }
    m_xftForeground = palette.foreground;
    m_xftBackground = palette.background;
    m_xftColorsAllocated = true;
    palette.xftHeld = false;      // handed over; the destructor must not free them

    m_frameBackgroundPixel  = palette.framePixel;
    m_buttonBackgroundPixel = palette.buttonPixel;
    m_borderPixel           = palette.borderPixel;

    m_drawGC        = std::move(palette.drawGC);
    m_bevelLightGC  = std::move(palette.lightGC);
    m_bevelShadowGC = std::move(palette.shadowGC);
}


void Border::repaintForColourChange()
{
    // A client that was never framed, or whose frame is stripped for
    // fullscreen, has nothing to repaint. Checked rather than assumed, for the
    // same reason relayoutForFrameThickness() checks it.
    if (!m_parent || m_parent == root()) return;

    // THE BACKGROUND PIXEL, THEN A CLEAR. Two of the surfaces the palette
    // governs are painted by the SERVER from the window's background pixel
    // rather than by any code here -- the frame body and the tab's top band --
    // so re-running the draw path alone would leave them in the old colour
    // until something else happened to expose them.
    //
    // The BORDER pixel matters too, and is easy to miss because every one of
    // these windows is created with a border WIDTH of zero: on a SHAPED window
    // the region between the bounding and the clip shape is painted by the
    // server from the border pixel, and that region is the black outline the
    // `borders` key names -- the tab's one-pixel top row and the ring around
    // the tab button.
    XSetWindowBackground(display(), m_parent, m_frameBackgroundPixel);
    XSetWindowBorder(display(), m_parent, m_borderPixel);
    XClearWindow(display(), m_parent);

    if (!isTransient()) {
        if (m_tab != None) {
            XSetWindowBackground(display(), m_tab, m_xftBackground.pixel);
            XSetWindowBorder(display(), m_tab, m_borderPixel);
            XClearWindow(display(), m_tab);
        }
        if (m_button != None) {
            XSetWindowBackground(display(), m_button, m_buttonBackgroundPixel);
            XSetWindowBorder(display(), m_button, m_borderPixel);
            XClearWindow(display(), m_button);
        }

        // The EXISTING paint path, not a second one: a frame repainted after a
        // colour change is byte-identical to one repainted after an Expose.
        const bool active = m_client->isActive();
        drawLabel(active);
        drawButtonBevel(active);
    }
}


// ---------------------------------------------------------------------------
// Live tab-font reload (CGUI-04, plan 09-05)
//
// LOAD BEFORE CLOSE, for the same reason openPalette() allocates before
// anything is released: the shared face is what every frame measures and draws
// its label with, and a window manager holding a null one after a failed
// reload is the outcome XDIS-04's ladder exists to prevent.
//
// The ladder here is loadTabFont()'s, walked in the same order and stopping at
// the same rungs -- but only rungs 1 to 3, because rung 4 is "no face at all",
// which at STARTUP is a legitimate degradation (the alternative is no window
// manager) and at RELOAD time is not: there is already a working face, and
// replacing it with nothing would be a downgrade the user did not ask for. So
// where loadTabFont() lands on rung 4, this returns false and keeps what it
// had.
//
// WM2_FORCE_TAB_FONT_RELOAD_FAILURE is an internal test lever in exactly the
// shape of WM2_FORCE_NO_TAB_FONT and WM2_FORCE_NO_ROTATED_TAB_FONT above: read
// here and nowhere else, never documented for users, no config key and no
// command-line flag. It exists because fontconfig SUBSTITUTES for a family it
// does not have rather than failing, so no string a user can type reaches the
// bottom of this ladder -- which is what makes the refusal path above
// unreachable, and therefore untestable, without it. A DIFFERENT lever from the
// two startup ones on purpose: those would leave the process with no face to
// begin with, and then "the previous face stays loaded" would be a claim about
// nothing.
// ---------------------------------------------------------------------------

bool Border::openTabFace(WindowManager *wm, const std::string &pattern,
                         TabFace &out)
{
    out = TabFace();

    // No frame exists yet, so no face has been loaded and the first Border
    // will read the new value itself. Succeeds with nothing staged.
    if (!m_staticsInitialised) {
        out.nothingToInstall = true;
        return true;
    }

    Display *d = wm->display();

    const char *forceFailure = std::getenv("WM2_FORCE_TAB_FONT_RELOAD_FAILURE");
    const bool forced =
        (forceFailure != nullptr && std::strcmp(forceFailure, "1") == 0);

    x11::XftFontPtr font;
    TabFontRung rung = TabFontRung::NoFont;

    if (!forced) {
        font = x11::make_xft_font_rotated(d, pattern.c_str());
        if (font) rung = TabFontRung::RotatedPreferred;

        if (!font) {
            font = x11::make_xft_font_rotated(d, "sans-serif:bold:size=12");
            if (font) rung = TabFontRung::RotatedGeneric;
        }
        if (!font) {
            font = x11::make_xft_font_name(d, pattern.c_str());
            if (font) rung = TabFontRung::Unrotated;
        }
    }

    if (!font) {
        // Rung 4 territory. Refuse rather than degrade: the previous face is
        // still open, still measured and still what every tab is drawn with.
        std::fprintf(stderr, "wm2: warning: no usable tab font for that "
                             "pattern, keeping the previous one\n");
        return false;
    }

    out.font = std::move(font);
    out.rung = rung;
    return true;
}


void Border::installTabFace(WindowManager *wm, TabFace &face)
{
    if (face.nothingToInstall || !face.font) return;

    Display *d = wm->display();

    // The swap. Exactly one face is closed and exactly one opened, so a
    // repeated reload cannot accumulate faces (T-9-27).
    if (m_tabFont) XftFontClose(d, m_tabFont);
    m_tabFont     = face.font.release();
    m_tabFontRung = face.rung;

    // Re-measure with the SAME arithmetic loadTabFont() uses. The two axis
    // reads below are the ones deferred item 11 corrected in plan 08-14, and
    // they are spelled the same way here on purpose: a second, subtly
    // different measurement is how a tab reloaded at runtime would end up a
    // different width from one measured at startup.
    XGlyphInfo extents;
    const char *sample = "M";
    if (tabFontRotated()) {
        XftTextExtentsUtf8(d, m_tabFont,
            reinterpret_cast<const FcChar8*>(kTabSample), kTabSampleLen, &extents);
        m_tabWidth    = extents.width + kTabOuterClearance + kTabFrameClearance;
        m_tabBaseline = kTabOuterClearance + extents.x;
    } else {
        XftTextExtentsUtf8(d, m_tabFont,
            reinterpret_cast<const FcChar8*>(sample), 1, &extents);
        m_tabWidth = m_tabFont->ascent + m_tabFont->descent + 4;
        if (m_tabWidth < extents.height + 4) m_tabWidth = extents.height + 4;
    }
    if (m_tabWidth < TAB_TOP_HEIGHT * 2 + 8) {
        m_tabWidth = TAB_TOP_HEIGHT * 2 + 8;
    }
}


bool Border::reloadTabFont(WindowManager *wm, const std::string &pattern)
{
    TabFace face;
    if (!openTabFace(wm, pattern, face)) return false;
    installTabFace(wm, face);
    return true;
}


void Border::allocateXftColors()
{
    if (m_xftColorsAllocated) return;

    Display* d = display();
    Visual* visual = DefaultVisual(d, DefaultScreen(d));
    Colormap cmap = DefaultColormap(d, DefaultScreen(d));

    if (!XftColorAllocName(d, visual, cmap, windowManager()->config().tabForeground.c_str(), &m_xftForeground)) {
        windowManager()->fatal("couldn't allocate Xft foreground color");
    }
    if (!XftColorAllocName(d, visual, cmap, windowManager()->config().tabBackground.c_str(), &m_xftBackground)) {
        XftColorFree(d, visual, cmap, &m_xftForeground);
        windowManager()->fatal("couldn't allocate Xft background color");
    }
    m_xftColorsAllocated = true;
}


bool Border::shapeAvailable()
{
    return windowManager()->hasShapeExtension();
}


// D-11: every rectangle-combining Shape request the window manager issues is
// funnelled through this one function. The early return below IS the XDIS-03
// fallback -- on a server that lacks the extension (or with the capability
// forced off for testing) the WM emits no Shape protocol traffic at all,
// instead of sending requests the server cannot answer.
//
// The rectangle array is taken as pointer-to-const for the callers' benefit;
// the constness is cast away at the Xlib boundary, which does not annotate it.
//
// NOTE: a ctest case (tests/test_wm_fallbacks.cpp, tag [shape_invariant])
// asserts that this file names the Xlib rectangle-combining entry point exactly
// ONCE. Keep that literal token out of comment prose here, or the comment
// defeats the guard it documents (same failure mode recorded in 08-01/08-02).
void Border::combineShape(Window dest, int destKind, int xOff, int yOff,
                          const XRectangle *rects, int nRects,
                          int op, int ordering)
{
    if (!windowManager()->hasShapeExtension()) return;

    XShapeCombineRectangles(display(), dest, destKind, xOff, yOff,
                            const_cast<XRectangle *>(rects), nRects,
                            op, ordering);
}


void Border::combineShapeSorted(Window dest, int destKind, int xOff, int yOff,
                                std::vector<XRectangle> rects, int op)
{
    // See the declaration in include/Border.h for why this exists. The sort is
    // STABLE so a list that already satisfies the promise -- which is every list
    // at the shipped frame thickness -- is passed through byte for byte, and the
    // fix cannot change any rendering that was already correct.
    std::stable_sort(rects.begin(), rects.end(),
                     [](const XRectangle& a, const XRectangle& b) {
                         if (a.y != b.y) return a.y < b.y;
                         return a.x < b.x;
                     });

    combineShape(dest, destKind, xOff, yOff, rects.data(),
                 static_cast<int>(rects.size()), op, YXSorted);
}


void Border::shapeParentRectangular(int w, int h)
{
    // Simple rectangular frame: full width/height, no fancy shaping
    XRectangle frame;
    frame.x = 0;
    frame.y = 0;
    frame.width = w + m_tabWidth + FRAME_WIDTH + 1;
    frame.height = h + FRAME_WIDTH + 1;
    combineShape(m_parent, ShapeBounding,
        0, 0, &frame, 1, ShapeSet, YXBanded);

    frame.x++; frame.y++; frame.width -= 2; frame.height -= 2;
    combineShape(m_parent, ShapeClip,
        0, 0, &frame, 1, ShapeSet, YXBanded);
}


void Border::shapeTabRectangular(int w, int h)
{
    // Plain rectangular tab: no diagonal, no button cutouts
    XRectangle tabBounding;
    tabBounding.x = 0;
    tabBounding.y = 0;
    tabBounding.width = m_tabWidth + 2;
    tabBounding.height = m_tabHeight + m_tabWidth + 2;
    combineShape(m_tab, ShapeBounding,
        0, 0, &tabBounding, 1, ShapeSet, YXBanded);

    XRectangle tabClip;
    tabClip.x = 1;
    tabClip.y = 1;
    tabClip.width = m_tabWidth;
    tabClip.height = m_tabHeight + m_tabWidth;
    combineShape(m_tab, ShapeClip,
        0, 0, &tabClip, 1, ShapeSet, YXBanded);
}


void Border::setFrameVisibilityRectangular(bool visible, int w, int h)
{
    // Simple frame visibility: just show/hide resize handle
    if (!visible) return;  // In rectangular mode, frame is always visible

    if (!isFixedSize()) {
        XMapRaised(display(), m_resize);
    } else {
        XUnmapWindow(display(), m_resize);
    }
}


void Border::fatal(const char *s)
{
    windowManager()->fatal(s);
}


Display *Border::display()
{
    return m_client->display();
}


WindowManager *Border::windowManager()
{
    return m_client->windowManager();
}


Window Border::root() const
{
    return m_client->root();
}


void Border::expose(XExposeEvent *e)
{
    if (e->window == m_button) {
        drawButtonBevel(m_client->isActive());
        return;
    }
    if (e->window != m_tab) return;
    drawLabel(m_client->isActive());
}


// THE FLAT LOOK (quick task 261004-vp6). This draws nothing, deliberately.
//
// The 1997 original was flat, and the operator's design handoff restored that.
// Plan 08.5-02's raised edge is gone from the tab and from its button; the
// focus cue is the frame's own visibility, which is what it was before that
// plan -- an unfocused window's frame is shape-subtracted away, so activity is
// already something you can see without a competing edge colour.
//
// WHAT THE FLAT LOOK DROPPED, kept here as the record rather than as
// unreachable code behind an early return. The tab window is an L: a band
// across the top of the frame and a column down its left side, joined at the
// corner, with a stair-stepped diagonal closing the bottom (see shapeTab).
// Two segment draws down the tab column and across the top band described it
// as lit from above and from the left -- a highlight along y=1 across the band,
// a highlight down x=1 at the column's left edge, and a shadow down the
// column's right inner edge from below the button notch to the diagonal. The
// diagonal itself was always left plain, because an edge following a
// stair-stepped boundary is a row of disconnected pixels rather than a line.
//
// THE FUNCTION AND ITS CALL SITES STAY. So do m_bevelLightGC and
// m_bevelShadowGC, their allocateShadeOf() derivation at construction, and
// their live re-derivation on a tab-background change -- the handoff kept them
// on purpose. The consequence is recorded where they are allocated: the shades
// are still derived from the configured tab background and no longer reach a
// pixel. Leaving the empty function in place is also what keeps the six call
// sites honest: a future look that wants an edge back has one place to put it.
void Border::drawBevel(bool active)
{
    (void)active;
}


// The same for the small square button at the tab's top: flat, like the tab it
// sits on. It was previously drawn as a raised key -- a highlight along the top
// and left edges and a shadow along the bottom and right of a buttonDrawSize()
// square -- and on an inactive client the button is not mapped at all.
void Border::drawButtonBevel(bool active)
{
    (void)active;
}


void Border::drawLabel(bool active)
{
    // Rung 4: there is nothing to draw the label WITH. Return before anything
    // touches the font, leaving the tab itself drawn but blank -- the tab
    // window carries the label background as its own background pixel, so the
    // server keeps it painted. The surrounding frame drawing is not on this
    // path and is unaffected.
    if (!hasTabFont()) return;

    if (m_label.empty()) return;

    // Create XftDraw lazily on first use, bound to this tab window (Pitfall 2)
    if (!m_tabDraw) {
        m_tabDraw = x11::XftDrawPtr(XftDrawCreate(
            display(), m_tab,
            DefaultVisual(display(), DefaultScreen(display())),
            DefaultColormap(display(), DefaultScreen(display()))));
    }
    if (!m_tabDraw) return;

    // Clear tab background using XftDrawRect (replaces XClearWindow)
    XftDrawRect(m_tabDraw.get(), &m_xftBackground, 0, 0,
                m_tabWidth, m_tabHeight + m_tabWidth);

    // Kept in its original position -- after the background fill and before the
    // label, which is where any future edge treatment would have to go so text
    // is never drawn under a line. Under the flat look it draws nothing.
    drawBevel(active);

    // Rung 3: an unrotated face cannot be drawn down the tab, so it is drawn
    // across it instead. Split out rather than branched inline so the rotated
    // path below stays byte-for-byte what it was.
    if (!tabFontRotated()) {
        drawLabelHorizontal();
        return;
    }

    // Draw rotated label text (UTF-8 natively via XftDrawStringUtf8)
    //
    // AXIS (deferred item 11, fixed in plan 08-14). The x offset positions the
    // string ACROSS the tab, so it is the thickness -- `width` -- not the
    // along-string advance. THIS read is what made the defect invisible rather
    // than merely ugly: reading `height` put the origin at 2 + 286 = 288 px for
    // a thirty-five-character title, far outside a ~20 px tab, where the server
    // clipped the entire label away. 08-06 predicted the label would overhang
    // its tab; MEASURED, it did not render at all. For a short title the two
    // axes are close enough that the label landed inside the tab by luck, which
    // is why only long titles were ever affected -- and why nobody caught it,
    // since a test window is usually called something short.
    //
    // BASELINE (quick task 260906-ldw). The x offset is now m_tabBaseline, a
    // per-FONT column measured in loadTabFont(), and no longer `2 + the width of
    // this label`. Two things were wrong with reading the label: the strip was
    // sized from a sample with no descender, so every g, j, p, q, y and the foot
    // of a t ran off the tab into the frame's black line; and the origin moved
    // with the title, so two windows whose names differ only in their descenders
    // drew their letters on different columns. Both are properties of the face,
    // so both are settled once when the face is loaded.
    XftDrawStringUtf8(m_tabDraw.get(), &m_xftForeground,
                       m_tabFont,
                       m_tabBaseline, m_tabHeight - 1,
                       reinterpret_cast<const FcChar8*>(m_label.c_str()),
                       static_cast<int>(m_label.size()));
}


// Rung 3 only. The label reads across the tab, so it is trimmed to what fits
// in the tab's width -- on a UTF-8 character boundary, never mid-sequence.
void Border::drawLabelHorizontal()
{
    if (!hasTabFont()) return;

    const int available = m_tabWidth - 4;
    if (available <= 0) return;

    std::string::size_type bytes = m_label.size();
    XGlyphInfo extents;

    while (bytes > 0) {
        XftTextExtentsUtf8(display(), m_tabFont,
            reinterpret_cast<const FcChar8*>(m_label.c_str()),
            static_cast<int>(bytes), &extents);
        if (static_cast<int>(extents.width) <= available) break;

        --bytes;
        while (bytes > 0 &&
               (static_cast<unsigned char>(m_label[bytes]) & 0xC0) == 0x80) {
            --bytes;
        }
    }

    if (bytes == 0) return;

    XftDrawStringUtf8(m_tabDraw.get(), &m_xftForeground,
                      m_tabFont,
                      2, m_tabFont->ascent + 2,
                      reinterpret_cast<const FcChar8*>(m_label.c_str()),
                      static_cast<int>(bytes));
}


bool Border::isTransient()
{
    return m_client->isTransient();
}


bool Border::isFixedSize()
{
    return m_client->isFixedSize();
}


bool Border::hasWindow(Window w) const
{
    return (w != root() && (w == m_parent || w == m_tab ||
                            w == m_button || w == m_resize));
}


bool Border::coordsInHole(int x, int y)
{
    return (x > 1 && x < m_tabWidth - 1 &&
            y > 1 && y < m_tabWidth - 1);
}


void Border::fixTabHeight(int maxHeight)
{
    m_tabHeight = 0x7fff;
    maxHeight -= m_tabWidth; // for diagonal

    m_label = m_client->label();

    // Rung 4: nothing to measure with. Keep a stub tab of the same order as the
    // transient tab (configure() pins that one at a fixed 10) so the frame
    // still has a grabbable tab, and blank the label so drawLabel() has nothing
    // to draw. No Xft call is reachable from here.
    if (!hasTabFont()) {
        m_label.clear();
        m_tabHeight = m_tabWidth * 2;
        if (m_tabHeight > maxHeight) m_tabHeight = maxHeight;
        // The floor is m_tabWidth, not an arbitrary small number: shapeTab()
        // builds rectangles of height (m_tabHeight - m_tabWidth + ...), and a
        // shorter tab makes those negative. XRectangle fields are unsigned, so
        // a negative height wraps to ~65535, the rectangle list stops being
        // YXSorted, and the server answers BadMatch -- which aborts framing
        // entirely. Measured on a 60x40 window before this floor was added.
        if (m_tabHeight < m_tabWidth) m_tabHeight = m_tabWidth;
        return;
    }

    // Rung 3: a horizontal label does not run DOWN the tab, so the shortening
    // loop below -- which trims the title until it fits the tab's length -- is
    // measuring the wrong axis. The tab keeps a fixed length here and
    // drawLabelHorizontal() trims to the tab's width instead.
    if (!tabFontRotated()) {
        if (m_label.empty()) {
            m_label = m_client->iconName().empty() ? std::string("incognito")
                                                   : m_client->iconName();
        }
        m_tabHeight = m_tabWidth * 2;
        if (m_tabHeight > maxHeight) m_tabHeight = maxHeight;
        // The floor is m_tabWidth, not an arbitrary small number: shapeTab()
        // builds rectangles of height (m_tabHeight - m_tabWidth + ...), and a
        // shorter tab makes those negative. XRectangle fields are unsigned, so
        // a negative height wraps to ~65535, the rectangle list stops being
        // YXSorted, and the server answers BadMatch -- which aborts framing
        // entirely. Measured on a 60x40 window before this floor was added.
        if (m_tabHeight < m_tabWidth) m_tabHeight = m_tabWidth;
        return;
    }

    // AXIS (deferred item 11, fixed in plan 08-14). m_tabHeight is the LENGTH of
    // the tab, down which the rotated label runs, so it is bounded below by the
    // along-string advance -- XGlyphInfo::height for a rotated face, not
    // ::width, which is the constant thickness across the string.
    //
    // This is the read that made the tab "very nearly the same length whatever
    // the title is": MEASURED 54 px for a one-character title against 58 px for
    // a thirty-four-character one, four pixels apart for a 34-fold difference in
    // length. All three sites in this function had it the same way round.
    //
    // CONSEQUENCE FOR THE CODE BELOW, and it is not incidental: the
    // icon-name-then-ellipsis shortening path that follows was effectively DEAD,
    // because m_tabHeight almost always came out under maxHeight on the first
    // try. With the length now tracking the title it fires for the first time
    // whenever a long title meets a short window, which is the case it was
    // written for.
    if (!m_label.empty()) {
        XGlyphInfo extents;
        XftTextExtentsUtf8(display(), m_tabFont,
            reinterpret_cast<const FcChar8*>(m_label.c_str()),
            static_cast<int>(m_label.size()), &extents);
        m_tabHeight = extents.height + 6 + m_tabWidth;
    }

    if (m_tabHeight <= maxHeight) return;

    m_label = m_client->iconName().empty() ? std::string("incognito") : m_client->iconName();

    int len = static_cast<int>(m_label.size());
    {
        XGlyphInfo extents;
        XftTextExtentsUtf8(display(), m_tabFont,
            reinterpret_cast<const FcChar8*>(m_label.c_str()), len, &extents);
        m_tabHeight = extents.height + 6 + m_tabWidth;   // along-string advance
    }
    if (m_tabHeight <= maxHeight) return;

    std::string newLabel = m_label;
    do {
        newLabel = newLabel.substr(0, len - 1) + "...";
        XGlyphInfo extents;
        XftTextExtentsUtf8(display(), m_tabFont,
            reinterpret_cast<const FcChar8*>(newLabel.c_str()),
            static_cast<int>(newLabel.size()), &extents);
        m_tabHeight = extents.height + 6 + m_tabWidth;   // along-string advance
        --len;
    } while (m_tabHeight > maxHeight && len > 2);

    m_label = newLabel;

    if (m_tabHeight > maxHeight) m_tabHeight = maxHeight;
}


void Border::shapeTransientParent(int w, int h)
{
    XRectangle r;
    r.x = xIndent() - 1; r.y = yIndent() - 1;
    r.width = w + 2; r.height = h + 2;

    combineShape(m_parent, ShapeBounding, 0, 0,
                 &r, 1, ShapeSet, YXBanded);

    r.x = xIndent(); r.y = yIndent();
    r.width = w; r.height = h;

    combineShape(m_parent, ShapeClip, 0, 0,
                 &r, 1, ShapeSet, YXBanded);
}


void Border::setTransientFrameVisibility(bool visible, int w, int h)
{
    std::vector<XRectangle> rects;

    auto appendRect = [&](int x, int y, int rw, int rh) {
        XRectangle r;
        r.x = x; r.y = y; r.width = rw; r.height = rh;
        rects.push_back(r);
    };

    appendRect(0, 0, w + 1, yIndent() - 1);
    for (int i = 1; i < yIndent(); ++i) {
        appendRect(w + 1, i - 1, i + 1, 1);
    }
    appendRect(0, yIndent() - 1, xIndent() - 1, h - yIndent() + 2);
    for (int i = 1; i < yIndent(); ++i) {
        appendRect(i - 1, h, 1, i + 2);
    }

    combineShapeSorted(m_parent, ShapeBounding, 0, 0, rects,
                       visible ? ShapeUnion : ShapeSubtract);

    rects.clear();

    appendRect(1, 1, w, yIndent() - 2);
    for (int i = 2; i < yIndent(); ++i) {
        appendRect(w + 1, i - 1, i, 1);
    }
    appendRect(1, yIndent() - 1, xIndent() - 2, h - yIndent() + 1);
    for (int i = 2; i < yIndent(); ++i) {
        appendRect(i - 1, h, 1, i + 1);
    }

    combineShapeSorted(m_parent, ShapeClip, 0, 0, rects,
                       visible ? ShapeUnion : ShapeSubtract);
}


void Border::shapeParent(int w, int h)
{
    if (!shapeAvailable()) {
        shapeParentRectangular(w, h);
        return;
    }

    if (isTransient()) {
        shapeTransientParent(w, h);
        return;
    }

    std::vector<XRectangle> rects;

    auto appendRect = [&](int x, int y, int rw, int rh) {
        XRectangle r;
        r.x = x; r.y = y; r.width = rw; r.height = rh;
        rects.push_back(r);
    };

    // top of tab
    appendRect(0, 0, w + m_tabWidth + 1, TAB_TOP_HEIGHT + 2);
    // struts in tab, left
    appendRect(0, TAB_TOP_HEIGHT + 1,
               TAB_TOP_HEIGHT + 2, m_tabWidth - TAB_TOP_HEIGHT * 2 - 1);
    // ...and right
    appendRect(m_tabWidth - TAB_TOP_HEIGHT, TAB_TOP_HEIGHT + 1,
               TAB_TOP_HEIGHT + 2, m_tabWidth - TAB_TOP_HEIGHT * 2 - 1);

    int mainRect = static_cast<int>(rects.size());
    appendRect(xIndent() - 1, yIndent() - 1, w + 2, h + 2);

    // main tab
    appendRect(0, m_tabWidth - TAB_TOP_HEIGHT, m_tabWidth + 2,
               m_tabHeight - m_tabWidth + TAB_TOP_HEIGHT);

    // diagonal
    for (int i = 1; i < m_tabWidth - 1; ++i) {
        appendRect(i, m_tabHeight + i - 1, m_tabWidth - i + 2, 1);
    }

    combineShapeSorted(m_parent, ShapeBounding, 0, 0, rects, ShapeSet);

    // mainRect indexes the UNSORTED list, which is exactly why
    // combineShapeSorted() takes its copy by value.
    rects[mainRect].x++;
    rects[mainRect].y++;
    rects[mainRect].width -= 2;
    rects[mainRect].height -= 2;

    combineShapeSorted(m_parent, ShapeClip, 0, 0, rects, ShapeSet);
}


void Border::shapeTab(int w, int h)
{
    if (!shapeAvailable()) {
        shapeTabRectangular(w, h);
        return;
    }

    if (isTransient()) return;

    std::vector<XRectangle> rects;

    auto appendRect = [&](int x, int y, int rw, int rh) {
        XRectangle r;
        r.x = x; r.y = y; r.width = rw; r.height = rh;
        rects.push_back(r);
    };

    // Bounding rectangles
    appendRect(0, 0, w + m_tabWidth + 1, TAB_TOP_HEIGHT + 2);
    appendRect(0, TAB_TOP_HEIGHT + 1, TAB_TOP_HEIGHT + 2,
               m_tabWidth - TAB_TOP_HEIGHT * 2 - 1);
    appendRect(m_tabWidth - TAB_TOP_HEIGHT, TAB_TOP_HEIGHT + 1,
               TAB_TOP_HEIGHT + 2, m_tabWidth - TAB_TOP_HEIGHT * 2 - 1);
    appendRect(0, m_tabWidth - TAB_TOP_HEIGHT, m_tabWidth + 2,
               m_tabHeight - m_tabWidth + TAB_TOP_HEIGHT);

    for (int i = 1; i < m_tabWidth - 1; ++i) {
        appendRect(i, m_tabHeight + i - 1, m_tabWidth - i + 2, 1);
    }

    combineShapeSorted(m_tab, ShapeBounding, 0, 0, rects, ShapeSet);

    rects.clear();

    // Clipping rectangles
    appendRect(1, 1, w + m_tabWidth - 1, TAB_TOP_HEIGHT);
    appendRect(1, TAB_TOP_HEIGHT + 1, TAB_TOP_HEIGHT,
               m_tabWidth + TAB_TOP_HEIGHT * 2 - 1);
    appendRect(m_tabWidth - TAB_TOP_HEIGHT + 1, TAB_TOP_HEIGHT + 1,
               TAB_TOP_HEIGHT, m_tabWidth + TAB_TOP_HEIGHT * 2 - 1);
    appendRect(1, m_tabWidth - TAB_TOP_HEIGHT + 1, m_tabWidth,
               m_tabHeight - m_tabWidth + TAB_TOP_HEIGHT - 1);

    for (int i = 1; i < m_tabWidth - 2; ++i) {
        appendRect(i + 1, m_tabHeight + i - 1, m_tabWidth - i, 1);
    }

    combineShapeSorted(m_tab, ShapeClip, 0, 0, rects, ShapeSet);
}


void Border::resizeTab(int h)
{
    if (!shapeAvailable()) return;  // No incremental resize shaping in rectangular mode

    if (isTransient()) return;

    int prevTabHeight = m_tabHeight;
    fixTabHeight(h);
    if (m_tabHeight == prevTabHeight) return;

    XWindowChanges wc;
    wc.height = m_tabHeight + 2 + m_tabWidth;
    XConfigureWindow(display(), m_tab, CWHeight, &wc);

    int shorter, longer, operation;

    if (m_tabHeight > prevTabHeight) {
        shorter = prevTabHeight;
        longer = m_tabHeight;
        operation = ShapeUnion;
    } else {
        shorter = m_tabHeight;
        longer = prevTabHeight + m_tabWidth;
        operation = ShapeSubtract;
    }

    XRectangle r;
    r.x = 0; r.y = shorter;
    r.width = m_tabWidth + 2; r.height = longer - shorter;

    combineShape(m_parent, ShapeBounding,
                 0, 0, &r, 1, operation, YXBanded);
    combineShape(m_parent, ShapeClip,
                 0, 0, &r, 1, operation, YXBanded);
    combineShape(m_tab, ShapeBounding,
                 0, 0, &r, 1, operation, YXBanded);

    r.x++; r.width -= 2;

    combineShape(m_tab, ShapeClip,
                 0, 0, &r, 1, operation, YXBanded);

    if (m_client->isActive()) {
        r.x = m_tabWidth + 1; r.y = shorter;
        r.width = FRAME_WIDTH - 1; r.height = longer - shorter;
        combineShape(m_parent, ShapeBounding,
                     0, 0, &r, 1, ShapeUnion, YXBanded);
    }

    std::vector<XRectangle> diagRects;
    for (int i = 1; i < m_tabWidth - 1; ++i) {
        XRectangle dr;
        dr.x = i; dr.y = m_tabHeight + i - 1;
        dr.width = m_tabWidth - i + 2; dr.height = 1;
        diagRects.push_back(dr);
    }

    combineShape(m_parent, ShapeBounding,
                 0, 0, diagRects.data(),
                 static_cast<unsigned int>(diagRects.size()),
                 ShapeUnion, YXBanded);
    combineShape(m_parent, ShapeClip,
                 0, 0, diagRects.data(),
                 static_cast<unsigned int>(diagRects.size()),
                 ShapeUnion, YXBanded);
    combineShape(m_tab, ShapeBounding,
                 0, 0, diagRects.data(),
                 static_cast<unsigned int>(diagRects.size()),
                 ShapeUnion, YXBanded);

    if (diagRects.size() >= 2) {
        for (size_t i = 0; i < diagRects.size() - 1; ++i) {
            diagRects[i].x++; diagRects[i].width -= 2;
        }
        combineShape(m_tab, ShapeClip,
                     0, 0, diagRects.data(),
                     static_cast<unsigned int>(diagRects.size() - 1),
                     ShapeUnion, YXBanded);
    }
}


void Border::shapeResize()
{
    std::vector<XRectangle> rects;

    for (int i = 0; i < FRAME_WIDTH * 2; ++i) {
        XRectangle r;
        r.x = FRAME_WIDTH * 2 - i - 1; r.y = i;
        r.width = i + 1; r.height = 1;
        rects.push_back(r);
    }

    combineShape(m_resize, ShapeBounding, 0, 0,
                 rects.data(), static_cast<unsigned int>(rects.size()),
                 ShapeSet, YXBanded);

    rects.clear();

    for (int i = 1; i < FRAME_WIDTH * 2; ++i) {
        XRectangle r;
        r.x = FRAME_WIDTH * 2 - i; r.y = i;
        r.width = i; r.height = 1;
        rects.push_back(r);
    }

    combineShape(m_resize, ShapeClip, 0, 0,
                 rects.data(), static_cast<unsigned int>(rects.size()),
                 ShapeSet, YXBanded);

    rects.clear();

    for (int i = 0; i < FRAME_WIDTH * 2 - 3; ++i) {
        XRectangle r;
        r.x = FRAME_WIDTH * 2 - i - 1; r.y = i + 3;
        r.width = 1; r.height = 1;
        rects.push_back(r);
    }

    combineShape(m_resize, ShapeClip, 0, 0,
                 rects.data(), static_cast<unsigned int>(rects.size()),
                 ShapeSubtract, YXBanded);

    // Install down-right cursor on resize handle
    windowManager()->installCursorOnWindow(WindowManager::RootCursor::DownRight, m_resize);
}


void Border::setFrameVisibility(bool visible, int w, int h)
{
    if (!shapeAvailable()) {
        setFrameVisibilityRectangular(visible, w, h);
        return;
    }

    std::vector<XRectangle> rects;

    auto appendRect = [&](int x, int y, int rw, int rh) {
        XRectangle r;
        r.x = x; r.y = y; r.width = rw; r.height = rh;
        rects.push_back(r);
    };

    if (isTransient()) {
        setTransientFrameVisibility(visible, w, h);
        return;
    }

    // Bounding rectangles
    appendRect(m_tabWidth + w + 1, 0, FRAME_WIDTH + 1, FRAME_WIDTH);
    appendRect(m_tabWidth + 2, TAB_TOP_HEIGHT + 2, w,
               FRAME_WIDTH - TAB_TOP_HEIGHT - 2);

    int ww = m_tabWidth - TAB_TOP_HEIGHT * 2 - 4;
    appendRect((m_tabWidth + 2 - ww) / 2, (m_tabWidth + 2 - ww) / 2, ww, ww);

    appendRect(m_tabWidth + 2, FRAME_WIDTH,
               FRAME_WIDTH - 2, m_tabHeight + m_tabWidth - FRAME_WIDTH - 2);

    // swap last two if sorted wrong
    if (rects.size() >= 2 && rects[rects.size() - 2].y > rects[rects.size() - 1].y) {
        std::swap(rects[rects.size() - 2], rects[rects.size() - 1]);
    }

    size_t finalIdx = rects.size();
    rects.push_back(rects[finalIdx - 1]);
    rects[finalIdx].x -= 1;
    rects[finalIdx].y += rects[finalIdx].height;
    rects[finalIdx].width += 1;
    rects[finalIdx].height = h - rects[finalIdx].height + 2;

    combineShapeSorted(m_parent, ShapeBounding, 0, 0, rects,
                       visible ? ShapeUnion : ShapeSubtract);
    rects.clear();

    // Clip rectangles
    appendRect(m_tabWidth + w + 1, 1, FRAME_WIDTH, FRAME_WIDTH - 1);
    appendRect(m_tabWidth + 2, TAB_TOP_HEIGHT + 2, w,
               FRAME_WIDTH - TAB_TOP_HEIGHT - 2);

    ww = m_tabWidth - TAB_TOP_HEIGHT * 2 - 6;
    appendRect((m_tabWidth + 2 - ww) / 2, (m_tabWidth + 2 - ww) / 2, ww, ww);

    appendRect(m_tabWidth + 2, FRAME_WIDTH,
               FRAME_WIDTH - 2, h - FRAME_WIDTH);

    // swap last two if sorted wrong
    if (rects.size() >= 2 && rects[rects.size() - 2].y > rects[rects.size() - 1].y) {
        std::swap(rects[rects.size() - 2], rects[rects.size() - 1]);
    }

    appendRect(m_tabWidth + 2, h, FRAME_WIDTH - 2, FRAME_WIDTH + 1);

    combineShapeSorted(m_parent, ShapeClip, 0, 0, rects,
                       visible ? ShapeUnion : ShapeSubtract);

    if (visible && !isFixedSize()) {
        XMapRaised(display(), m_resize);
    } else {
        XUnmapWindow(display(), m_resize);
    }
}


void Border::configure(int x, int y, int w, int h,
                       unsigned long mask, int detail,
                       bool force)
{
    if (!m_parent || m_parent == root()) {
        m_parent = XCreateSimpleWindow(display(), root(), 1, 1, 1, 1, 0,
                                       m_borderPixel, m_frameBackgroundPixel);

        m_tab = XCreateSimpleWindow(display(), m_parent, 1, 1, 1, 1, 0,
                                    m_borderPixel, m_xftBackground.pixel);

        m_button = XCreateSimpleWindow(display(), m_parent, 1, 1, 1, 1, 0,
                                       m_borderPixel, m_buttonBackgroundPixel);

        m_resize = XCreateWindow(display(), m_child, 1, 1,
                                 FRAME_WIDTH * 2, FRAME_WIDTH * 2, 0,
                                 CopyFromParent, InputOutput, CopyFromParent, 0L, nullptr);

        shapeResize();

        XSelectInput(display(), m_parent,
                     SubstructureRedirectMask | SubstructureNotifyMask |
                     ButtonPressMask | ButtonReleaseMask);

        if (!isTransient()) {
            XSelectInput(display(), m_tab,
                         ExposureMask | ButtonPressMask | ButtonReleaseMask |
                         EnterWindowMask);
        }

        // ExposureMask added in plan 08.5-02, when the button carried an edge
        // this code drew: before that the button was drawn entirely by the
        // server from its background pixel and never needed to hear about
        // exposure. Under the flat look it is back to being a plain painted
        // patch, so the mask is no longer load-bearing for the button's own
        // appearance -- it is kept because the exposure handler is the one
        // place a future treatment would be repainted from, and because
        // removing a selected event mask is a behaviour change this task did
        // not ask for.
        XSelectInput(display(), m_button,
                     ExposureMask | ButtonPressMask | ButtonReleaseMask);
        XSelectInput(display(), m_resize, ButtonPressMask | ButtonReleaseMask);
        mask |= CWX | CWY | CWWidth | CWHeight | CWBorderWidth;
    }

    XWindowChanges wc;
    wc.x = x - xIndent();
    wc.y = y - yIndent();
    wc.width  = w + xIndent() + 1;
    wc.height = h + yIndent() + 1;
    wc.border_width = 0;
    wc.sibling = None;
    wc.stack_mode = detail;
    XConfigureWindow(display(), m_parent, mask, &wc);

    unsigned long rmask = 0L;
    if (mask & CWWidth)  rmask |= CWX;
    if (mask & CWHeight) rmask |= CWY;
    wc.x = w - FRAME_WIDTH * 2;
    wc.y = h - FRAME_WIDTH * 2;
    XConfigureWindow(display(), m_resize, rmask, &wc);

    if (force ||
        (m_prevW < 0 || m_prevH < 0) ||
        ((mask & (CWWidth | CWHeight)) && (w != m_prevW || h != m_prevH))) {

        int prevTabHeight = m_tabHeight;
        if (isTransient()) m_tabHeight = 10;
        else fixTabHeight(h);

        shapeParent(w, h);
        setFrameVisibility(m_client->isActive(), w, h);

        if (force || w != m_prevW ||
            prevTabHeight != m_tabHeight || m_prevW < 0 || m_prevH < 0) {

            wc.x = 0;
            wc.y = 0;
            wc.width = w + xIndent();
            wc.height = m_tabHeight + 2 + m_tabWidth;
            XConfigureWindow(display(), m_tab, mask, &wc);
            shapeTab(w, h);
        }

        m_prevW = w;
        m_prevH = h;

    } else {
        resizeTab(h);
    }

    wc.x = buttonDrawInset();
    wc.y = wc.x;
    wc.width = wc.height = buttonDrawSize();
    XConfigureWindow(display(), m_button, mask, &wc);
}




void Border::relayoutForFrameThickness(int x, int y, int w, int h)
{
    // A client that was never framed (or is fullscreen, its frame stripped) has
    // nothing here to re-lay out. Checked rather than assumed: configure()
    // would CREATE the frame windows from this call, which is not what a
    // thickness change should do.
    if (!m_parent || m_parent == root()) return;

    // The resize handle is created FRAME_WIDTH*2 square and shaped from the
    // same number; configure() below only ever moves it. Both have to be
    // redone or the corner grabber keeps the old thickness's size and its
    // triangular shape stops matching the frame it sits in.
    if (m_resize != None) {
        XResizeWindow(display(), m_resize, FRAME_WIDTH * 2, FRAME_WIDTH * 2);
        shapeResize();
    }

    // force = true is load-bearing. w and h have NOT changed -- only the
    // indents around them have -- and without the force, configure()'s
    // "did the size change?" test skips the reshape of the frame and the tab,
    // which are precisely the two windows the thickness governs. The result
    // would be a frame that moved but kept its old outline.
    configure(x, y, w, h, CWX | CWY | CWWidth | CWHeight, Above, true);

    // The child moves to the new content offset and keeps its own size, so its
    // absolute position on screen is unchanged: the frame origin moved by
    // exactly the amount the indent grew. That is why no synthetic
    // ConfigureNotify is owed here -- ICCCM reports absolute position and size,
    // and neither changed.
    XMoveWindow(display(), m_child, xIndent(), yIndent());
}


void Border::relayoutForTabFont(int x, int y, int w, int h)
{
    // The thickness path, not a copy of it. A tab-font change moves
    // m_tabWidth, xIndent() is m_tabWidth + FRAME_WIDTH + 1, and every window
    // the frame is made of is positioned from those indents -- which is the
    // same set of recomputations a thickness change needs, done by the same
    // code. Two parallel computations of one geometry is how they drift.
    relayoutForFrameThickness(x, y, w, h);

    // ...and then the label, which the thickness path deliberately does not
    // redraw because a thickness change does not alter the FACE. Here it does:
    // the glyphs themselves are different, so the tab has to be repainted in
    // them rather than left showing the old face until the next Expose.
    //
    // WHICH IS WHY THIS IS THE ENTRY POINT WindowManager::applyConfig() USES
    // FOR EVERY CLIENT WHEN THE FACE MOVED, thickness or no thickness
    // (CodeRabbit F3). An application carrying BOTH used to take the thickness
    // branch alone -- the walk above, without this repaint -- and left every
    // open frame in the new thickness wearing the old glyphs. "Until the next
    // Expose" is a real reprieve on this project's own target: a VNC server
    // with backing store restores the tab's old contents instead of asking for
    // them back, so the stale label can survive the reshape that would
    // otherwise have hidden the defect.
    if (!isTransient() && m_parent && m_parent != root()) {
        const bool active = m_client->isActive();
        drawLabel(active);
        drawButtonBevel(active);
    }
}


void Border::moveTo(int x, int y)
{
    XWindowChanges wc;
    wc.x = x - xIndent();
    wc.y = y - yIndent();
    XConfigureWindow(display(), m_parent, CWX | CWY, &wc);
}


void Border::map()
{
    if (m_parent == root()) {
        std::fprintf(stderr, "wm2: bad parent in Border::map()\n");
    } else {
        XMapWindow(display(), m_parent);

        if (!isTransient()) {
            XMapWindow(display(), m_tab);
            XMapWindow(display(), m_button);
            if (!isFixedSize()) XMapWindow(display(), m_resize);
        }
    }
}


void Border::mapRaised()
{
    if (m_parent == root()) {
        std::fprintf(stderr, "wm2: bad parent in Border::mapRaised()\n");
    } else {
        XMapRaised(display(), m_parent);

        if (!isTransient()) {
            XMapWindow(display(), m_tab);
            XMapRaised(display(), m_button);
            if (!isFixedSize()) XMapRaised(display(), m_resize);
        }
    }
}


void Border::lower()
{
    XLowerWindow(display(), m_parent);
}


void Border::unmap()
{
    if (m_parent == root()) {
        std::fprintf(stderr, "wm2: bad parent in Border::unmap()\n");
    } else {
        XUnmapWindow(display(), m_parent);

        if (!isTransient()) {
            XUnmapWindow(display(), m_tab);
            XUnmapWindow(display(), m_button);
        }
    }
}


void Border::decorate(bool active, int w, int h)
{
    setFrameVisibility(active, w, h);

    // This is the one place that learns activity changed, so both surfaces are
    // repainted here. Under the flat look neither surface has an active-only
    // treatment left to add or remove -- the frame's own visibility is the
    // focus cue -- but the repaint stays: drawLabel() is what redraws the tab
    // background and the label, and the button paint is what the second call
    // keeps in step with it.
    if (!isTransient()) {
        drawLabel(active);
        drawButtonBevel(active);
    }
}


void Border::reparent()
{
    x11::ServerGrab grab(display());
    XReparentWindow(display(), m_child, m_parent, xIndent(), yIndent());
}


void Border::stripForFullscreen()
{
    // Unmap all frame components
    XUnmapWindow(display(), m_parent);
    if (!isTransient()) {
        XUnmapWindow(display(), m_tab);
        XUnmapWindow(display(), m_button);
        if (!isFixedSize()) XUnmapWindow(display(), m_resize);
    }

    // Reparent child directly to root (per D-05: covers full screen including docks)
    x11::ServerGrab grab(display());
    XReparentWindow(display(), m_child, root(), 0, 0);
}


void Border::restoreFromFullscreen(int x, int y, int w, int h)
{
    // WHAT THIS DOES NOT DO. It re-parents and re-configures at the indents in
    // force NOW, so a thickness that changed during the fullscreen spell is
    // honoured for the parent and for where the child sits inside it -- but the
    // tab, the button and the grabber are merely remapped, keeping the geometry
    // and the shape they had when stripForFullscreen() unmapped them, and no
    // window's background pixel is touched. The re-layout and the repaint that
    // a live change would have performed are replayed by
    // Client::applyDeferredFrameRefresh(), which setFullscreen(false) calls
    // straight after this returns.

    // Reparent child back into frame
    x11::ServerGrab grab(display());
    XReparentWindow(display(), m_child, m_parent, xIndent(), yIndent());

    // Configure frame at saved position
    XWindowChanges wc;
    wc.x = x - xIndent();
    wc.y = y - yIndent();
    wc.width = w + xIndent() + 1;
    wc.height = h + yIndent() + 1;
    XConfigureWindow(display(), m_parent, CWX | CWY | CWWidth | CWHeight, &wc);

    // Resize child to saved size, AT THE FRAME'S CONTENT OFFSET.
    //
    // Not (0, 0): the reparent two statements above deliberately places the
    // child at (xIndent, yIndent), and moving it to the frame's origin here
    // undid that -- putting the client underneath the sideways tab and the top
    // border, and leaving it xIndent pixels left and yIndent pixels above where
    // the window manager's own m_x/m_y say it is.
    //
    // MEASURED before this fix (plan 08-12): a client the WM had placed at
    // (175,128 300x220) came back from fullscreen at (150,120 300x220) -- off
    // by exactly the indent, every single round trip. Fixed alongside the two
    // identical spellings in Client::setMaximized(); this window manager has
    // exactly one convention for where a client sits inside its frame, and
    // these three call sites were the only places that did not follow it.
    XMoveResizeWindow(display(), m_child, xIndent(), yIndent(), w, h);

    // Map all frame components
    map();
}


void Border::eventButton(XButtonEvent *e)
{
    if (e->window == m_parent) {
        if (!m_client->isActive()) return;
        if (isTransient()) {
            if (e->x >= xIndent() && e->y >= yIndent()) {
                return;
            } else {
                m_client->move(e);
                return;
            }
        }
        // The frame owns a thin part of the button's target square too -- the
        // notch's inner edge. Same routing as the tab branch below, so a miss
        // that lands there presses the button instead of starting a drag.
        if (e->type == ButtonPress &&
            e->x >= 0 && e->x < buttonHitSize() &&
            e->y >= 0 && e->y < buttonHitSize()) {
            runButtonPress(e, e->x - buttonDrawInset(),
                              e->y - buttonDrawInset());
            return;
        }

        m_client->moveOrResize(e);
        return;

    } else if (e->window == m_tab) {
        if (e->button == Button2) {
            // D-08: Middle-click on tab toggles maximize
            m_client->toggleMaximized();
            return;
        }

        // A press in the tab's top square was aimed at the button. The tab and
        // the button share an origin, so the tab's coordinates ARE the frame's
        // here, and shifting by the inset puts them in the button's.
        //
        // Gated on isActive() for the same reason the frame branch above is:
        // setFrameVisibility() subtracts the button's square from an inactive
        // client's frame and unmaps the button, so there is no button to aim at,
        // and a press here is a plain click on an unfocused window.
        if (e->type == ButtonPress && m_client->isActive() &&
            e->x >= 0 && e->x < buttonHitSize() &&
            e->y >= 0 && e->y < buttonHitSize()) {
            runButtonPress(e, e->x - buttonDrawInset(),
                              e->y - buttonDrawInset());
            return;
        }

        m_client->move(e);
        return;
    }

    if (e->window == m_resize) {
        m_client->resize(e, true, true);
        return;
    }

    if (e->window != m_button || e->type == ButtonRelease) return;

    runButtonPress(e, e->x, e->y);
}


// The button's target square is the tab's whole top square, while the square it
// PAINTS stays the small one it has always been.
//
// It has to work this way round. The obvious approach -- grow the button window,
// or give it a large input shape and a small bounding shape -- cannot work here,
// and both were built and measured before this was written: the TAB is stacked
// ABOVE the button and is shaped with a hole exactly the size of the painted
// square, so every press outside that hole is delivered to the tab no matter how
// large the button window is. Widening the hole instead would stop the tab
// painting its struts there, which changes the frame's appearance.
//
// So the presses the tab already receives are interpreted here instead, and not
// one window, shape or pixel moves.
//
// What this fixes, MEASURED with a hit probe on a 16px tab: the target was 8x8,
// 64 square pixels, and a near-miss did not merely fail. Four pixels to any side
// hit the tab and started a DRAG; one or two to the right or below fell through
// the frame's shaped hole to the ROOT WINDOW and opened the menu. Both are worse
// than nothing happening.
void Border::runButtonPress(XButtonEvent *e, int startX, int startY)
{
    // Pointer events only: the GrabPointer request carries its mask as a
    // CARD16, so StructureNotifyMask (bit 17) was silently truncated out of
    // this mask and never took effect (WINDOWS.md ledger 11).
    int menuGrabMask = ButtonPressMask | ButtonReleaseMask | ButtonMotionMask;
    if (windowManager()->attemptGrab(m_button, None, menuGrabMask, e->time)
        != GrabSuccess) {
        return;
    }

    XEvent event;
    bool found;
    bool done = false;
    unsigned long tdiff = 0L;
    int x = startX;
    int y = startY;
    int action = 1;

    // Two spans, and they must not be conflated. drawSize is what gets painted,
    // in button-window coordinates. The bounds below are the TARGET, expressed
    // in the same button-window coordinates but covering the tab's whole top
    // square -- which starts one inset ABOVE and LEFT of the button, hence the
    // negative lower bound. They are what give the pointer room to wander during
    // a press without silently cancelling the action.
    const int drawSize = buttonDrawSize();
    const int lo = -buttonDrawInset();
    const int hi = buttonHitSize() - buttonDrawInset();

    XFillRectangle(display(), m_button, m_drawGC.get(), 0, 0, drawSize, drawSize);

    while (!done) {

        found = false;

        if (tdiff > static_cast<unsigned long>(windowManager()->config().destroyWindowDelay) && action == 1) {
            windowManager()->installCursor(WindowManager::RootCursor::Delete);
            action = 2;
        }

        while (XCheckMaskEvent(display(),
                ButtonPressMask | ButtonReleaseMask | ButtonMotionMask | ExposureMask,
                &event)) {
            found = true;
            if (event.type != MotionNotify) break;
        }

        if (!found) {
            // Ledger 8: the 50 ms sleep is now a wait that also watches the
            // exit flag and the self-pipe. Interrupted: no action is taken.
            //
            // MEASURED, NOT ASSUMED (WR-14). tdiff used to accrue a hard-coded
            // 50 on every Timeout, on the assumption that modalWait(..., 50)
            // waited 50 ms. Since the configuration socket joined the poll,
            // modalWait clamps its timeout with clampPollTimeoutForSocket() --
            // so a connection within 50 ms of its silence deadline makes the
            // poll return after a few milliseconds with r == 0, and a full
            // tick was charged for a fraction of one. tdiff is what escalates
            // this hold from hide to destroy, so the escalation fired early.
            // The exposure was one tick per silent connection; the reason to
            // fix it rather than bound it is that any future hint source makes
            // it worse, and the clock is right here.
            const std::chrono::steady_clock::time_point before =
                std::chrono::steady_clock::now();
            const WindowManager::ModalWait wait = windowManager()->modalWait(
                ButtonPressMask | ButtonReleaseMask | ButtonMotionMask | ExposureMask,
                &event, 50);
            if (wait == WindowManager::ModalWait::Interrupted) {
                // "No action is taken" has to be made true here: action starts
                // at 1 (hide) and a long in-bounds hold turns it into 2 (kill),
                // and the code after the loop acts on whichever is set. Left
                // alone, a signal during a press would hide -- or close -- the
                // client on the way out (CodeRabbit pre-flight, 2026-09-05).
                action = 0;
                break;
            }
            if (wait == WindowManager::ModalWait::Timeout) {
                const long long waited =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - before).count();
                tdiff += static_cast<unsigned long>(waited > 0 ? waited : 0);
                continue;
            }
        }

        switch (event.type) {

        default:
            std::fprintf(stderr, "wm2: unknown event type %d\n", event.type);
            break;

        case Expose:
            windowManager()->eventExposure(&event.xexpose);
            break;

        case ButtonPress:
            break;

        case ButtonRelease:
            {
                int state = event.xbutton.state & (Button1Mask | Button2Mask | Button3Mask |
                                                    Button4Mask | Button5Mask);
                if (event.xbutton.type == ButtonRelease && (state & (state - 1)) != 0) {
                    action = 0;
                }
            }
            if (x < lo || y < lo || x >= hi || y >= hi) {
                action = 0;
            }
            windowManager()->releaseGrab(&event.xbutton);
            done = true;
            break;

        case MotionNotify:
            tdiff = event.xmotion.time - e->time;
            if (tdiff > 5000L) tdiff = 5001L;

            x = event.xmotion.x;
            y = event.xmotion.y;

            if (action == 0 || action == 2) {
                if (x < lo || y < lo || x >= hi || y >= hi) {
                    windowManager()->installCursor(WindowManager::RootCursor::Normal);
                    action = 0;
                } else {
                    windowManager()->installCursor(WindowManager::RootCursor::Delete);
                    action = 2;
                }
            }
            break;
        }
    }

    // The clear wipes the press feedback, and the repaint that follows is what
    // puts the button's own drawing back. Under the flat look the button has
    // nothing but its background pixel to restore and the clear alone is
    // enough; the call stays because this is the paint path after a press, and
    // when it was not here the button went permanently flat after its first
    // press -- a decoration bug that only appears after an interaction, which
    // is the kind nobody notices in a screenshot.
    XClearWindow(display(), m_button);
    drawButtonBevel(m_client->isActive());
    windowManager()->installCursor(WindowManager::RootCursor::Normal);

    if (tdiff > 5000L) return;  // dithered too long

    if (action == 1) m_client->hide();
    else if (action == 2) m_client->kill();
}
