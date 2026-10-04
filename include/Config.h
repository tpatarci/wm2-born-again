#pragma once

#include "AppEntry.h"
#include "Rules.h"

#include <string>
#include <vector>

// Parser bookkeeping for the repeated rule-* key groups (RULES-01, D-20).
//
// Two pieces of state, both explicit, because the grouping rule needs both:
// whether a rule is currently open for keys to attach to, and whether the last
// rule key seen was an action (which is what makes the NEXT match key open a
// fresh group). A file boundary resets both -- see Config::applyFile.
struct RuleParseState {
    bool ruleOpen = false;
    bool ruleLastWasAction = false;
};

struct Config {
    // ------------------------------------------------------------------
    // Palette (plan 08.5-02)
    //
    // One silver family, cool-cast, with black ink. Replaces the neutral
    // gray80/gray95 defaults the project carried from upstream.
    //
    // THE COOL CAST IS THE WHOLE TRICK, and it is deliberate: every silver
    // below has R < G < B, so each one reads as aluminium rather than
    // concrete. It costs exactly nothing to draw and degrades gracefully -- a
    // 16-bit VNC session (RGB565) quantises the cast away and leaves plain
    // neutral gray, which is merely the old look rather than a broken one.
    //
    // THE STEP IS NOT UNIFORM, and the arithmetic is written out here rather
    // than left as an almost-true rule of thumb. Counted channel by channel:
    // #C8CACC steps by 2 and 2; #F0F1F3, the lighter frame quick task
    // 261004-vp6 introduced, steps by 1 and 2. The direction is what the
    // family shares; the size of the step is not, and a comment claiming "by 2
    // per channel" was false of the values it covered.
    //
    // menuHighlight IS NOT A SILVER AND IS NOT A TINT. The selected menu row
    // is a solid BAR (quick task 261004-vp6), and its label is drawn in
    // menuBackground rather than menuForeground -- an inversion, not a shade
    // behind unchanged text. Black is the default, which makes it equal to
    // menuForeground; that is fine, because the selected row's text is the
    // only thing drawn inside the bar and it uses the other colour.
    //
    // THE ONE SELF-INFLICTED WAY TO BREAK IT, named once rather than guarded:
    // set menu-highlight equal to menu-background and your own selected label
    // becomes invisible, because the bar and its ink are then the same colour.
    // Reversible by changing one key, already reachable for any self-inverting
    // palette, and a validator rejecting equal colours would be a policy about
    // taste rather than a correctness check.
    //
    // The frame is LIGHTER than the tab on purpose. That value order is what
    // says "lit from above", and under the flat look (quick task 261004-vp6)
    // it is the whole of where the metallic impression comes from: there is no
    // raised edge any more, so the two flat surfaces and the black outline
    // between them carry it alone. A banded gradient was considered and
    // rejected: across a ~21 px tab, two or three bands are ~7 px each, which
    // reads as stripes rather than sheen and either collapses or visibly bands
    // once VNC quantises it.
    //
    // Every surface that carries black text keeps it above 8.8:1 contrast.
    // The menu-highlight bar is the exception by construction: it carries no
    // black text, because its label is drawn in menuBackground -- #C8CACC on
    // black, which is about 12.8:1.
    // ------------------------------------------------------------------

    // Colors (tab)
    std::string tabForeground   = "#000000";
    std::string tabBackground   = "#C8CACC";
    // Colors (frame)
    std::string frameBackground = "#F0F1F3";
    std::string buttonBackground = "#F0F1F3";
    std::string borders         = "#000000";
    // Colors (menu)
    std::string menuForeground  = "#000000";
    std::string menuBackground  = "#C8CACC";
    std::string menuHighlight   = "#000000";
    std::string menuBorders     = "#000000";

    // ------------------------------------------------------------------
    // Fonts (plan 09-01, CONF-02)
    //
    // The fonts this window manager draws with lived as string literals in the
    // code that draws with them -- rung 1 of Border::loadTabFont() and the
    // menu-font load in WindowManager::initialiseScreen() -- until this plan.
    // They are here now because the Phase 9 configuration GUI edits fonts
    // (CGUI-03) and nothing it could edit existed: `grep -ci font
    // src/Config.cpp` returned 0.
    //
    // DISC-05a: the value grammar is a fontconfig pattern, handed to
    // XftFontOpenName unchanged. That is the same grammar the two literals
    // already used, and it is why there is no separate size key -- a size key
    // would be a second way of saying something the pattern already says, and
    // the two could disagree.
    //
    // SIZED IN PIXELS, NOT POINTS (quick task 261004-vp6). A pattern that says
    // size=12 says twelve POINTS, and a point is 1/72 inch, so the pixel size
    // fontconfig resolves it to follows whatever DPI the server reports. A VNC
    // or RDP server's DPI is not ours to predict: MEASURED with fc-match,
    // size=12 resolves to pixelsize 16 at 96 dpi and 20 at 120 dpi, so the
    // same desktop came up with a visibly different tab label depending on the
    // viewer. pixelsize=13 says thirteen pixels and means it at any DPI, which
    // is what a window manager sized in pixels everywhere else should have
    // said all along. Pinned by the DPI case in tests/test_xft_poc.cpp, with
    // the old point-sized spelling kept there as its negative control.
    //
    // DISC-05b IS SUPERSEDED AND WAS THE MOST MISLEADING SENTENCE IN THIS
    // FILE. It said each default IS the literal the binary hardcoded, character
    // for character, and that a user with no config file sees no change
    // whatsoever. That stopped being true here. The literals it described,
    // recorded so the change is legible:
    //     tab-font   Ubuntu,Noto Sans,DejaVu Sans,Sans:bold:size=12
    //     menu-font  Ubuntu,Noto Sans,DejaVu Sans,Sans:size=12
    // A user with no config file DOES see a change: a slightly smaller, DejaVu
    // label at a size that no longer moves with the server.
    //
    // WHY DejaVu RATHER THAN THE OLD FAMILY LIST: fonts-dejavu-core is on every
    // Ubuntu image including the minimal VPS ones this window manager targets,
    // so the shipped default resolves to a real readable file without
    // depending on a desktop font package being present. The four-rung
    // fallback ladder is untouched and still catches a host where even that is
    // missing.
    //
    // D-8.5-01: `tab-font` and `menu-font` are permanent spellings. Key names are
    // free to choose before v1.0 and fixed after, and there will be no
    // deprecated aliases.
    //
    // The two still differ by exactly one token: the tab is drawn BOLD and the
    // menu is not. That is not an oversight to tidy up -- bold survives a
    // RENDER-less remote server where lighter weights go ragged (measured,
    // 08.5-02), and the tab label is the text that has to stay legible
    // sideways.
    // ------------------------------------------------------------------
    std::string tabFont  = "DejaVu Sans:bold:pixelsize=13";
    std::string menuFont = "DejaVu Sans:pixelsize=13";

    // Focus policy
    //
    // D-17: these three values were CORRECTED to describe what the binary
    // actually does. Until FOCUS-02 wired them up (plan 08-07) nothing in the
    // runtime read them, so the shipped defaults could say all-false while the
    // WM unconditionally performed pointer focus with auto-raise, and raising
    // was fused into focusing. The literal previous values had never reflected
    // reality; resolving that contradiction in their favour would have silently
    // changed behaviour for every existing user the moment the gates landed.
    //
    // So: pointer focus (click-to-focus off), auto-raise on, raise-on-focus on
    // -- which is exactly what a user with no config file got before, and gets
    // now.
    bool clickToFocus = false;
    bool raiseOnFocus = true;
    bool autoRaise    = true;
    // FOCUS-01 (plan 08-08): focus-stealing prevention, ON by default.
    //
    // Unlike the three above, this one defaults to its safe value rather than to
    // the previous behaviour, because it IS the mitigation: a default of false
    // would ship the feature switched off. The switch exists at all for D-19's
    // reason -- a user whose important legacy applications set no
    // _NET_WM_USER_TIME may prefer the old unconditional grant to a correct
    // refusal, and that is their informed choice to make (threat T-8-OFF).
    bool focusStealingPrevention = true;
    // Timing (milliseconds)
    int autoRaiseDelay      = 400;
    int pointerStoppedDelay = 80;

    // How long a tab-button press must be HELD before it deletes rather than
    // hides. 1500 here was upstream wm2's CONFIG_DESTROY_WINDOW_DELAY, carried
    // over verbatim from 1997 and never revisited until an operator sat with it
    // in a real session and reported it as "way too long" -- this is software
    // for adept users, and a delay tuned for hesitancy taxes every close.
    //
    // 400 is not the minimum the parser accepts (that is 1), and the floor is
    // real: the failure is ASYMMETRIC. A false hide costs nothing -- the window
    // is one menu row away. A false DELETE sends WM_DELETE_WINDOW and can lose
    // the user's work. An ordinary click runs 50-150 ms, so a threshold much
    // under ~250 ms would let a slightly sticky click destroy a window.
    //
    // 400 sits clear of that, is 3.75x faster than the value it replaces, and
    // matches autoRaiseDelay above -- one fewer arbitrary constant in the file.
    int destroyWindowDelay  = 400;
    // Frame
    int frameThickness = 7;
    // Commands
    std::string newWindowCommand = "xterm";
    bool execUsingShell = false;

    // Manual menu entries (APPS-04)
    std::vector<AppEntry> manualMenuEntries;

    // Window rules (RULES-01), in file order across every config layer. The
    // fold in src/Rules.cpp is later-wins, so this order is load-bearing.
    std::vector<WindowRule> rules;

    // Grouping state for the sequential applyKeyValue() entry point below.
    // Bookkeeping, not a setting: applyFile() uses its own per-file instance.
    RuleParseState ruleParseState;

    // Load config: defaults -> system config -> user config -> CLI overrides
    static Config load(int argc, char** argv);

    // Apply a config file (only sets keys present in the file)
    void applyFile(const std::string& path);

    // Apply a single key=value pair. Rule grouping uses the member state above,
    // so repeated sequential calls behave exactly like consecutive lines of one
    // file -- which is what makes rule parsing deterministic in unit tests.
    void applyKeyValue(const std::string& key, const std::string& value);

    // As above, but with caller-owned grouping state. applyFile() uses this so
    // an open rule cannot survive the end of the file that opened it.
    void applyKeyValue(const std::string& key, const std::string& value,
                       RuleParseState& ruleState);

    // Apply CLI arguments (getopt_long)
    void applyCliArgs(int argc, char** argv);
};

// XDG path resolution (exposed for testing)
std::string xdgConfigHome();
std::vector<std::string> xdgConfigDirs();

// EVERY configuration FILE Config::load() reads, in the order it reads them:
// the XDG_CONFIG_DIRS files lowest-precedence first, then the user's own file
// last. The CLI layer is not a file and so is not here.
//
// WHY THIS IS A FUNCTION RATHER THAN A LOOP IN load(). A reload must refuse
// when a file that EXISTS cannot be read, or Config::applyFile() skips it in
// silence and the window manager reports a successful reload that quietly
// dropped a whole layer. That check and the load itself have to walk the same
// list, and a second hand-written copy of "which files are the layers" is
// exactly how the two come to disagree -- one file added to load() and not to
// the preflight is a layer nothing checks again (Codex pass 7, P2).
std::vector<std::string> configFileLayerPaths();


// -----------------------------------------------------------------------------
// The settable surface, described rather than re-listed (plan 09-04)
// -----------------------------------------------------------------------------
//
// Everything below is a VIEW of the option table src/Config.cpp already carries
// -- the one plan 08-13 introduced so that getopt_long()'s array and the --help
// text could not drift apart. The configuration socket's `set` needs the same
// knowledge in a third place (what kind of value a key takes, and for an
// integer what range the parser clamps it to), and a third hand-written list
// would drift exactly as the first two used to.
//
// So there is no new table here. configKeySpecs() is built from kOptionSpecs,
// and the integer bounds moved INTO those rows: Config::applyKeyValue() and
// Config::applyCliArgs() now read 1..50 and 1..60000 from the same rows this
// view exposes, instead of each spelling the numbers out again.
//
// Why the socket needs it at all: the prohibition this phase carries is that a
// value arriving over the socket must never reach window-manager state without
// the validation the config file performs. The parser's answer to a bad value
// is to CLAMP it and warn on stderr -- correct for a file being read at
// startup, wrong for a request that has a client waiting for an answer. The
// dispatcher therefore checks kind and range BEFORE handing the value to the
// very same Config::applyKeyValue(), and refuses what the parser would have
// silently corrected. Stricter than the file, never looser.

enum class ConfigValueKind {
    String,   // taken verbatim
    Boolean,  // true/false/1/0
    Integer   // clamped to [minValue, maxValue]
};

struct ConfigKeySpec {
    std::string     name;
    ConfigValueKind kind = ConfigValueKind::String;
    int             minValue = 0;   // Integer only
    int             maxValue = 0;   // Integer only
    std::string     summary;        // the same one --help prints
};

// Every key the parser accepts as a single key=value setting, in the order the
// option table declares them. `rule-*` and `menu-entry-*` are deliberately
// absent: they are ordered repeated groups rather than independent settings,
// which is the same reason configFileManagedKeys() omits them. A
// [wm_config_live] case asserts the two lists name exactly the same keys.
const std::vector<ConfigKeySpec>& configKeySpecs();

// The spec for one key, or nullptr for a key that is not a single setting.
const ConfigKeySpec* configKeySpecFor(const std::string& key);

// -----------------------------------------------------------------------------
// The manual menu entries, as ONE value (plan 09-05, D-12)
// -----------------------------------------------------------------------------
//
// `menu-entry-name` / `-command` / `-category` are an ORDERED, STATEFUL
// accumulator: a name opens an entry and the other two fill in whichever entry
// is currently open. That grammar reads perfectly as consecutive lines of a
// file and does not survive being cut into independent request-reply messages,
// which is why configKeySpecs() does not name them and why the socket cannot
// treat them as settings.
//
// So the whole list travels as ONE value, in the file's own key order, records
// separated by ';':
//
//   menu-entry-name=Editor;menu-entry-command=/usr/bin/vim;menu-entry-category=Custom;menu-entry-name=Mail;menu-entry-command=/usr/bin/mutt
//
// WHOLESALE REPLACEMENT, never a mutation of one row. That is what makes the
// operation idempotent -- sending the same list twice leaves the same list --
// and what lets a settings window's Add, Edit and Remove rows map onto it with
// no per-row protocol and no row identity to keep in sync.
//
// The separator has NO ESCAPE, deliberately: a ';' inside a value would need
// one, an escape needs a second grammar, and a second grammar is a second thing
// to get wrong. A command that must contain a ';' is written into the config
// file directly, where the accumulator's own line-per-key form has no
// separator to collide with.

// Render `config`'s manual entries in the grammar above. Empty for a
// configuration with no manual entries, which is the same value that clears
// them.
std::string configMenuEntriesValue(const Config& config);

// Parse the grammar above into `out`, replacing whatever it held. False with a
// human-readable reason in `reasonOut` for a record that is not one of the
// three keys, for a record with no '=', or for a value that opens a command or
// a category before any name -- each of which the file parser answers with a
// warning to a stderr nobody is reading, and which over the socket must be a
// refusal instead.
bool parseMenuEntriesValue(const std::string& value,
                           std::vector<AppEntry>& out,
                           std::string& reasonOut);

// The permanent spelling of the key that carries the value above. Named here
// rather than spelled as a literal at each of its three use sites (the socket's
// get arm, its set arm, and wm2-ctl's help).
inline constexpr const char* kMenuEntriesKey = "menu-entries";


// The categories the running root menu currently shows, ';'-separated, in the
// menu's own order -- alphabetical with "Custom" last (plan 09-07, D-12).
//
// READ-ONLY, and read-only by construction rather than by a check: it is not
// one of the keys configKeySpecs() names, so applyConfigSet() refuses it as an
// unknown setting like any other non-setting. That is the correct shape,
// because the list is DERIVED -- from what discovery found plus the manual
// entry list -- and setting a view of two things would mean setting neither.
//
// It exists because D-12 asks the settings window's category dropdown to offer
// "the categories the WM currently shows". The GUI cannot compute that: it
// would have to re-run the .desktop and /usr/bin scan, and a second
// implementation of discovery is a second answer that disagrees with the first
// the moment a .desktop file changes. So the window manager answers, and the
// GUI asks.
//
// This adds no TWELFTH MESSAGE TYPE -- the eleven the version-1 contract froze
// are untouched. It adds one key to the vocabulary the existing `get` verb
// carries, which is exactly what plan 09-05 did for `menu-entries`.
inline constexpr const char* kMenuCategoriesKey = "menu-categories";


// Split a command string into an argument vector the way the config file's
// `menu-entry-command` arm splits it: on whitespace, with NO shell evaluation,
// no quote handling and no field codes.
//
// One implementation with two callers -- Config::applyKeyValue() and the
// settings window's menu-entry dialog -- so a command typed into the window and
// the same line written into the file cannot become two different argument
// vectors. The dialog SHOWS the result of this call (T-9-40), which is the
// honest form of the guarantee that a manual entry is never shell-evaluated: a
// semicolon the user typed is visibly one argument rather than the start of a
// second command.
std::vector<std::string> configTokeniseCommand(const std::string& command);


// The EFFECTIVE value of one key, spelled the way the config file would spell
// it: a boolean as "true" or "false", an integer in decimal, a string
// verbatim. False for a key configKeySpecs() does not name, leaving `out`
// untouched -- which is how a caller distinguishes "unknown key" from "set to
// the empty string".
bool configValueForKey(const Config& config, const std::string& key,
                       std::string& out);
