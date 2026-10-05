#include <catch2/catch_test_macros.hpp>
#include "x11wrap.h"
#include "Config.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <cstdlib>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// Test 1: Xft font loads by fontconfig pattern (validates VISL-04, A3)
// ---------------------------------------------------------------------------

TEST_CASE("Xft font loads by fontconfig pattern", "[xft][poc]")
{
    // DisplayPtr ensures XCloseDisplay happens AFTER all RAII wrappers
    // that store the raw pointer are destroyed (declared first = destroyed last)
    x11::DisplayPtr display(XOpenDisplay(std::getenv("DISPLAY")));
    REQUIRE(display);

    // D-02: fallback chain Noto Sans -> DejaVu Sans -> any Sans
    auto font = x11::make_xft_font_name(display.get(), "Noto Sans,DejaVu Sans,Sans:bold:size=12");
    REQUIRE(font);
    REQUIRE(font.get() != nullptr);

    // Verify valid font metrics
    REQUIRE(font->ascent > 0);
    REQUIRE(font->descent > 0);
}

// ---------------------------------------------------------------------------
// Test 2: Rotated font loads via FcMatrix (validates D-04, A2)
// ---------------------------------------------------------------------------

TEST_CASE("Rotated font loads via FcMatrix", "[xft][poc][rotation]")
{
    x11::DisplayPtr display(XOpenDisplay(std::getenv("DISPLAY")));
    REQUIRE(display);

    auto font = x11::make_xft_font_rotated(display.get(), "Noto Sans,DejaVu Sans,Sans:bold:size=12");
    REQUIRE(font);
    REQUIRE(font.get() != nullptr);

    // Rotated Xft fonts have zero ascent/descent/height because glyph metrics
    // are in a rotated coordinate system. Use XftTextExtentsUtf8 instead.
    XGlyphInfo extents;
    const char* text = "Hello";
    XftTextExtentsUtf8(display.get(), font.get(),
                       reinterpret_cast<const FcChar8*>(text),
                       static_cast<int>(std::strlen(text)), &extents);
    REQUIRE(extents.width > 0);
    REQUIRE(extents.height > 0);
}

// ---------------------------------------------------------------------------
// Test 3: Xft text renders inside shaped window (validates A1, D-05)
// ---------------------------------------------------------------------------

TEST_CASE("Xft text renders inside shaped window", "[xft][poc][shape]")
{
    x11::DisplayPtr display(XOpenDisplay(std::getenv("DISPLAY")));
    REQUIRE(display);

    int screen = DefaultScreen(display.get());
    Window root = RootWindow(display.get(), screen);
    Visual* visual = DefaultVisual(display.get(), screen);
    Colormap colormap = DefaultColormap(display.get(), screen);

    // Create a 200x300 window (simulating a tab)
    Window window = XCreateSimpleWindow(display.get(), root, 0, 0, 200, 300, 0,
                                         BlackPixel(display.get(), screen),
                                         WhitePixel(display.get(), screen));
    REQUIRE(window != None);
    XMapWindow(display.get(), window);
    XSync(display.get(), false);

    // Check Shape extension availability
    int shape_event_base, shape_error_base;
    bool has_shape = XShapeQueryExtension(display.get(), &shape_event_base, &shape_error_base);

    if (has_shape) {
        // Apply a shaped mask (single rectangle covering full window)
        XRectangle rect;
        rect.x = 0;
        rect.y = 0;
        rect.width = 200;
        rect.height = 300;
        XShapeCombineRectangles(display.get(), window, ShapeBounding, 0, 0,
                                &rect, 1, ShapeSet, YXBanded);
        XSync(display.get(), false);
    }

    // Create XftDraw for the window
    XftDraw* raw_draw = XftDrawCreate(display.get(), window, visual, colormap);
    REQUIRE(raw_draw != nullptr);
    x11::XftDrawPtr draw(raw_draw);
    REQUIRE(draw);

    // Allocate XftColor
    x11::XftColorWrap fg_color(display.get(), visual, colormap, "black");
    REQUIRE(fg_color);

    // Load rotated font
    auto font = x11::make_xft_font_rotated(display.get(), "Noto Sans,DejaVu Sans,Sans:bold:size=12");
    REQUIRE(font);

    // Render text -- this MUST NOT crash or produce an X error
    const char* text = "Hello";
    XftDrawStringUtf8(draw.get(), fg_color.get(), font.get(),
                      10, 200,
                      reinterpret_cast<const FcChar8*>(text),
                      static_cast<int>(std::strlen(text)));
    XSync(display.get(), false);

    // Cleanup Xft resources explicitly before destroying the window
    draw.reset();
    XDestroyWindow(display.get(), window);

    // Test passes whether Shape is available or not (graceful on VNC without Shape)
}

// ---------------------------------------------------------------------------
// Test 4: XftColor allocation and cleanup (validates Pitfall 4)
// ---------------------------------------------------------------------------

TEST_CASE("XftColor allocation and cleanup", "[xft][poc][color]")
{
    x11::DisplayPtr display(XOpenDisplay(std::getenv("DISPLAY")));
    REQUIRE(display);

    int screen = DefaultScreen(display.get());
    Visual* visual = DefaultVisual(display.get(), screen);
    Colormap colormap = DefaultColormap(display.get(), screen);

    // Allocate two colors
    x11::XftColorWrap black_color(display.get(), visual, colormap, "black");
    x11::XftColorWrap gray_color(display.get(), visual, colormap, "gray80");

    REQUIRE(black_color);
    REQUIRE(gray_color);

    // Different colors should have different pixel values
    REQUIRE(black_color->pixel != gray_color->pixel);

    // Colors and display go out of scope -- RAII cleanup should not crash
    // (display destroyed last since it's declared first)
}

// ---------------------------------------------------------------------------
// Test 5: Xft text measurement with XftTextExtentsUtf8
//           (validates replacement for XRotTextWidth)
// ---------------------------------------------------------------------------

TEST_CASE("Xft text measurement with XftTextExtentsUtf8", "[xft][poc][metrics]")
{
    x11::DisplayPtr display(XOpenDisplay(std::getenv("DISPLAY")));
    REQUIRE(display);

    auto font = x11::make_xft_font_name(display.get(), "Noto Sans,DejaVu Sans,Sans:bold:size=12");
    REQUIRE(font);

    // Measure non-empty text
    const char* hello = "Hello";
    XGlyphInfo extents;
    XftTextExtentsUtf8(display.get(), font.get(),
                       reinterpret_cast<const FcChar8*>(hello),
                       static_cast<int>(std::strlen(hello)),
                       &extents);
    REQUIRE(extents.width > 0);

    // Measure shorter text -- should have smaller width
    const char* hi = "Hi";
    XGlyphInfo extents2;
    XftTextExtentsUtf8(display.get(), font.get(),
                       reinterpret_cast<const FcChar8*>(hi),
                       static_cast<int>(std::strlen(hi)),
                       &extents2);
    REQUIRE(extents2.width > 0);
    REQUIRE(extents2.width < extents.width);
}

// ---------------------------------------------------------------------------
// Test 6: UTF-8 string rendering does not crash (validates VISL-03)
// ---------------------------------------------------------------------------

TEST_CASE("UTF-8 string rendering does not crash", "[xft][poc][utf8]")
{
    x11::DisplayPtr display(XOpenDisplay(std::getenv("DISPLAY")));
    REQUIRE(display);

    int screen = DefaultScreen(display.get());
    Window root = RootWindow(display.get(), screen);
    Visual* visual = DefaultVisual(display.get(), screen);
    Colormap colormap = DefaultColormap(display.get(), screen);

    Window window = XCreateSimpleWindow(display.get(), root, 0, 0, 200, 200, 0, 0, 0);
    REQUIRE(window != None);
    XMapWindow(display.get(), window);
    XSync(display.get(), false);

    // Create XftDraw
    XftDraw* raw_draw = XftDrawCreate(display.get(), window, visual, colormap);
    REQUIRE(raw_draw != nullptr);
    x11::XftDrawPtr draw(raw_draw);

    // Load rotated font and allocate color
    auto font = x11::make_xft_font_rotated(display.get(), "Noto Sans,DejaVu Sans,Sans:bold:size=12");
    REQUIRE(font);

    x11::XftColorWrap fg_color(display.get(), visual, colormap, "black");
    REQUIRE(fg_color);

    // Render Latin-1 supplement: Cafe with e-acute (UTF-8: 0xC3 0xA9)
    const char* latin_text = "Caf\xC3\xA9";
    XftDrawStringUtf8(draw.get(), fg_color.get(), font.get(),
                      10, 100,
                      reinterpret_cast<const FcChar8*>(latin_text),
                      static_cast<int>(std::strlen(latin_text)));
    XSync(display.get(), false);

    // Render CJK: Japanese (3-byte UTF-8 per character)
    const char* cjk_text = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E";
    XftDrawStringUtf8(draw.get(), fg_color.get(), font.get(),
                      10, 150,
                      reinterpret_cast<const FcChar8*>(cjk_text),
                      static_cast<int>(std::strlen(cjk_text)));
    XSync(display.get(), false);

    // Cleanup before display destruction
    draw.reset();
    XDestroyWindow(display.get(), window);
}

// ---------------------------------------------------------------------------
// Test 7: the SHIPPED font defaults are sized in PIXELS, so a remote session's
// DPI cannot change the rendered size (quick task 261004-vp6)
//
// A fontconfig pattern that says `size=12` says TWELVE POINTS, and a point is
// 1/72 inch -- so the pixel size fontconfig resolves it to depends on the dpi
// the pattern or the server reports. A VNC or RDP server's DPI is not ours to
// predict: the same desktop can come up at 96 on one viewer and 120 on
// another, and the tab label changes size underneath the user.
//
// `pixelsize=13` says thirteen pixels and means it at any DPI. This case
// measures that, on the defaults the binary actually ships, and it reads them
// from Config rather than repeating the literals -- so reverting either default
// to a point-sized pattern reddens it.
//
// THE NEGATIVE CONTROL IS HALF THE CASE. Without it the first assertions would
// also pass on a host or a fontconfig build that ignores the `dpi=` token
// altogether, which would make this case a test of nothing. MEASURED on this
// host with fc-match before the case was written: `size=12` resolves to
// pixelsize 16 at dpi=96 and 20 at dpi=120, `pixelsize=13` to 13 at both.
// ---------------------------------------------------------------------------

namespace {

// ascent+descent of the face fontconfig resolves `pattern` to, or -1 if the
// pattern could not be loaded at all.
int faceHeightAt(Display* d, const std::string& pattern, const char* dpi)
{
    const std::string withDpi = pattern + ":dpi=" + dpi;
    auto font = x11::make_xft_font_name(d, withDpi.c_str());
    if (!font) return -1;
    return font->ascent + font->descent;
}

}  // namespace

TEST_CASE("the shipped font defaults measure the same at 96 and 120 dpi",
          "[xft][dpi]")
{
    x11::DisplayPtr display(XOpenDisplay(std::getenv("DISPLAY")));
    REQUIRE(display);
    Display* d = display.get();

    const Config defaults;

    // --- the two shipped defaults -------------------------------------------
    const int tab96  = faceHeightAt(d, defaults.tabFont,  "96");
    const int tab120 = faceHeightAt(d, defaults.tabFont,  "120");
    const int menu96  = faceHeightAt(d, defaults.menuFont, "96");
    const int menu120 = faceHeightAt(d, defaults.menuFont, "120");

    // --- the superseded point-sized spelling, as the control ----------------
    const std::string control = "DejaVu Sans:bold:size=12";
    const int ctl96  = faceHeightAt(d, control, "96");
    const int ctl120 = faceHeightAt(d, control, "120");

    INFO("tab-font default  '" << defaults.tabFont << "': "
         << tab96 << " px at 96 dpi, " << tab120 << " px at 120 dpi");
    INFO("menu-font default '" << defaults.menuFont << "': "
         << menu96 << " px at 96 dpi, " << menu120 << " px at 120 dpi");
    INFO("control '" << control << "': "
         << ctl96 << " px at 96 dpi, " << ctl120 << " px at 120 dpi");

    // Every pattern resolved to a real face, or nothing below means anything.
    REQUIRE(tab96  > 0);
    REQUIRE(tab120 > 0);
    REQUIRE(menu96  > 0);
    REQUIRE(menu120 > 0);
    REQUIRE(ctl96  > 0);
    REQUIRE(ctl120 > 0);

    // 1. THE SHIPPED DEFAULTS DO NOT FOLLOW THE DPI.
    CHECK(tab96  == tab120);
    CHECK(menu96 == menu120);

    // 2. NEGATIVE CONTROL: the point-sized spelling measurably does. This is
    //    what proves the `dpi=` token reached fontconfig at all, and therefore
    //    that assertion 1 is a measurement rather than a tautology.
    CHECK(ctl96 != ctl120);
    CHECK(ctl120 > ctl96);
}
