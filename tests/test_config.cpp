#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "Config.h"
#include "ConfigProtocol.h"
#include "ConfigFileWriter.h"   // kConfigFileMaxValueBytes -- the per-value bound the wire must respect too

#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

// Helper: write a temp config file and return its path
static std::string writeTempConfig(const std::string& content) {
    static int counter = 0;
    std::string path = "/tmp/wm2-test-config-" + std::to_string(++counter) + ".cfg";
    std::ofstream out(path);
    out << content;
    out.close();
    return path;
}

// Helper: clean up temp file
static void removeTempFile(const std::string& path) {
    std::remove(path.c_str());
}

// Helper: capture everything written to stderr for the lifetime of the object.
//
// The warning-emitting cases assert on the captured TEXT rather than merely on
// the resulting Config, because a warning that is never emitted is exactly the
// silent-failure mode they exist to catch. The same shape as
// tests/test_rules.cpp's StderrCapture, duplicated per the project's
// per-translation-unit convention for small test helpers.
class StderrCapture {
public:
    StderrCapture() {
        static int counter = 0;
        m_path = "/tmp/wm2-test-config-stderr-" + std::to_string(::getpid()) +
                 "-" + std::to_string(++counter) + ".txt";
        std::fflush(stderr);
        m_saved = dup(STDERR_FILENO);
        m_fd = open(m_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (m_fd >= 0) dup2(m_fd, STDERR_FILENO);
    }

    StderrCapture(const StderrCapture&) = delete;
    StderrCapture& operator=(const StderrCapture&) = delete;

    ~StderrCapture() {
        std::fflush(stderr);
        if (m_saved >= 0) { dup2(m_saved, STDERR_FILENO); close(m_saved); }
        if (m_fd >= 0) close(m_fd);
        std::remove(m_path.c_str());
    }

    std::string text() const {
        std::fflush(stderr);
        std::ifstream in(m_path);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

private:
    std::string m_path;
    int m_saved = -1;
    int m_fd = -1;
};

// The encoded `value` reply that `get menu-entries` would produce for `cfg`.
static std::string menuEntriesReplyLine(const Config& cfg) {
    ConfigMessage reply;
    reply.type  = ConfigMessageType::Value;
    reply.key   = kMenuEntriesKey;
    reply.value = configMenuEntriesValue(cfg);
    return configProtocolEncode(reply);
}

// =============================================================================
// Test 1: Config struct initializes with all 18 default values
// =============================================================================
TEST_CASE("Config defaults match upstream Config.h", "[config]") {
    Config cfg;

    // Colors (tab) - 2 settings
    REQUIRE(cfg.tabForeground == "#000000");
    REQUIRE(cfg.tabBackground == "#C8CACC");

    // Colors (frame) - 3 settings
    REQUIRE(cfg.frameBackground == "#F0F1F3");
    REQUIRE(cfg.buttonBackground == "#F0F1F3");
    REQUIRE(cfg.borders == "#000000");

    // Colors (menu) - 4 settings
    REQUIRE(cfg.menuForeground == "#000000");
    REQUIRE(cfg.menuBackground == "#C8CACC");
    REQUIRE(cfg.menuHighlight == "#000000");
    REQUIRE(cfg.menuBorders == "#000000");

    // Focus policy - 3 settings.
    //
    // D-17: raiseOnFocus and autoRaise assert TRUE deliberately. They read false
    // until plan 08-07, but nothing in the runtime consulted them, so those
    // assertions were pinning values the WM ignored -- it did pointer focus with
    // auto-raise and fused raising into focusing regardless. The defaults were
    // corrected to describe the shipped binary; these assertions follow, and are
    // stated explicitly rather than loosened.
    REQUIRE(cfg.clickToFocus == false);
    REQUIRE(cfg.raiseOnFocus == true);
    REQUIRE(cfg.autoRaise == true);

    // FOCUS-01 (plan 08-08): focus-stealing prevention defaults ON. It is an
    // access-control mitigation, so the safe value is the default one; the
    // switch exists (D-19) for users whose legacy X clients set no
    // _NET_WM_USER_TIME and who would rather have the old always-grant
    // behaviour than a correctly-refused window.
    REQUIRE(cfg.focusStealingPrevention == true);

    // Timing (milliseconds) - 3 settings
    REQUIRE(cfg.autoRaiseDelay == 400);
    REQUIRE(cfg.pointerStoppedDelay == 80);
    // 400, not upstream's 1500. Changed in plan 08.5-02 after the operator sat
    // with it in a real remote session; see the reasoning in include/Config.h.
    REQUIRE(cfg.destroyWindowDelay == 400);

    // Frame - 1 setting
    REQUIRE(cfg.frameThickness == 7);

    // Commands - 2 settings
    REQUIRE(cfg.newWindowCommand == "xterm");
    REQUIRE(cfg.execUsingShell == false);
}

// =============================================================================
// Test 2: applyFile() parses a key=value file and overrides defaults
// =============================================================================
TEST_CASE("applyFile parses key=value and overrides defaults", "[config]") {
    std::string path = writeTempConfig(
        "tab-foreground = white\n"
        "frame-thickness = 10\n"
        "auto-raise-delay = 200\n"
        "click-to-focus = true\n"
        "new-window-command = alacritty\n"
    );

    Config cfg;
    cfg.applyFile(path);

    REQUIRE(cfg.tabForeground == "white");
    REQUIRE(cfg.frameThickness == 10);
    REQUIRE(cfg.autoRaiseDelay == 200);
    REQUIRE(cfg.clickToFocus == true);
    REQUIRE(cfg.newWindowCommand == "alacritty");

    // Unset keys retain defaults (D-17: auto-raise defaults to true)
    REQUIRE(cfg.tabBackground == "#C8CACC");
    REQUIRE(cfg.autoRaise == true);

    removeTempFile(path);
}

// =============================================================================
// Test 3: applyFile() skips comment lines (# prefix) and blank lines
// =============================================================================
TEST_CASE("applyFile skips comments and blank lines", "[config]") {
    std::string path = writeTempConfig(
        "# This is a comment\n"
        "\n"
        "  # Indented comment\n"
        "\t\n"
        "tab-foreground = red\n"
        "  \n"
        "# Another comment\n"
        "frame-thickness = 5\n"
    );

    Config cfg;
    cfg.applyFile(path);

    REQUIRE(cfg.tabForeground == "red");
    REQUIRE(cfg.frameThickness == 5);

    removeTempFile(path);
}

// =============================================================================
// Test 4: applyFile() warns on lines without '=' but continues parsing
// =============================================================================
TEST_CASE("applyFile warns on lines without equals sign", "[config]") {
    std::string path = writeTempConfig(
        "this line has no equals\n"
        "tab-foreground = blue\n"
        "another bad line\n"
        "frame-thickness = 3\n"
    );

    Config cfg;
    cfg.applyFile(path);

    // Should still parse valid lines after warnings
    REQUIRE(cfg.tabForeground == "blue");
    REQUIRE(cfg.frameThickness == 3);

    removeTempFile(path);
}

// =============================================================================
// Test 5: applyFile() silently skips non-existent files (no error)
// =============================================================================
TEST_CASE("applyFile silently skips non-existent files", "[config]") {
    Config cfg;
    // Should not throw or crash
    cfg.applyFile("/tmp/wm2-nonexistent-config-file-xyz123.cfg");

    // All defaults should remain
    REQUIRE(cfg.tabForeground == "#000000");
    REQUIRE(cfg.frameThickness == 7);
    REQUIRE(cfg.newWindowCommand == "xterm");
}

// =============================================================================
// Test 6: xdgConfigHome() returns $XDG_CONFIG_HOME when set and absolute
// =============================================================================
TEST_CASE("xdgConfigHome returns XDG_CONFIG_HOME when set and absolute", "[config][xdg]") {
    // Save original
    char* origXdg = std::getenv("XDG_CONFIG_HOME");

    std::string origXdgStr;
    if (origXdg) origXdgStr = origXdg;

    setenv("XDG_CONFIG_HOME", "/custom/config", 1);

    std::string result = xdgConfigHome();
    REQUIRE(result == "/custom/config");

    // Restore
    if (origXdgStr.empty()) {
        unsetenv("XDG_CONFIG_HOME");
    } else {
        setenv("XDG_CONFIG_HOME", origXdgStr.c_str(), 1);
    }
}

// =============================================================================
// Test 7: xdgConfigHome() falls back to $HOME/.config when XDG_CONFIG_HOME unset
// =============================================================================
TEST_CASE("xdgConfigHome falls back to HOME/.config", "[config][xdg]") {
    char* origXdg = std::getenv("XDG_CONFIG_HOME");
    char* origHome = std::getenv("HOME");

    std::string origXdgStr, origHomeStr;
    if (origXdg) origXdgStr = origXdg;
    if (origHome) origHomeStr = origHome;

    unsetenv("XDG_CONFIG_HOME");
    setenv("HOME", "/home/testuser", 1);

    std::string result = xdgConfigHome();
    REQUIRE(result == "/home/testuser/.config");

    // Restore
    if (origXdgStr.empty()) {
        unsetenv("XDG_CONFIG_HOME");
    } else {
        setenv("XDG_CONFIG_HOME", origXdgStr.c_str(), 1);
    }
    if (!origHomeStr.empty()) {
        setenv("HOME", origHomeStr.c_str(), 1);
    }
}

// =============================================================================
// Test 8: xdgConfigDirs() parses colon-separated $XDG_CONFIG_DIRS
// =============================================================================
TEST_CASE("xdgConfigDirs parses colon-separated XDG_CONFIG_DIRS", "[config][xdg]") {
    char* orig = std::getenv("XDG_CONFIG_DIRS");
    std::string origStr;
    if (orig) origStr = orig;

    setenv("XDG_CONFIG_DIRS", "/usr/local/etc/xdg:/etc/xdg", 1);

    auto dirs = xdgConfigDirs();
    REQUIRE(dirs.size() == 2);
    REQUIRE(dirs[0] == "/usr/local/etc/xdg");
    REQUIRE(dirs[1] == "/etc/xdg");

    // Restore
    if (origStr.empty()) {
        unsetenv("XDG_CONFIG_DIRS");
    } else {
        setenv("XDG_CONFIG_DIRS", origStr.c_str(), 1);
    }
}

// =============================================================================
// Test 9: xdgConfigDirs() defaults to {"/etc/xdg"} when env unset
// =============================================================================
TEST_CASE("xdgConfigDirs defaults to /etc/xdg", "[config][xdg]") {
    char* orig = std::getenv("XDG_CONFIG_DIRS");
    std::string origStr;
    if (orig) origStr = orig;

    unsetenv("XDG_CONFIG_DIRS");

    auto dirs = xdgConfigDirs();
    REQUIRE(dirs.size() == 1);
    REQUIRE(dirs[0] == "/etc/xdg");

    // Restore
    if (origStr.empty()) {
        unsetenv("XDG_CONFIG_DIRS");
    } else {
        setenv("XDG_CONFIG_DIRS", origStr.c_str(), 1);
    }
}

// =============================================================================
// Test 10: Config::load applies system config first, user config second
// =============================================================================
TEST_CASE("Config load precedence: user config overrides system config", "[config]") {
    // Set up system config dir with a test config
    char* origDirs = std::getenv("XDG_CONFIG_DIRS");
    char* origHome = std::getenv("XDG_CONFIG_HOME");
    char* origUserHome = std::getenv("HOME");

    std::string origDirsStr, origHomeStr, origUserHomeStr;
    if (origDirs) origDirsStr = origDirs;
    if (origHome) origHomeStr = origHome;
    if (origUserHome) origUserHomeStr = origUserHome;

    // System config: tab-foreground = system-color
    std::string sysDir = "/tmp/wm2-test-sys-" + std::to_string(std::rand());
    std::string sysSubdir = sysDir + "/wm2-born-again";
    std::string userDir = "/tmp/wm2-test-user-" + std::to_string(std::rand());
    std::string userSubdir = userDir + "/wm2-born-again";

    // Create directories. Not a shell-out: std::system()'s result is
    // warn_unused_result under -O2, and the four calls here were the release
    // gate's only warning lines once it compiled every unit (08.5-08, capture 5).
    std::filesystem::create_directories(sysSubdir);
    std::filesystem::create_directories(userSubdir);

    // Write system config
    {
        std::ofstream out(sysSubdir + "/config");
        out << "tab-foreground = system-color\n";
        out << "frame-thickness = 15\n";
    }

    // Write user config (overrides some, adds new)
    {
        std::ofstream out(userSubdir + "/config");
        out << "tab-foreground = user-color\n";
        // Written FALSE against a true default (D-17), so this assertion keeps
        // discriminating. `auto-raise = true` would now agree with the default
        // and would pass whether the file was read or ignored.
        out << "auto-raise = false\n";
    }

    setenv("XDG_CONFIG_DIRS", sysDir.c_str(), 1);
    setenv("XDG_CONFIG_HOME", userDir.c_str(), 1);

    // Load with zero CLI args
    char* argv[] = {(char*)"wm2-born-again"};
    Config cfg = Config::load(1, argv);

    // User config overrides system for tab-foreground
    REQUIRE(cfg.tabForeground == "user-color");
    // System config value preserved (not overridden by user)
    REQUIRE(cfg.frameThickness == 15);
    // User config adds new setting, overriding the built-in default
    REQUIRE(cfg.autoRaise == false);
    // Defaults preserved where neither config sets
    REQUIRE(cfg.tabBackground == "#C8CACC");

    // Clean up
    std::filesystem::remove_all(sysDir);
    std::filesystem::remove_all(userDir);

    // Restore
    if (origDirsStr.empty()) unsetenv("XDG_CONFIG_DIRS");
    else setenv("XDG_CONFIG_DIRS", origDirsStr.c_str(), 1);
    if (origHomeStr.empty()) unsetenv("XDG_CONFIG_HOME");
    else setenv("XDG_CONFIG_HOME", origHomeStr.c_str(), 1);
    if (!origUserHomeStr.empty()) setenv("HOME", origUserHomeStr.c_str(), 1);
}

// =============================================================================
// Test 11: Config::load works with zero config files present
// =============================================================================
TEST_CASE("Config load with no config files returns pure defaults", "[config]") {
    char* origDirs = std::getenv("XDG_CONFIG_DIRS");
    char* origHome = std::getenv("XDG_CONFIG_HOME");

    std::string origDirsStr, origHomeStr;
    if (origDirs) origDirsStr = origDirs;
    if (origHome) origHomeStr = origHome;

    // Point to non-existent dirs
    setenv("XDG_CONFIG_DIRS", "/tmp/wm2-noexist-sys-xyz", 1);
    setenv("XDG_CONFIG_HOME", "/tmp/wm2-noexist-user-xyz", 1);

    char* argv[] = {(char*)"wm2-born-again"};
    Config cfg = Config::load(1, argv);

    // All defaults
    REQUIRE(cfg.tabForeground == "#000000");
    REQUIRE(cfg.frameThickness == 7);
    REQUIRE(cfg.autoRaiseDelay == 400);
    REQUIRE(cfg.newWindowCommand == "xterm");
    REQUIRE(cfg.autoRaise == true);          // D-17
    REQUIRE(cfg.raiseOnFocus == true);       // D-17
    REQUIRE(cfg.clickToFocus == false);

    // Restore
    if (origDirsStr.empty()) unsetenv("XDG_CONFIG_DIRS");
    else setenv("XDG_CONFIG_DIRS", origDirsStr.c_str(), 1);
    if (origHomeStr.empty()) unsetenv("XDG_CONFIG_HOME");
    else setenv("XDG_CONFIG_HOME", origHomeStr.c_str(), 1);
}

// =============================================================================
// Test 12: applyKeyValue handles all 18 settings (string, int, bool)
// =============================================================================
TEST_CASE("applyKeyValue handles all 18 settings", "[config]") {
    Config cfg;

    // String settings (9 colors + 1 command = 10 string settings, but menu-highlight is 1 of them)
    cfg.applyKeyValue("tab-foreground", "red");
    REQUIRE(cfg.tabForeground == "red");

    cfg.applyKeyValue("tab-background", "blue");
    REQUIRE(cfg.tabBackground == "blue");

    cfg.applyKeyValue("frame-background", "green");
    REQUIRE(cfg.frameBackground == "green");

    cfg.applyKeyValue("button-background", "yellow");
    REQUIRE(cfg.buttonBackground == "yellow");

    cfg.applyKeyValue("borders", "purple");
    REQUIRE(cfg.borders == "purple");

    cfg.applyKeyValue("menu-foreground", "cyan");
    REQUIRE(cfg.menuForeground == "cyan");

    cfg.applyKeyValue("menu-background", "magenta");
    REQUIRE(cfg.menuBackground == "magenta");

    cfg.applyKeyValue("menu-highlight", "orange");
    REQUIRE(cfg.menuHighlight == "orange");

    cfg.applyKeyValue("menu-borders", "pink");
    REQUIRE(cfg.menuBorders == "pink");

    cfg.applyKeyValue("new-window-command", "alacritty");
    REQUIRE(cfg.newWindowCommand == "alacritty");

    // Bool settings (4)
    cfg.applyKeyValue("click-to-focus", "true");
    REQUIRE(cfg.clickToFocus == true);

    cfg.applyKeyValue("raise-on-focus", "true");
    REQUIRE(cfg.raiseOnFocus == true);

    cfg.applyKeyValue("auto-raise", "true");
    REQUIRE(cfg.autoRaise == true);

    cfg.applyKeyValue("exec-using-shell", "true");
    REQUIRE(cfg.execUsingShell == true);

    // Int settings (4)
    cfg.applyKeyValue("auto-raise-delay", "500");
    REQUIRE(cfg.autoRaiseDelay == 500);

    cfg.applyKeyValue("pointer-stopped-delay", "100");
    REQUIRE(cfg.pointerStoppedDelay == 100);

    cfg.applyKeyValue("destroy-window-delay", "2000");
    REQUIRE(cfg.destroyWindowDelay == 2000);

    cfg.applyKeyValue("frame-thickness", "12");
    REQUIRE(cfg.frameThickness == 12);

    // Unknown key should not crash
    cfg.applyKeyValue("unknown-key", "value");
    // No assertion needed -- just checking it doesn't crash
}

// =============================================================================
// Test 13: Boolean config values accept "true"/"false" and "1"/"0"
// =============================================================================
TEST_CASE("Boolean config values accept multiple formats", "[config]") {
    Config cfg;

    // "true" / "false"
    cfg.applyKeyValue("click-to-focus", "true");
    REQUIRE(cfg.clickToFocus == true);

    cfg.applyKeyValue("click-to-focus", "false");
    REQUIRE(cfg.clickToFocus == false);

    // "1" / "0"
    cfg.applyKeyValue("raise-on-focus", "1");
    REQUIRE(cfg.raiseOnFocus == true);

    cfg.applyKeyValue("raise-on-focus", "0");
    REQUIRE(cfg.raiseOnFocus == false);

    // Case insensitive
    cfg.applyKeyValue("auto-raise", "True");
    REQUIRE(cfg.autoRaise == true);

    cfg.applyKeyValue("auto-raise", "FALSE");
    REQUIRE(cfg.autoRaise == false);
}

// =============================================================================
// Test 14: Integer values out of range are clamped
// =============================================================================
TEST_CASE("Integer values are clamped to valid ranges", "[config]") {
    Config cfg;

    // Delays clamped to 1-60000
    cfg.applyKeyValue("auto-raise-delay", "0");
    REQUIRE(cfg.autoRaiseDelay == 1);

    cfg.applyKeyValue("auto-raise-delay", "70000");
    REQUIRE(cfg.autoRaiseDelay == 60000);

    cfg.applyKeyValue("pointer-stopped-delay", "-5");
    REQUIRE(cfg.pointerStoppedDelay == 1);

    cfg.applyKeyValue("pointer-stopped-delay", "99999");
    REQUIRE(cfg.pointerStoppedDelay == 60000);

    cfg.applyKeyValue("destroy-window-delay", "-100");
    REQUIRE(cfg.destroyWindowDelay == 1);

    cfg.applyKeyValue("destroy-window-delay", "100000");
    REQUIRE(cfg.destroyWindowDelay == 60000);

    // Frame thickness clamped to 1-50
    cfg.applyKeyValue("frame-thickness", "0");
    REQUIRE(cfg.frameThickness == 1);

    cfg.applyKeyValue("frame-thickness", "100");
    REQUIRE(cfg.frameThickness == 50);

    cfg.applyKeyValue("frame-thickness", "-10");
    REQUIRE(cfg.frameThickness == 1);
}

// =============================================================================
// Test 15: Lines > 4096 chars are skipped with warning
// =============================================================================
TEST_CASE("Lines exceeding 4096 chars are skipped", "[config]") {
    std::string longLine(4100, 'x');
    longLine += " = value";

    std::string path = writeTempConfig(longLine + "\ntab-foreground = green\n");

    Config cfg;
    cfg.applyFile(path);

    // Long line should be skipped, valid line should parse
    REQUIRE(cfg.tabForeground == "green");

    removeTempFile(path);
}

// =============================================================================
// Test 16: Values > 256 chars are rejected with warning
// =============================================================================
TEST_CASE("Values exceeding 256 chars are rejected", "[config]") {
    std::string longValue(300, 'a');

    std::string path = writeTempConfig(
        "tab-foreground = " + longValue + "\n"
        "tab-background = blue\n"
    );

    Config cfg;
    cfg.applyFile(path);

    // Long value should be rejected (default kept)
    REQUIRE(cfg.tabForeground == "#000000");
    // Normal value should parse
    REQUIRE(cfg.tabBackground == "blue");

    removeTempFile(path);
}

// =============================================================================
// Test 17: xdgConfigHome ignores relative XDG_CONFIG_HOME
// =============================================================================
TEST_CASE("xdgConfigHome ignores relative XDG_CONFIG_HOME", "[config][xdg]") {
    char* origXdg = std::getenv("XDG_CONFIG_HOME");
    char* origHome = std::getenv("HOME");

    std::string origXdgStr, origHomeStr;
    if (origXdg) origXdgStr = origXdg;
    if (origHome) origHomeStr = origHome;

    // Set relative path (should be ignored)
    setenv("XDG_CONFIG_HOME", "relative/config", 1);
    setenv("HOME", "/home/testuser2", 1);

    std::string result = xdgConfigHome();
    REQUIRE(result == "/home/testuser2/.config");

    // Restore
    if (origXdgStr.empty()) unsetenv("XDG_CONFIG_HOME");
    else setenv("XDG_CONFIG_HOME", origXdgStr.c_str(), 1);
    if (!origHomeStr.empty()) setenv("HOME", origHomeStr.c_str(), 1);
}

// =============================================================================
// Test 18: xdgConfigDirs skips relative paths
// =============================================================================
TEST_CASE("xdgConfigDirs skips relative paths", "[config][xdg]") {
    char* orig = std::getenv("XDG_CONFIG_DIRS");
    std::string origStr;
    if (orig) origStr = orig;

    // Mix absolute and relative paths
    setenv("XDG_CONFIG_DIRS", "relative/path:/etc/xdg:another/relative", 1);

    auto dirs = xdgConfigDirs();
    REQUIRE(dirs.size() == 1);
    REQUIRE(dirs[0] == "/etc/xdg");

    // Restore
    if (origStr.empty()) unsetenv("XDG_CONFIG_DIRS");
    else setenv("XDG_CONFIG_DIRS", origStr.c_str(), 1);
}

// =============================================================================
// Test 19: applyKeyValue handles non-numeric int values gracefully
// =============================================================================
TEST_CASE("applyKeyValue handles non-numeric int values gracefully", "[config]") {
    Config cfg;

    // Should not crash, should warn and keep default
    cfg.applyKeyValue("frame-thickness", "not-a-number");
    REQUIRE(cfg.frameThickness == 7);

    cfg.applyKeyValue("auto-raise-delay", "abc");
    REQUIRE(cfg.autoRaiseDelay == 400);
}

// =============================================================================
// CLI Tests (CONF-04): Command-line option parsing via applyCliArgs()
// =============================================================================

// Test 20: CLI --tab-foreground=red sets tabForeground to "red"
TEST_CASE("CLI --tab-foreground sets string value", "[config][cli]") {
    Config cfg;
    char* argv[] = { const_cast<char*>("wm2"), const_cast<char*>("--tab-foreground=red"), nullptr };
    cfg.applyCliArgs(2, argv);
    REQUIRE(cfg.tabForeground == "red");
}

// Test 21: CLI --frame-thickness=3 sets frameThickness to 3
TEST_CASE("CLI --frame-thickness sets integer value", "[config][cli]") {
    Config cfg;
    char* argv[] = { const_cast<char*>("wm2"), const_cast<char*>("--frame-thickness=3"), nullptr };
    cfg.applyCliArgs(2, argv);
    REQUIRE(cfg.frameThickness == 3);
}

// Test 22: CLI --auto-raise sets autoRaise to true
TEST_CASE("CLI --auto-raise enables boolean", "[config][cli]") {
    Config cfg;
    // Forced off first: the built-in default is now TRUE (D-17), so without
    // this the case would pass whether or not the flag did anything.
    cfg.autoRaise = false;
    REQUIRE(cfg.autoRaise == false);
    char* argv[] = { const_cast<char*>("wm2"), const_cast<char*>("--auto-raise"), nullptr };
    cfg.applyCliArgs(2, argv);
    REQUIRE(cfg.autoRaise == true);
}

// Test 23: CLI --no-auto-raise sets autoRaise to false (even if config set it true)
TEST_CASE("CLI --no-auto-raise disables boolean", "[config][cli]") {
    Config cfg;
    cfg.autoRaise = true;  // Simulate config file setting it
    char* argv[] = { const_cast<char*>("wm2"), const_cast<char*>("--no-auto-raise"), nullptr };
    cfg.applyCliArgs(2, argv);
    REQUIRE(cfg.autoRaise == false);
}

// Test 24: CLI --click-to-focus sets clickToFocus to true
TEST_CASE("CLI --click-to-focus enables boolean", "[config][cli]") {
    Config cfg;
    char* argv[] = { const_cast<char*>("wm2"), const_cast<char*>("--click-to-focus"), nullptr };
    cfg.applyCliArgs(2, argv);
    REQUIRE(cfg.clickToFocus == true);
}

// Test 25: CLI --no-click-to-focus sets clickToFocus to false
TEST_CASE("CLI --no-click-to-focus disables boolean", "[config][cli]") {
    Config cfg;
    cfg.clickToFocus = true;
    char* argv[] = { const_cast<char*>("wm2"), const_cast<char*>("--no-click-to-focus"), nullptr };
    cfg.applyCliArgs(2, argv);
    REQUIRE(cfg.clickToFocus == false);
}

// Test 25a: CLI --raise-on-focus sets raiseOnFocus to true
//
// FOCUS-02: raise-on-focus was the one focus boolean with no CLI coverage in
// either direction. Both flags existed and dispatched correctly, but nothing
// asserted it -- so the pair could have been broken (or removed) without a
// single test noticing, on the boolean whose default this plan changes.
TEST_CASE("CLI --raise-on-focus enables boolean", "[config][cli]") {
    Config cfg;
    // Forced off first: the built-in default is now TRUE (D-17).
    cfg.raiseOnFocus = false;
    REQUIRE(cfg.raiseOnFocus == false);
    char* argv[] = { const_cast<char*>("wm2"), const_cast<char*>("--raise-on-focus"), nullptr };
    cfg.applyCliArgs(2, argv);
    REQUIRE(cfg.raiseOnFocus == true);
}

// Test 25b: CLI --no-raise-on-focus sets raiseOnFocus to false
TEST_CASE("CLI --no-raise-on-focus disables boolean", "[config][cli]") {
    Config cfg;
    cfg.raiseOnFocus = true;
    char* argv[] = { const_cast<char*>("wm2"), const_cast<char*>("--no-raise-on-focus"), nullptr };
    cfg.applyCliArgs(2, argv);
    REQUIRE(cfg.raiseOnFocus == false);
}

// Test 25c: the full precedence chain, all three focus booleans, both directions
//
// Default -> config file -> CLI, asserted end to end in one case per direction,
// because that ordering is what the three gates added in this plan actually
// consume. The pieces were covered separately; the chain was not.
//
// Direction 1: a CLI negation beats a config-file enable.
TEST_CASE("A CLI negation overrides a config-file enable for every focus boolean",
          "[config][cli]") {
    std::string path = writeTempConfig(
        "click-to-focus = true\n"
        "raise-on-focus = true\n"
        "auto-raise = true\n"
    );

    Config cfg;
    cfg.applyFile(path);
    REQUIRE(cfg.clickToFocus == true);
    REQUIRE(cfg.raiseOnFocus == true);
    REQUIRE(cfg.autoRaise == true);

    char* argv[] = {
        const_cast<char*>("wm2"),
        const_cast<char*>("--no-click-to-focus"),
        const_cast<char*>("--no-raise-on-focus"),
        const_cast<char*>("--no-auto-raise"),
        nullptr
    };
    cfg.applyCliArgs(4, argv);

    REQUIRE(cfg.clickToFocus == false);
    REQUIRE(cfg.raiseOnFocus == false);
    REQUIRE(cfg.autoRaise == false);

    removeTempFile(path);
}

// Direction 2: a CLI enable beats a config-file disable.
TEST_CASE("A CLI enable overrides a config-file disable for every focus boolean",
          "[config][cli]") {
    std::string path = writeTempConfig(
        "click-to-focus = false\n"
        "raise-on-focus = false\n"
        "auto-raise = false\n"
    );

    Config cfg;
    cfg.applyFile(path);
    // Non-vacuous for all three: two of these disagree with the built-in
    // default (D-17), and the third agrees with it but is flipped below.
    REQUIRE(cfg.clickToFocus == false);
    REQUIRE(cfg.raiseOnFocus == false);
    REQUIRE(cfg.autoRaise == false);

    char* argv[] = {
        const_cast<char*>("wm2"),
        const_cast<char*>("--click-to-focus"),
        const_cast<char*>("--raise-on-focus"),
        const_cast<char*>("--auto-raise"),
        nullptr
    };
    cfg.applyCliArgs(4, argv);

    REQUIRE(cfg.clickToFocus == true);
    REQUIRE(cfg.raiseOnFocus == true);
    REQUIRE(cfg.autoRaise == true);

    removeTempFile(path);
}

// ---------------------------------------------------------------------------
// FOCUS-01 (plan 08-08): the focus-stealing-prevention off switch.
//
// All four precedence steps are asserted -- built-in default, config-file key,
// CLI enable, CLI negation -- because this switch disables a security
// mitigation. A user who believes they have turned it off and has not, or who
// believes it is on and it is not, is worse off than one with no switch at all.
// ---------------------------------------------------------------------------

// Step 2: the config-file key, in both directions.
TEST_CASE("Config key focus-stealing-prevention parses in both directions",
          "[config]") {
    Config cfg;
    REQUIRE(cfg.focusStealingPrevention == true);   // step 1: the default

    cfg.applyKeyValue("focus-stealing-prevention", "false");
    REQUIRE(cfg.focusStealingPrevention == false);

    cfg.applyKeyValue("focus-stealing-prevention", "true");
    REQUIRE(cfg.focusStealingPrevention == true);

    // The same 0/1 spelling the other booleans accept.
    cfg.applyKeyValue("focus-stealing-prevention", "0");
    REQUIRE(cfg.focusStealingPrevention == false);

    cfg.applyKeyValue("focus-stealing-prevention", "1");
    REQUIRE(cfg.focusStealingPrevention == true);
}

// Step 3/4: the CLI pair. The enable case forces the value OFF first, so it
// cannot pass vacuously against a default that is already true.
TEST_CASE("CLI --no-focus-stealing-prevention disables the mitigation",
          "[config][cli]") {
    Config cfg;
    REQUIRE(cfg.focusStealingPrevention == true);
    char* argv[] = { const_cast<char*>("wm2"),
                     const_cast<char*>("--no-focus-stealing-prevention"), nullptr };
    cfg.applyCliArgs(2, argv);
    REQUIRE(cfg.focusStealingPrevention == false);
}

TEST_CASE("CLI --focus-stealing-prevention enables the mitigation",
          "[config][cli]") {
    Config cfg;
    cfg.focusStealingPrevention = false;
    REQUIRE(cfg.focusStealingPrevention == false);
    char* argv[] = { const_cast<char*>("wm2"),
                     const_cast<char*>("--focus-stealing-prevention"), nullptr };
    cfg.applyCliArgs(2, argv);
    REQUIRE(cfg.focusStealingPrevention == true);
}

// The whole chain, in the direction that actually matters for a mitigation:
// a config file that turned it off, overridden back on from the command line.
TEST_CASE("A CLI enable overrides a config-file disable of focus-stealing prevention",
          "[config][cli]") {
    std::string path = writeTempConfig("focus-stealing-prevention = false\n");

    Config cfg;
    cfg.applyFile(path);
    REQUIRE(cfg.focusStealingPrevention == false);   // disagrees with the default

    char* argv[] = { const_cast<char*>("wm2"),
                     const_cast<char*>("--focus-stealing-prevention"), nullptr };
    cfg.applyCliArgs(2, argv);
    REQUIRE(cfg.focusStealingPrevention == true);

    removeTempFile(path);
}

// Test 26: Multiple CLI options on same command line all applied
TEST_CASE("CLI multiple options all applied", "[config][cli]") {
    Config cfg;
    char* argv[] = {
        const_cast<char*>("wm2"),
        const_cast<char*>("--tab-foreground=white"),
        const_cast<char*>("--frame-thickness=12"),
        const_cast<char*>("--auto-raise"),
        const_cast<char*>("--new-window-command=alacritty"),
        nullptr
    };
    cfg.applyCliArgs(5, argv);
    REQUIRE(cfg.tabForeground == "white");
    REQUIRE(cfg.frameThickness == 12);
    REQUIRE(cfg.autoRaise == true);
    REQUIRE(cfg.newWindowCommand == "alacritty");
}

// Test 27: Precedence: CLI overrides config file values
TEST_CASE("CLI overrides config file values", "[config][cli]") {
    Config cfg;
    // Simulate config file setting thickness=10
    cfg.frameThickness = 10;
    cfg.tabForeground = "blue";
    cfg.autoRaise = true;

    // CLI sets different values
    char* argv[] = {
        const_cast<char*>("wm2"),
        const_cast<char*>("--frame-thickness=3"),
        const_cast<char*>("--tab-foreground=red"),
        const_cast<char*>("--no-auto-raise"),
        nullptr
    };
    cfg.applyCliArgs(4, argv);

    REQUIRE(cfg.frameThickness == 3);       // CLI wins
    REQUIRE(cfg.tabForeground == "red");    // CLI wins
    REQUIRE(cfg.autoRaise == false);        // CLI wins
}

// Test 28: Unknown option prints error to stderr and exits with code 2
TEST_CASE("CLI unknown option causes exit", "[config][cli]") {
    // Fork a child process to test exit behavior
    int pipefd[2];
    REQUIRE(pipe(pipefd) == 0);

    pid_t pid = fork();
    REQUIRE(pid >= 0);

    if (pid == 0) {
        // Child process
        close(pipefd[0]);
        // Redirect stderr to pipe
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        // Ensure stderr is unbuffered so output appears immediately
        setvbuf(stderr, nullptr, _IONBF, 0);

        Config cfg;
        char* argv[] = { const_cast<char*>("wm2"), const_cast<char*>("--unknown-option"), nullptr };
        cfg.applyCliArgs(2, argv);
        _exit(0);  // Should not reach here
    }

    // Parent process
    close(pipefd[1]);

    // Wait for child to exit first, then read stderr
    int status;
    waitpid(pid, &status, 0);

    // Now read stderr from child (child has exited, pipe has all data)
    char buf[1024] = {};
    ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
    close(pipefd[0]);

    // Verify child exited with code 2
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 2);

    // Verify error message was printed
    std::string output(buf, n > 0 ? n : 0);
    REQUIRE(output.find("Try") != std::string::npos);
    REQUIRE(output.find("--help") != std::string::npos);
}

// Test 29: -- terminates option parsing (per GNU convention)
TEST_CASE("CLI -- terminates option parsing", "[config][cli]") {
    Config cfg;
    char* argv[] = {
        const_cast<char*>("wm2"),
        const_cast<char*>("--frame-thickness=5"),
        const_cast<char*>("--"),
        const_cast<char*>("--tab-foreground=blue"),
        nullptr
    };
    cfg.applyCliArgs(4, argv);

    REQUIRE(cfg.frameThickness == 5);
    // --tab-foreground=blue should NOT be parsed (after --)
    REQUIRE(cfg.tabForeground == "#000000");  // default remains
}

// Test 30: CLI --new-window-command="alacritty" sets newWindowCommand
TEST_CASE("CLI --new-window-command sets string value", "[config][cli]") {
    Config cfg;
    char* argv[] = { const_cast<char*>("wm2"), const_cast<char*>("--new-window-command=alacritty"), nullptr };
    cfg.applyCliArgs(2, argv);
    REQUIRE(cfg.newWindowCommand == "alacritty");
}

// Test 31: CLI --auto-raise-delay=200 sets autoRaiseDelay to 200
TEST_CASE("CLI --auto-raise-delay sets integer value", "[config][cli]") {
    Config cfg;
    char* argv[] = { const_cast<char*>("wm2"), const_cast<char*>("--auto-raise-delay=200"), nullptr };
    cfg.applyCliArgs(2, argv);
    REQUIRE(cfg.autoRaiseDelay == 200);
}

// Test 32: CLI --exec-using-shell sets execUsingShell to true
TEST_CASE("CLI --exec-using-shell enables boolean", "[config][cli]") {
    Config cfg;
    char* argv[] = { const_cast<char*>("wm2"), const_cast<char*>("--exec-using-shell"), nullptr };
    cfg.applyCliArgs(2, argv);
    REQUIRE(cfg.execUsingShell == true);
}

// Test 33: CLI --no-exec-using-shell sets execUsingShell to false
TEST_CASE("CLI --no-exec-using-shell disables boolean", "[config][cli]") {
    Config cfg;
    cfg.execUsingShell = true;
    char* argv[] = { const_cast<char*>("wm2"), const_cast<char*>("--no-exec-using-shell"), nullptr };
    cfg.applyCliArgs(2, argv);
    REQUIRE(cfg.execUsingShell == false);
}

// Test 34: Integer CLI values are clamped same as config file values
TEST_CASE("CLI integer values are clamped to valid ranges", "[config][cli]") {
    Config cfg;

    // Thickness clamped to 1-50
    char* argv1[] = { const_cast<char*>("wm2"), const_cast<char*>("--frame-thickness=100"), nullptr };
    cfg.applyCliArgs(2, argv1);
    REQUIRE(cfg.frameThickness == 50);

    // Reset
    Config cfg2;
    char* argv2[] = { const_cast<char*>("wm2"), const_cast<char*>("--frame-thickness=-5"), nullptr };
    cfg2.applyCliArgs(2, argv2);
    REQUIRE(cfg2.frameThickness == 1);

    // Delay clamped to 1-60000
    Config cfg3;
    char* argv3[] = { const_cast<char*>("wm2"), const_cast<char*>("--auto-raise-delay=70000"), nullptr };
    cfg3.applyCliArgs(2, argv3);
    REQUIRE(cfg3.autoRaiseDelay == 60000);

    Config cfg4;
    char* argv4[] = { const_cast<char*>("wm2"), const_cast<char*>("--auto-raise-delay=0"), nullptr };
    cfg4.applyCliArgs(2, argv4);
    REQUIRE(cfg4.autoRaiseDelay == 1);
}

// Test 35: No CLI args leaves config unchanged
TEST_CASE("CLI no args leaves config unchanged", "[config][cli]") {
    Config cfg;
    char* argv[] = { const_cast<char*>("wm2"), nullptr };
    cfg.applyCliArgs(1, argv);

    REQUIRE(cfg.tabForeground == "#000000");
    REQUIRE(cfg.frameThickness == 7);
    REQUIRE(cfg.autoRaise == true);          // D-17: unchanged means the default
    REQUIRE(cfg.newWindowCommand == "xterm");
}

// =============================================================================
// Manual menu entry tests (APPS-04): menu-entry-name=/menu-entry-command=/
// menu-entry-category= accumulator parsing
// =============================================================================

// Test 36: Full triple (name, command, category) produces one AppEntry
TEST_CASE("menu-entry-name/command/category produce one manual AppEntry", "[config]") {
    Config cfg;
    cfg.applyKeyValue("menu-entry-name", "Firefox");
    cfg.applyKeyValue("menu-entry-command", "firefox --private-window");
    cfg.applyKeyValue("menu-entry-category", "Internet");

    REQUIRE(cfg.manualMenuEntries.size() == 1);
    REQUIRE(cfg.manualMenuEntries[0].name == "Firefox");
    REQUIRE(cfg.manualMenuEntries[0].execArgv == (std::vector<std::string>{"firefox", "--private-window"}));
    REQUIRE(cfg.manualMenuEntries[0].category == "Internet");
    REQUIRE(cfg.manualMenuEntries[0].source == AppEntry::Source::Manual);
}

// Test 37: menu-entry-name alone defaults category to "Custom" (D-07)
TEST_CASE("menu-entry-name alone defaults category to Custom", "[config]") {
    Config cfg;
    cfg.applyKeyValue("menu-entry-name", "App");

    REQUIRE(cfg.manualMenuEntries.size() == 1);
    REQUIRE(cfg.manualMenuEntries[0].name == "App");
    REQUIRE(cfg.manualMenuEntries[0].category == "Custom");
}

// Test 38: menu-entry-command with no preceding menu-entry-name does not
// crash and leaves manualMenuEntries empty
TEST_CASE("menu-entry-command with no preceding menu-entry-name is a no-op", "[config]") {
    Config cfg;
    cfg.applyKeyValue("menu-entry-command", "x");

    REQUIRE(cfg.manualMenuEntries.empty());
}

// Test 39: multiple menu-entry-name= blocks each accumulate independently
TEST_CASE("multiple menu-entry-name blocks accumulate independently", "[config]") {
    Config cfg;
    cfg.applyKeyValue("menu-entry-name", "First");
    cfg.applyKeyValue("menu-entry-command", "first-cmd");
    cfg.applyKeyValue("menu-entry-name", "Second");
    cfg.applyKeyValue("menu-entry-command", "second-cmd");
    cfg.applyKeyValue("menu-entry-category", "Graphics");

    REQUIRE(cfg.manualMenuEntries.size() == 2);
    REQUIRE(cfg.manualMenuEntries[0].name == "First");
    REQUIRE(cfg.manualMenuEntries[0].execArgv == std::vector<std::string>{"first-cmd"});
    REQUIRE(cfg.manualMenuEntries[0].category == "Custom");
    REQUIRE(cfg.manualMenuEntries[1].name == "Second");
    REQUIRE(cfg.manualMenuEntries[1].execArgv == std::vector<std::string>{"second-cmd"});
    REQUIRE(cfg.manualMenuEntries[1].category == "Graphics");
}

// Test 40: menu-entry-category with no preceding menu-entry-name is a no-op
TEST_CASE("menu-entry-category with no preceding menu-entry-name is a no-op", "[config]") {
    Config cfg;
    cfg.applyKeyValue("menu-entry-category", "Graphics");

    REQUIRE(cfg.manualMenuEntries.empty());
}

// Test 41: a ';' in any of the three accumulator values is refused, because a
// stored ';' makes `get menu-entries` unreadable by `set menu-entries`
// (CodeRabbit F1, 2026-09-07)
TEST_CASE("a menu-entry value containing a semicolon is refused by the file parser",
          "[config]") {
    // WHY THE FILE PARSER AND NOT THE WIRE PARSER. configMenuEntriesValue()
    // renders the manual entries as ';'-separated records and
    // parseMenuEntriesValue() splits on ';' to read them back, so the WIRE
    // parser cannot produce an entry whose name, command or category contains
    // a ';'. The config FILE accumulator could -- and the value rendered from
    // one could not be read back by the parser that is supposed to read it, so
    // a `set` of exactly what `get` had just printed was refused.
    //
    // All THREE keys are exercised in one file, because the renderer emits all
    // three and a fix that guarded only the name would leave the same defect
    // reachable through the other two.
    std::string path = writeTempConfig(
        "menu-entry-name = a;b\n"
        "menu-entry-name = Mail\n"
        "menu-entry-command = mutt;rm -rf\n"
        "menu-entry-category = Inter;net\n"
    );

    Config cfg;
    cfg.applyFile(path);
    removeTempFile(path);

    const std::string rendered = configMenuEntriesValue(cfg);
    std::vector<AppEntry> readBack;
    std::string reason;
    const bool reRead = parseMenuEntriesValue(rendered, readBack, reason);

    INFO("rendered: " << rendered);
    INFO("re-read refusal: " << reason);
    INFO("stored entries: " << cfg.manualMenuEntries.size());

    // THE DEFECT. What `get` prints must be a value `set` accepts, and it must
    // mean the same list.
    CHECK(reRead);
    CHECK(readBack.size() == cfg.manualMenuEntries.size());

    // The refused NAME opened no entry at all, so the well-formed one is the
    // only entry stored.
    REQUIRE(cfg.manualMenuEntries.size() == 1);
    CHECK(cfg.manualMenuEntries[0].name == "Mail");

    // ...and the refused COMMAND and CATEGORY were skipped rather than written
    // into the entry that was open, which is what keeps the ';' out of the
    // rendered value by every route into it.
    CHECK(cfg.manualMenuEntries[0].execArgv.empty());
    CHECK(cfg.manualMenuEntries[0].category == "Custom");

    // The round trip agrees field by field, not merely in length.
    if (reRead && readBack.size() == cfg.manualMenuEntries.size()) {
        for (std::size_t i = 0; i < readBack.size(); ++i) {
            CHECK(readBack[i].name     == cfg.manualMenuEntries[i].name);
            CHECK(readBack[i].category == cfg.manualMenuEntries[i].category);
            CHECK(readBack[i].execArgv == cfg.manualMenuEntries[i].execArgv);
        }
    }
}

// Test 42: an entry the file parser DOES accept still round-trips, so Test 41
// cannot pass by refusing everything
TEST_CASE("menu entries with no semicolon round-trip through the rendered value",
          "[config]") {
    std::string path = writeTempConfig(
        "menu-entry-name = Mail\n"
        "menu-entry-command = mutt -f inbox\n"
        "menu-entry-category = Internet\n"
        "menu-entry-name = Editor\n"
        "menu-entry-command = vi\n"
    );

    Config cfg;
    cfg.applyFile(path);
    removeTempFile(path);

    REQUIRE(cfg.manualMenuEntries.size() == 2);

    const std::string rendered = configMenuEntriesValue(cfg);
    std::vector<AppEntry> readBack;
    std::string reason;
    INFO("rendered: " << rendered);
    INFO("re-read refusal: " << reason);
    REQUIRE(parseMenuEntriesValue(rendered, readBack, reason));

    REQUIRE(readBack.size() == 2);
    CHECK(readBack[0].name     == "Mail");
    CHECK(readBack[0].execArgv == (std::vector<std::string>{"mutt", "-f", "inbox"}));
    CHECK(readBack[0].category == "Internet");
    CHECK(readBack[1].name     == "Editor");
    CHECK(readBack[1].execArgv == std::vector<std::string>{"vi"});
    CHECK(readBack[1].category == "Custom");
}

// =============================================================================
// Font settings (plan 09-01): tab-font
//
// CONF-02's outstanding "fonts" box. Before this plan `grep -ci font
// src/Config.cpp` was 0: the tab font was a string literal inside
// Border::loadTabFont() and the menu font a string literal inside
// WindowManager::initialiseScreen(). Neither could be changed without editing
// the source and recompiling.
//
// The default is asserted as a LITERAL rather than against a named constant,
// deliberately: a comparison against whatever the header happens to hold would
// keep passing after a silent drift, and the shipped font is a visual claim
// that should have to be changed on purpose.
//
// WHAT THE LITERAL NOW IS, and what it is not. Plan 09-01 set these defaults
// to the strings the binary had hardcoded, character for character, so that an
// existing user saw no change at all. Quick task 261004-vp6 ended that: the
// defaults are now sized in PIXELS rather than points, because a point size
// follows the X server's reported DPI and a VNC server's DPI is not
// predictable. The DPI claim itself is measured in tests/test_xft_poc.cpp; the
// two cases below only pin which string ships.
//
// D-8.5-01: the spelling `tab-font` is permanent. Chosen over `font-tab`
// because every existing key in this file is <subject>-<attribute>
// (tab-foreground, menu-highlight, frame-background).
// =============================================================================

TEST_CASE("tab-font defaults to the shipped pixel-sized pattern", "[config]") {
    Config cfg;
    REQUIRE(cfg.tabFont == "DejaVu Sans:bold:pixelsize=13");
}

TEST_CASE("applyKeyValue sets tab-font verbatim", "[config]") {
    Config cfg;
    cfg.applyKeyValue("tab-font", "Monospace:size=20");
    REQUIRE(cfg.tabFont == "Monospace:size=20");
}

// A fontconfig pattern is spaces, commas and colons by nature (DISC-05a: the
// value grammar is a pattern handed to XftFontOpenName unchanged). If applyFile
// mangled any of those the setting would be silently unusable for every
// multi-word family name, which is most of them.
TEST_CASE("A tab-font value with spaces, commas and colons survives applyFile intact",
          "[config]") {
    const std::string pattern = "Noto Sans Mono,DejaVu Sans Mono:bold:size=14";
    std::string path = writeTempConfig("tab-font = " + pattern + "\n");

    Config cfg;
    cfg.applyFile(path);

    REQUIRE(cfg.tabFont == pattern);

    removeTempFile(path);
}

TEST_CASE("CLI --tab-font sets string value", "[config][cli]") {
    Config cfg;
    char arg0[] = "wm2";
    char arg1[] = "--tab-font=Monospace:size=20";
    char* argv[] = {arg0, arg1, nullptr};

    cfg.applyCliArgs(2, argv);
    REQUIRE(cfg.tabFont == "Monospace:size=20");
}

// =============================================================================
// Font settings (plan 09-01): menu-font
//
// The sibling of tab-font above, and the same reasoning applies to all of it:
// the default is asserted as a literal so a silent drift fails here; the
// spelling `menu-font` is permanent under D-8.5-01; the value is a fontconfig
// pattern taken verbatim; and as of quick task 261004-vp6 it is sized in
// pixels for the reason written out above.
//
// Note the ONE difference from tab-font: this default carries no `:bold`. The
// menu was never drawn bold.
// =============================================================================

TEST_CASE("menu-font defaults to the shipped pixel-sized pattern", "[config]") {
    Config cfg;
    REQUIRE(cfg.menuFont == "DejaVu Sans:pixelsize=13");
}

TEST_CASE("applyKeyValue sets menu-font verbatim", "[config]") {
    Config cfg;
    cfg.applyKeyValue("menu-font", "Monospace:size=20");
    REQUIRE(cfg.menuFont == "Monospace:size=20");
}

TEST_CASE("A menu-font value with spaces, commas and colons survives applyFile intact",
          "[config]") {
    const std::string pattern = "Noto Sans Mono,DejaVu Sans Mono:size=14";
    std::string path = writeTempConfig("menu-font = " + pattern + "\n");

    Config cfg;
    cfg.applyFile(path);

    REQUIRE(cfg.menuFont == pattern);

    removeTempFile(path);
}

TEST_CASE("CLI --menu-font sets string value", "[config][cli]") {
    Config cfg;
    char arg0[] = "wm2";
    char arg1[] = "--menu-font=Monospace:size=20";
    char* argv[] = {arg0, arg1, nullptr};

    cfg.applyCliArgs(2, argv);
    REQUIRE(cfg.menuFont == "Monospace:size=20");
}

// The two font keys are independent of one another. Setting one must not move
// the other -- a single shared member behind two key names would pass every
// case above and fail this one.
TEST_CASE("tab-font and menu-font are independent settings", "[config]") {
    Config cfg;
    const std::string tabDefault = cfg.tabFont;
    const std::string menuDefault = cfg.menuFont;

    REQUIRE(tabDefault != menuDefault);

    cfg.applyKeyValue("tab-font", "Monospace:size=20");
    REQUIRE(cfg.tabFont == "Monospace:size=20");
    REQUIRE(cfg.menuFont == menuDefault);

    cfg.applyKeyValue("menu-font", "Serif:size=9");
    REQUIRE(cfg.menuFont == "Serif:size=9");
    REQUIRE(cfg.tabFont == "Monospace:size=20");
}


// =============================================================================
// C2 (Codex pass 4): the accumulated menu-entry list has to fit in one reply
//
// The WHOLE manual entry list travels as ONE value under `menu-entries`, and
// the protocol refuses a line over kConfigProtocolMaxLine before it scans it.
// A config file may hold any number of individually valid menu-entry groups,
// so a file with enough of them loaded perfectly and then made
// `get menu-entries` produce a reply BOTH clients reject as TooLong --
// wm2-config disconnecting during its opening read of a file the window
// manager was entirely happy with.
// =============================================================================

TEST_CASE("a menu-entry list too long for one reply is trimmed as the file is read, with a warning",
          "[config]") {
    // Two hundred entries is far more than one 4096-byte line can carry, so
    // the guard has to bite; the exact number that survives is not asserted as
    // a constant but derived from the bound itself below.
    std::string contents;
    for (int i = 0; i < 200; ++i) {
        const std::string n = std::to_string(i);
        contents += "menu-entry-name=Entry" + n + "\n";
        contents += "menu-entry-command=/usr/bin/app" + n + "\n";
        contents += "menu-entry-category=Custom\n";
    }
    const std::string path = writeTempConfig(contents);

    Config cfg;
    std::string warnings;
    {
        StderrCapture capture;
        cfg.applyFile(path);
        warnings = capture.text();
    }
    removeTempFile(path);

    const std::size_t kept = cfg.manualMenuEntries.size();
    INFO("kept " << kept << " of 200; reply line "
         << menuEntriesReplyLine(cfg).size() << " bytes, bound "
         << kConfigProtocolMaxLine);
    INFO("warnings:\n" << warnings);

    // THE POINT: what the window manager holds is always what the wire can
    // carry.
    CHECK(menuEntriesReplyLine(cfg).size() <= kConfigProtocolMaxLine);

    // Trimmed, not emptied, and trimmed from the END so the file's own order
    // decides which entries survive.
    CHECK(kept > 0);
    CHECK(kept < 200);
    REQUIRE(kept > 0);
    CHECK(cfg.manualMenuEntries.front().name == "Entry0");
    CHECK(cfg.manualMenuEntries.back().name ==
          "Entry" + std::to_string(kept - 1));

    // The user is TOLD, and told how many and why.
    CHECK(warnings.find("wm2: warning:") != std::string::npos);
    CHECK(warnings.find(std::to_string(200 - kept)) != std::string::npos);
    CHECK(warnings.find("menu") != std::string::npos);
}

TEST_CASE("a menu-entry list that fits in one reply is loaded whole and silently",
          "[config]") {
    // The other side of the bound, so the case above cannot pass by refusing
    // everything: the largest list the guard keeps is loaded entry for entry
    // with nothing said about it.
    Config probe;
    {
        std::string contents;
        for (int i = 0; i < 200; ++i) {
            const std::string n = std::to_string(i);
            contents += "menu-entry-name=Entry" + n + "\n";
            contents += "menu-entry-command=/usr/bin/app" + n + "\n";
            contents += "menu-entry-category=Custom\n";
        }
        const std::string path = writeTempConfig(contents);
        StderrCapture quiet;
        probe.applyFile(path);
        removeTempFile(path);
    }
    const std::size_t fits = probe.manualMenuEntries.size();
    REQUIRE(fits > 1);

    std::string contents;
    for (std::size_t i = 0; i < fits; ++i) {
        const std::string n = std::to_string(i);
        contents += "menu-entry-name=Entry" + n + "\n";
        contents += "menu-entry-command=/usr/bin/app" + n + "\n";
        contents += "menu-entry-category=Custom\n";
    }
    const std::string path = writeTempConfig(contents);

    Config cfg;
    std::string warnings;
    {
        StderrCapture capture;
        cfg.applyFile(path);
        warnings = capture.text();
    }
    removeTempFile(path);

    INFO("reply line " << menuEntriesReplyLine(cfg).size() << " bytes, bound "
         << kConfigProtocolMaxLine);
    INFO("warnings:\n" << warnings);

    CHECK(cfg.manualMenuEntries.size() == fits);
    CHECK(menuEntriesReplyLine(cfg).size() <= kConfigProtocolMaxLine);
    CHECK(warnings.empty());
}

// Test 43: the wire parser enforces the same PER-VALUE rules the config file
// does -- the 256-byte bound and the ban on line breaks (X2, Codex pass 5)
TEST_CASE("a menu-entry value the configuration file could not hold is refused on the wire",
          "[config]") {
    // X2. parseMenuEntriesValue() checked each `val` for leading and trailing
    // space -- because applyFile() trims and the wire does not -- and stopped
    // there. Two rules of exactly the same class were missing:
    //
    //   * Config::applyFile() SKIPS a value longer than
    //     kConfigFileMaxValueBytes with a warning to a stderr nobody reads, and
    //     configFileWrite() refuses to write one;
    //   * the file is one value per line, so it can hold no line break at all,
    //     and configFileWrite() refuses that too. The wire's JSON escaping is
    //     what lets a '\n' arrive here in the first place.
    //
    // So `set menu-entries` was ACKNOWLEDGED for a list the file can neither
    // preserve nor reproduce: applied live, and then either lost at the next
    // reload or refused at Save with a message about a rule the client was
    // never told.
    //
    // The reasons are in the shape src/Manager.cpp's socket String branch uses
    // for the same two rules on a plain string key: name the bound, name the
    // newline.

    SECTION("a name over the file's per-value bound is refused, and the reason names it") {
        const std::string tooLong(300, 'x');
        REQUIRE(tooLong.size() > kConfigFileMaxValueBytes);

        std::vector<AppEntry> out;
        std::string reason;
        const bool accepted = parseMenuEntriesValue(
            "menu-entry-name=" + tooLong + ";menu-entry-command=mutt", out, reason);

        INFO("refusal: " << reason);
        CHECK_FALSE(accepted);
        CHECK(reason.find(std::to_string(kConfigFileMaxValueBytes)) != std::string::npos);
    }

    SECTION("a name with a line break in it is refused, and the reason names it") {
        std::vector<AppEntry> out;
        std::string reason;
        const bool accepted = parseMenuEntriesValue(
            "menu-entry-name=Ma\nil;menu-entry-command=mutt", out, reason);

        INFO("refusal: " << reason);
        CHECK_FALSE(accepted);
        CHECK(reason.find("newline") != std::string::npos);
    }

    SECTION("a COMMAND with a line break in it is refused too") {
        // The command is exempt from the trim rule, because applyKeyValue()
        // tokenises it on whitespace and its outer spaces survive nothing on
        // either route. A LINE BREAK is a different thing: on the wire it is
        // one more token separator, and in the file it ends the line -- so the
        // entry the file reads back is not the entry that was acknowledged.
        std::vector<AppEntry> out;
        std::string reason;
        const bool accepted = parseMenuEntriesValue(
            "menu-entry-name=Mail;menu-entry-command=mutt\n-f inbox", out, reason);

        INFO("refusal: " << reason);
        CHECK_FALSE(accepted);
        CHECK(reason.find("newline") != std::string::npos);
    }

    SECTION("a carriage return is refused on the same grounds") {
        std::vector<AppEntry> out;
        std::string reason;
        const bool accepted = parseMenuEntriesValue(
            "menu-entry-name=Mail;menu-entry-category=Inter\rnet", out, reason);

        INFO("refusal: " << reason);
        CHECK_FALSE(accepted);
        CHECK(reason.find("newline") != std::string::npos);
    }

    SECTION("an ordinary value is still accepted, so the guards refuse something rather than everything") {
        // Including a name of EXACTLY the bound, so the comparison is proved to
        // be "longer than" rather than "as long as".
        const std::string atTheBound(kConfigFileMaxValueBytes, 'x');

        std::vector<AppEntry> out;
        std::string reason;
        const bool accepted = parseMenuEntriesValue(
            "menu-entry-name=" + atTheBound +
            ";menu-entry-command=mutt -f inbox;menu-entry-category=Internet",
            out, reason);

        INFO("refusal: " << reason);
        REQUIRE(accepted);
        REQUIRE(out.size() == 1);
        CHECK(out[0].name == atTheBound);
        CHECK(out[0].execArgv == (std::vector<std::string>{"mutt", "-f", "inbox"}));
        CHECK(out[0].category == "Internet");
    }
}
