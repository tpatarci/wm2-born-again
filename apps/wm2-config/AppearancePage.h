#pragma once

// The Appearance page: what the desktop looks like (D-09, D-10, D-13).
//
// This is the ONE file in wm2-config that is allowed to be full of widgets. The
// model it edits (FormState) and the client it commits through (ProtocolClient)
// both know nothing about GTK, which is what keeps their behaviour testable
// without a display; this class is the seam where the toolkit meets them.
//
// The GTK signal handlers are static member functions taking the owning object
// through `user_data`, the standard plain-C-GTK-from-C++ idiom (09-RESEARCH.md
// Pattern 1). gtkmm would give typed signals and RAII widget ownership for the
// price of a second toolkit dependency, and this window is one window with
// three pages.

#include "FormState.h"

#include <gtk/gtk.h>

#include <functional>
#include <string>
#include <vector>


class AppearancePage {
public:
    // A control committed a value (D-05: colour chosen, field left, slider
    // released). The owner decides what that means -- in practice, sending it
    // to the running window manager immediately and touching no file.
    using CommitHandler =
        std::function<void(const std::string& key, const std::string& value)>;

    // Something worth telling the user, for the window's status line.
    using StatusHandler = std::function<void(const std::string& message)>;

    AppearancePage(FormState& form, CommitHandler onCommit, StatusHandler onStatus);
    ~AppearancePage();

    AppearancePage(const AppearancePage&) = delete;
    AppearancePage& operator=(const AppearancePage&) = delete;

    // The page's root widget, owned by the notebook it is added to.
    GtkWidget* widget() const { return m_root; }

    // Re-render every control from the model. Called after a revert, after a
    // save, and when a reload notice moves the effective values underneath an
    // open window (D-08).
    void refreshFromForm();

    // D-13's per-page half: every setting on this page marked for removal from
    // the user file, through the SAME per-setting reset the arrow beside each
    // control performs.
    void resetAll();

private:
    // What kind of control a key gets, and therefore how its value is read
    // back out of the toolkit's vocabulary and into the config file's.
    enum class Kind { Colour, Font, Thickness };

    struct Row {
        std::string key;
        Kind        kind = Kind::Colour;
        GtkWidget*  chooser = nullptr;   // the native chooser
        GtkWidget*  raw = nullptr;       // the config-file spelling, editable
        GtkWidget*  reset = nullptr;
        GtkWidget*  label = nullptr;     // marked when a reload moved the file
        std::string labelText;
        // The control's OWN sentence, for a row that has no raw field.
        //
        // DISC-08's origin line goes onto the raw field when there is one, and
        // onto the control itself when there is not -- and
        // gtk_widget_set_tooltip_text() REPLACES, so the sentence the row was
        // built with was gone the first time the row was rendered (A3). Stored
        // once, where it is set, and appended to by renderRow(). Empty for a
        // row whose control carries no sentence of its own.
        std::string baseTooltip;
        AppearancePage* owner = nullptr; // for the static callbacks
    };

    void addColourRow(GtkWidget* grid, int line, const std::string& key,
                      const std::string& label);
    void addFontRow(GtkWidget* grid, int line, const std::string& key,
                    const std::string& label);
    void addThicknessRow(GtkWidget* grid, int line, const std::string& key,
                         const std::string& label);
    GtkWidget* addRawField(Row* row);
    GtkWidget* addResetButton(Row* row);
    void renderRow(Row& row);
    // D-08: the file moved underneath an edit this user has not saved. Marked
    // rather than replaced, which is the whole of that decision.
    void markRow(Row& row, bool stale);
    void commit(Row& row, const std::string& value);
    void reset(Row& row);
    Row* rowFor(GtkWidget* widget);

    static void onColourSet(GtkColorButton* button, gpointer userData);
    static void onFontSet(GtkFontButton* button, gpointer userData);
    static gboolean onScaleReleased(GtkWidget* scale, GdkEvent* event,
                                    gpointer userData);
    static void onRawActivate(GtkEntry* entry, gpointer userData);
    static gboolean onRawFocusOut(GtkWidget* entry, GdkEvent* event,
                                  gpointer userData);
    static void onResetClicked(GtkButton* button, gpointer userData);
    static void onResetAllClicked(GtkButton* button, gpointer userData);

    void commitRawField(GtkWidget* entry);

    FormState&    m_form;
    CommitHandler m_onCommit;
    StatusHandler m_onStatus;
    GtkWidget*    m_root = nullptr;
    std::vector<Row*> m_rows;
    std::vector<std::string> m_keys;

    // True while the page is writing values INTO its own widgets. Every
    // handler returns early when it is set, because a programmatic
    // gtk_entry_set_text() emits the same signals a user's typing does, and
    // without this a refresh would commit every value it displayed straight
    // back to the window manager.
    bool m_updating = false;
};


// The two vocabularies, converted in ONE place (D-10).
//
// A GdkRGBA is what the toolkit's colour chooser speaks; "#RRGGBB" is what the
// config file spells and what XParseColor reads back. Alpha is dropped on
// purpose: the window manager allocates opaque pixels, so a translucent choice
// would be a promise the desktop cannot keep.
std::string configColourFromRgba(const GdkRGBA& rgba);

// The inverse. False for a spelling the toolkit cannot parse, leaving `out`
// untouched -- which is how a raw field distinguishes "the user is mid-word"
// from "the user meant black".
bool rgbaFromConfigColour(const std::string& spelling, GdkRGBA& out);

// THE GATE BETWEEN THE TWO GRAMMARS (X1). One spelling in, the X11 spelling of
// the same colour out; false, with `out` untouched, for a spelling the toolkit
// cannot parse at all.
//
// The two grammars are NOT the same grammar. GDK accepts CSS -- rgb(200,202,204),
// rgba(...), and more -- and XParseColor accepts none of it, so a value that got
// past a GDK-only check was refused by the running window manager, kept by the
// form anyway, written to the user's file by Save, and then handed at the next
// startup to Border::allocateXftColors(), which calls fatal() on a tab colour
// the server cannot parse. A typed colour could stop the desktop from starting.
//
// Everything is therefore canonicalised, NAMES INCLUDED. Deciding that a name is
// safe to keep as a name would mean asking XParseColor, and XParseColor needs a
// display this program has no reason to open; the conversion is exact either way,
// because a name resolves to the same 8-bit channels on both sides.
bool configCanonicalColour(const std::string& spelling, std::string& out);


// The OTHER pair of vocabularies, converted in ONE place.
//
// The toolkit speaks Pango font descriptions ("DejaVu Sans Bold 12"); the
// config file and the window manager speak fontconfig patterns
// ("DejaVu Sans:bold:pixelsize=13"). Both the chooser and the raw field go
// through this pair, because two conversions are how two spellings of one font
// start disagreeing.
//
// The conversion is LOSSY IN ONE DIRECTION and deliberately so: a fontconfig
// pattern can name a fallback LIST of families and a Pango description cannot,
// so the chooser is shown the first family and the raw field keeps the whole
// list. Which is why the raw field -- not the chooser -- is what gets saved and
// sent when the user typed it: their fallback chain survives.
std::string fontDescriptionFromConfigPattern(const std::string& pattern);
std::string configPatternFromFontDescription(const std::string& description);
