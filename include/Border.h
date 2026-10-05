#pragma once

#include "x11wrap.h"
#include <X11/Xutil.h>
#include <string>
#include <vector>

class Client;
class WindowManager;
struct Config;

// Frame dimensions (from upstream Config.h and Border.h)
constexpr int TAB_TOP_HEIGHT = 2;
extern int FRAME_WIDTH;                 // CONFIG_FRAME_THICKNESS (runtime from Config)
constexpr int TRANSIENT_FRAME_WIDTH = 4;

class Border {
public:
    Border(Client *client, Window child);
    ~Border();

    void map();
    void unmap();
    void lower();
    void mapRaised();
    void decorate(bool active, int w, int h);
    void reparent();
    void configure(int x, int y, int w, int h, unsigned long mask, int detail,
                   bool force = false);
    void moveTo(int x, int y);

    // Re-lay this frame out IN PLACE after FRAME_WIDTH changed (CGUI-04, plan
    // 09-04). Every window the frame is made of -- the frame itself, the tab,
    // the button and the resize handle -- has geometry computed from the frame
    // thickness, and two of them (the resize handle's size and its shape) are
    // set only at creation, so configure() alone would leave a corner grabber
    // sized for the old thickness.
    //
    // Deliberately NOT a destroy-and-rebuild: rebuilding would reparent the
    // client, which flashes and loses stacking order. The client window keeps
    // its size and its position ON SCREEN; only the decoration around it moves.
    void relayoutForFrameThickness(int x, int y, int w, int h);

    // -----------------------------------------------------------------------
    // Live colour and font reload (CGUI-04, plan 09-05)
    // -----------------------------------------------------------------------

    // A palette that has been ALLOCATED and not yet INSTALLED: five pixels,
    // two Xft colours, two derived bevel shades and up to three graphics
    // contexts, all obtained from `next` and none of them yet shared with
    // anything that draws.
    //
    // The sibling of TabFace below, and it exists for the enlarged form of the
    // same defect. applyConfig() applies a WHOLE Config on a reload, so one
    // edit can carry a colour AND a font -- and a palette that was allocated
    // and swapped before either face was opened left every frame built
    // afterwards wearing colours the window manager then refused to report,
    // with no `set` able to repair it (coloursChanged is false on the way
    // back). Splitting the allocation from the swap lets applyConfig() prove
    // the whole configuration applies before it commits any part of it.
    //
    // Owns the two Xft colours until installPalette() takes them: a staged
    // palette that is abandoned frees them in its destructor, so a refusal
    // upstream leaks nothing.
    struct Palette {
        // Nothing to install. Before the first frame exists the statics block
        // has not run, and the first Border will read the new colours for
        // itself -- so there is nothing to allocate against and nothing to
        // swap.
        bool nothingToInstall = false;

        unsigned long framePixel  = 0;
        unsigned long buttonPixel = 0;
        unsigned long borderPixel = 0;
        unsigned long fgPixel     = 0;
        unsigned long bgPixel     = 0;

        XftColor foreground{};
        XftColor background{};
        bool     xftHeld = false;   // the two above are allocated and not handed over

        x11::GCPtr drawGC;
        x11::GCPtr lightGC;      // null when the shade would not allocate
        x11::GCPtr shadowGC;     // ditto; every draw site treats null as "no bevel"

        // The connection the two Xft colours came from, so the destructor can
        // free them with no window manager to ask.
        Display *display  = nullptr;
        Visual  *visual   = nullptr;
        Colormap colormap = 0;

        Palette() = default;
        ~Palette();
        Palette(const Palette &) = delete;
        Palette &operator=(const Palette &) = delete;
    };

    // Allocate every colour and every graphics context this class draws with,
    // from `next`, WITHOUT installing any of them. Changes nothing whichever
    // way it answers; returns false with the offending key in `keyOut`.
    //
    // Static because the palette is shared by every frame: one allocation, one
    // set of graphics contexts, one bevel derivation.
    static bool openPalette(WindowManager *wm, const Config &next, Palette &out,
                            std::string &keyOut);

    // Install a palette openPalette() produced. Cannot fail: everything that
    // can is upstream, in the open. Exactly one set of old values is released
    // and exactly one installed, so a repeated reload cannot accumulate
    // colours. The per-instance repaint that makes them visible is the next
    // method down.
    static void installPalette(WindowManager *wm, Palette &palette);

    // Push the reloaded palette onto THIS frame: the new background pixels on
    // each window, a clear so the server repaints from them, and the existing
    // paint path re-run for the tab and the button. Deliberately re-runs
    // drawLabel()/drawButtonBevel() rather than inventing a second drawing
    // path, so a frame repainted after a colour change is byte-identical to
    // one repainted after an Expose.
    void repaintForColourChange();

    // XDIS-04: which rung of the tab-font degradation ladder this process
    // landed on. Established once, at the first Border construction, and never
    // revisited -- font availability is fixed for the lifetime of the X
    // connection, exactly like the extension sentinels in include/Manager.h.
    //
    // Rung 1 is the normal path and the only one that prints nothing; every
    // other rung announces itself on stderr so a release-evidence transcript
    // records the degradation instead of leaving it to be inferred from a
    // screenshot.
    //
    // PUBLIC because TabFace below carries one, and TabFace is what lets a
    // caller hold an opened-but-not-installed face across another open (CR-04).
    enum class TabFontRung {
        RotatedPreferred,   // 1 -- sideways labels from the preferred chain
        RotatedGeneric,     // 2 -- sideways labels from the generic sans chain
        Unrotated,          // 3 -- horizontal labels, truncated to the tab width
        NoFont              // 4 -- no label at all; frames are still drawn (None is an Xlib macro)
    };

    // A tab face that has been OPENED and not yet INSTALLED.
    //
    // The reason this type exists is CR-04: applyConfig() applies a whole
    // Config on a reload, so a file that changes tab-font AND menu-font runs
    // both swaps -- and a tab face that installed followed by a menu face that
    // would not open left the shared face and the shared tab width moved while
    // m_config was never updated. `get tab-font` then named a pattern nothing
    // on screen was drawn with, and no `set` could repair it. Splitting the
    // open from the install lets the caller prove BOTH faces open before it
    // swaps EITHER.
    struct TabFace {
        x11::XftFontPtr font;
        TabFontRung     rung = TabFontRung::NoFont;
        // True when there is nothing to install: no frame has been built yet,
        // so no face has been loaded and the first Border will read the new
        // value for itself.
        bool            nothingToInstall = false;
    };

    // Open `pattern` as a tab face WITHOUT installing it. Walks the same ladder
    // loadTabFont() walks, stopping at rungs 1 to 3; rung 4 ("no face at all")
    // is a legitimate degradation at startup and a downgrade at reload time, so
    // it is reported as false here and the previous face stays in force.
    // Changes nothing at all, whichever way it answers.
    static bool openTabFace(WindowManager *wm, const std::string &pattern,
                            TabFace &out);

    // Install a face openTabFace() produced and re-measure the tab width.
    // Cannot fail: everything that can is upstream, in the open. Exactly one
    // face is closed and exactly one installed, so a repeated reload cannot
    // accumulate faces (T-9-27).
    static void installTabFace(WindowManager *wm, TabFace &face);

    // Load `pattern` as the shared tab face and re-measure the tab width: the
    // open and the install in one call, for a caller with only one face to
    // change. LOADS BEFORE IT CLOSES -- a pattern with no usable face at any
    // rung leaves the previous face loaded and the previous tab width in
    // force, and returns false. Static for the same reason as openPalette().
    static bool reloadTabFont(WindowManager *wm, const std::string &pattern);

    // Re-lay this frame out after the shared tab font changed. The tab's
    // thickness moves with the face's metrics, so the indents move with it and
    // the frame, the tab, the button and the shape must all be recomputed --
    // which is exactly what the thickness path above already does, so this
    // delegates to it rather than computing the same geometry a second way.
    // The label is redrawn afterwards because the FACE changed, which the
    // thickness path has no reason to do.
    void relayoutForTabFont(int x, int y, int w, int h);

    // Fullscreen support
    void stripForFullscreen();
    void restoreFromFullscreen(int x, int y, int w, int h);

    // Accessors
    Window parent() const { return m_parent; }
    bool hasWindow(Window w) const;

    // Delegated accessors (call into Client)
    WindowManager* windowManager();
    bool isTransient();
    bool isFixedSize();

    Display* display();
    Window root() const;

    void expose(XExposeEvent *e);
    void eventButton(XButtonEvent *e);

    int yIndent() {
        return isTransient() ? TRANSIENT_FRAME_WIDTH + 1 : FRAME_WIDTH + 1;
    }
    int xIndent() {
        return isTransient() ? TRANSIENT_FRAME_WIDTH + 1 :
            m_tabWidth + FRAME_WIDTH + 1;
    }

    bool coordsInHole(int x, int y);

    // The button paints a small square but ANSWERS across the whole top square
    // of the tab. The two sizes are deliberately different and must not be
    // conflated: the painted one is the window's look, which is settled, and the
    // hit one is how hard it is to aim at, which was 8x8 -- 64 square pixels,
    // with every near-miss landing on the tab (starting a drag) or in the shaped
    // hole (falling through to the root window). See Border::configure().
    static int buttonDrawInset() { return TAB_TOP_HEIGHT + 2; }
    static int buttonDrawSize()  { return m_tabWidth - TAB_TOP_HEIGHT * 2 - 4; }
    static int buttonHitSize()   { return m_tabWidth; }

private:
    void fatal(const char *m);

    std::string m_label;

    void loadTabFont();
    void fixTabHeight(int h);
    void drawLabel(bool active);
    void drawLabelHorizontal();

    // Predicates over the rung, in the same shape as the capability predicates
    // in include/Manager.h: consumers branch on these, never on the enumerator.
    static bool hasTabFont()    { return m_tabFont != nullptr; }
    static bool tabFontRotated() {
        return m_tabFont != nullptr &&
               (m_tabFontRung == TabFontRung::RotatedPreferred ||
                m_tabFontRung == TabFontRung::RotatedGeneric);
    }

    void setFrameVisibility(bool, int, int);
    void setTransientFrameVisibility(bool, int, int);
    void shapeParent(int, int);
    void shapeTransientParent(int, int);
    void shapeTab(int, int);
    void resizeTab(int);
    void shapeResize();

    // The tab-button press loop. Takes the press position in BUTTON-window
    // coordinates, because it is entered from two places: a press on the button
    // itself, and a press on the tab that landed inside the button's target
    // square but outside the small square the button paints.
    void runButtonPress(XButtonEvent *e, int startX, int startY);

    // D-11: the one and only entry point through which this class is permitted
    // to issue a rectangle-combining request to the X Shape extension. Mirrors
    // the Xlib call's parameter list minus the Display*, and takes the rectangle
    // array as pointer-to-const so callers may pass const data. Definition in
    // src/Border.cpp guards on the capability and returns without touching the
    // connection when it is absent.
    void combineShape(Window dest, int destKind, int xOff, int yOff,
                      const XRectangle *rects, int nRects,
                      int op, int ordering);

    // The YXSorted form of the above (plan 08-13).
    //
    // A YXSorted request is a PROMISE to the server that the rectangles arrive
    // sorted by y origin and then by x origin. The server validates the promise
    // and rejects the whole request with BadMatch when it is broken -- so a
    // mis-ordered list does not merely render slowly, it leaves the window
    // UNSHAPED, and WindowManager::errorHandler() logs the rejection and carries
    // on. Every list below is assembled in an order that depends on FRAME_WIDTH
    // and on the measured tab width, neither of which is fixed, so the promise
    // held only for a narrow band of configured frame thicknesses.
    //
    // Takes the vector BY VALUE on purpose: shapeParent() submits one list
    // twice, mutating a remembered INDEX in between, and sorting the caller's
    // own vector would invalidate that index.
    void combineShapeSorted(Window dest, int destKind, int xOff, int yOff,
                            std::vector<XRectangle> rects, int op);

    Client *m_client;

    Window m_parent;
    Window m_tab;
    Window m_child;
    Window m_button;
    Window m_resize;

    int m_prevW;
    int m_prevH;
    int m_tabHeight;

    // Static resources shared across all Border instances
    static int m_tabWidth;
    // The one column the rotated label's baseline sits on -- a property of
    // the FONT, measured beside m_tabWidth when a face is loaded, never of
    // the title. Left at -1 on the unrotated and no-font rungs, which do not
    // read it (quick task 260906-ldw; see src/Border.cpp).
    static int m_tabBaseline;
    static XftFont *m_tabFont;         // raw pointer, managed via static refcount
    static TabFontRung m_tabFontRung;

    // The statics below used to be initialised on "m_tabFont is still null",
    // which stops working the moment a null font becomes a legitimate outcome
    // (rung 4): every subsequent Border would re-run the whole block and leak a
    // GC per frame. The guard is therefore explicit rather than inferred.
    static bool m_staticsInitialised;
    static x11::GCPtr m_drawGC;

    // The two derived shades of plan 08.5-02's 1 px raised bevel. STILL
    // ALLOCATED, AND NO LONGER DRAWN WITH: quick task 261004-vp6 restored the
    // flat look the 1997 original had, so both draw functions below are
    // documented no-ops. The handoff kept these GCs and their derivation
    // rather than ripping them out; see src/Border.cpp above drawBevel.
    //
    // Why a single pixel was the right size when there was one, kept as the
    // record: at this window manager's scale -- a 7 px frame and a ~22 px tab
    // -- a 2 px bevel is Windows 95, while one pixel inside the existing 1 px
    // black outline put four values across about four pixels (highlight, body,
    // shadow, black). That was the machined-metal read, and it is the one
    // NeXTSTEP used, which is where wm2's look comes from. The flat look drops
    // it in favour of the original's plain surfaces; activity is shown the way
    // it was before 08.5-02, by the frame appearing at all
    // (setFrameVisibility).
    //
    // Either GC may be null when the colormap is full, which is why that was
    // never a fatal error.
    static x11::GCPtr m_bevelLightGC;
    static x11::GCPtr m_bevelShadowGC;
    void drawBevel(bool active);
    void drawButtonBevel(bool active);
    static unsigned long m_frameBackgroundPixel;
    static unsigned long m_buttonBackgroundPixel;
    static unsigned long m_borderPixel;
    static int m_borderCount;  // reference count for static resources

    // Static Xft colors (allocated once, shared across all Border instances)
    static XftColor m_xftForeground;
    static XftColor m_xftBackground;
    static bool m_xftColorsAllocated;  // guard for XftColor allocation

    // Per-instance XftDraw for tab label rendering (Pitfall 2)
    x11::XftDrawPtr m_tabDraw;

    // Shape extension helpers
    void allocateXftColors();
    bool shapeAvailable();

    // Rectangular fallback when Shape extension unavailable (D-07)
    void shapeParentRectangular(int w, int h);
    void shapeTabRectangular(int w, int h);
    void setFrameVisibilityRectangular(bool visible, int w, int h);
};
