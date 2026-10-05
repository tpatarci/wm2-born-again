cmake_minimum_required(VERSION 3.20)
# D-33: 3.20 is the real floor. The tags-as-labels discovery option is
# implemented in Catch2's CatchAddTests.cmake with string(JSON ...), which needs
# CMake >= 3.19, and every ctest gate in Phase 8 carries --no-tests=error, which
# needs ctest >= 3.20. Declaring an older floor would make a 3.16-3.18 builder
# fail at configure time with an opaque string(JSON) error instead of a named
# cause -- exactly the failure mode scripts/preflight.sh exists to eliminate.
project(wm2-born-again VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- X11 dependencies via pkg-config (BLD-01, BLD-05) ---
find_package(PkgConfig REQUIRED)
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
pkg_check_modules(XEXT REQUIRED IMPORTED_TARGET xext)
pkg_check_modules(XFT REQUIRED IMPORTED_TARGET xft)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
# D-26 (XDIS-01/XDIS-02): libxrandr is a HARD BUILD dependency, deliberately not
# an optional one. It is part of the base X client stack on every target this
# project supports, so conditional compilation would buy nothing and would fork
# the geometry path in two. What is NOT hard is the RUNTIME availability of the
# extension: XRRQueryExtension is checked at startup and a server without RANDR
# gets a warning plus the root-ConfigureNotify fallback, never a fatal error.
# Build-time required, run-time optional -- the two are independent.
pkg_check_modules(XRANDR REQUIRED IMPORTED_TARGET xrandr)

# XDIS-04: libxrender follows exactly the same build-time-required /
# run-time-optional split as libxrandr above. It is pulled in transitively by
# libXft today, but the capability probe calls a public XRender entry point
# directly, so the dependency is declared here rather than being relied on by
# accident. A server without the extension is a warning and a degraded font
# ladder, never a build failure and never a fatal error.
pkg_check_modules(XRENDER REQUIRED IMPORTED_TARGET xrender)

# --- WM Binary (BLD-04) ---
add_executable(${PROJECT_NAME}
    src/main.cpp
    src/Manager.cpp
    src/Client.cpp
    src/Border.cpp
    src/Buttons.cpp
    src/Events.cpp
    src/Config.cpp
    src/Rules.cpp
    src/SocketServer.cpp
    src/AppCache.cpp
    src/DesktopEntry.cpp
    src/BinaryScanner.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${XFT_INCLUDE_DIRS}
    ${FONTCONFIG_INCLUDE_DIRS}
)

# The version the window manager reports over the configuration socket (D-14).
# Taken from the project version above so the two cannot drift; src/Manager.cpp
# carries a fallback only so the file still compiles outside this build.
target_compile_definitions(${PROJECT_NAME} PRIVATE
    WM2_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    PkgConfig::X11
    PkgConfig::XEXT
    PkgConfig::XFT
    PkgConfig::FONTCONFIG
    PkgConfig::XRANDR
    PkgConfig::XRENDER
    m
)

# --- wm2-ctl: the display-free command-line client (CGUI-02, D-17) ---
#
# A TOP-LEVEL target, deliberately outside if(BUILD_TESTS): D-17 ships this in
# the window-manager package so a droplet reached over SSH -- no desktop, no
# toolkit -- can still drive the running window manager. A binary that only
# existed in test builds would not be that.
#
# NO target_link_libraries AT ALL. That empty line is the requirement, not an
# omission: the sources this tool needs are compiled straight into it (the
# test_config pattern), and every one of them is display-free by construction --
# include/ConfigProtocol.h, include/SocketServer.h and include/Config.h name no
# X11, Xft, GTK or GLib header between them. A [wm_config_live] acceptance check
# runs ldd over the result and fails if any of those four appear, so the day
# somebody adds an include that drags in a display library, a test says so.
#
# src/Config.cpp is here for the settable-key list --help prints, which is
# generated from configKeySpecs() -- the same option table the window manager's
# own --help comes from, so the two tools cannot advertise different keys.
add_executable(wm2-ctl
    apps/wm2-ctl/main.cpp
    src/Config.cpp
    src/SocketServer.cpp
)

target_include_directories(wm2-ctl PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# --- wm2-config: the optional GTK3 settings window (CGUI-01, CGUI-05, D-18) ---
#
# THREE STATES, not a boolean, and the third one is the default:
#
#   AUTO  probe for gtk+-3.0 and build the GUI if it is there. One status line
#         either way, so a user who expected a settings window and did not get
#         one is told why by the configure they already ran.
#   ON    probe with REQUIRED. Somebody who asked for the GUI explicitly gets a
#         named configure failure rather than a silent absence -- AUTO's
#         forgiveness is exactly wrong for an explicit request.
#   OFF   DO NOT PROBE AT ALL. A probe that runs when the answer is already
#         known is how a dependency leaks into a build that asked not to have
#         one: it populates GTK3_* variables that a later edit can reference
#         from outside the gate without anything failing to configure.
#
# CGUI-05 is the requirement all of this serves: the window manager builds,
# installs and runs exactly as today when GTK is absent. Nothing outside the
# resolved-ON branch may reference PkgConfig::GTK3, and the window manager's own
# link libraries are not touched by any of it -- asserted by a [wm2_config_smoke]
# gate that runs ldd over the built binary.
set(BUILD_CONFIG_GUI "AUTO" CACHE STRING
    "Build the GTK3 configuration GUI (wm2-config): AUTO, ON or OFF")
set_property(CACHE BUILD_CONFIG_GUI PROPERTY STRINGS AUTO ON OFF)

set(CONFIG_GUI_ENABLED OFF)
if(BUILD_CONFIG_GUI STREQUAL "AUTO")
    pkg_check_modules(GTK3 IMPORTED_TARGET gtk+-3.0)
    if(GTK3_FOUND)
        set(CONFIG_GUI_ENABLED ON)
        message(STATUS "wm2-config: gtk+-3.0 ${GTK3_VERSION} found -- building the configuration GUI")
    else()
        message(STATUS "wm2-config: pkg-config cannot find gtk+-3.0 -- the configuration GUI will NOT be built (install libgtk-3-dev to get it; the window manager and wm2-ctl are unaffected)")
    endif()
elseif(BUILD_CONFIG_GUI STREQUAL "ON")
    pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
    set(CONFIG_GUI_ENABLED ON)
    message(STATUS "wm2-config: gtk+-3.0 ${GTK3_VERSION} -- building the configuration GUI")
elseif(BUILD_CONFIG_GUI STREQUAL "OFF")
    message(STATUS "wm2-config: BUILD_CONFIG_GUI=OFF -- the configuration GUI will NOT be built (the window manager and wm2-ctl are unaffected)")
else()
    message(FATAL_ERROR "BUILD_CONFIG_GUI must be AUTO, ON or OFF; got '${BUILD_CONFIG_GUI}'")
endif()

# Recorded in the cache so a tool outside CMake can read the RESOLUTION rather
# than re-deriving it from BUILD_CONFIG_GUI plus a pkg-config probe of its own
# (scripts/gates/install-components.sh, IN-06). INTERNAL and written on every
# configure, so it always describes THIS configure rather than a previous one.
set(CONFIG_GUI_RESOLVED ${CONFIG_GUI_ENABLED} CACHE INTERNAL
    "Whether this tree builds wm2-config, as resolved from BUILD_CONFIG_GUI")

if(CONFIG_GUI_ENABLED)
    # The same "compile the source straight into the consumer" pattern wm2-ctl
    # and every test_config* target use; the project ships no library target.
    # src/Config.cpp is the layered read and the key table, ConfigFileWriter.cpp
    # the surgical save, SocketServer.cpp the socket path -- all three already
    # display-free, none of them dragging X11 in behind them.
    add_executable(wm2-config
        apps/wm2-config/main.cpp
        apps/wm2-config/AppearancePage.cpp
        apps/wm2-config/BehaviourPage.cpp
        apps/wm2-config/MenuPage.cpp
        apps/wm2-config/FormState.cpp
        apps/wm2-config/ProtocolClient.cpp
        src/Config.cpp
        src/ConfigFileWriter.cpp
        src/SocketServer.cpp
    )
    target_include_directories(wm2-config PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )
    target_compile_definitions(wm2-config PRIVATE
        WM2_VERSION="${PROJECT_VERSION}"
    )
    # The ONLY reference to the GTK imported target in this file.
    target_link_libraries(wm2-config PRIVATE PkgConfig::GTK3)
endif()

# --- Install components (D-19) ---
#
# The first install() rules this project has ever had, and they are split in two
# from the outset because the COMPONENT, not the binary, is this project's
# install identity:
#
#   wm          the window manager, wm2-ctl (D-17 ships the command-line client
#               in the window-manager package, so a droplet reached over SSH can
#               still drive a running WM), the session entry a display manager
#               reads, and the documentation.
#   config-gui  wm2-config and its application entry, and nothing else.
#
# A packager can therefore produce wm2-born-again and wm2-born-again-config from
# ONE build tree, and the first of the two declares no GTK dependency. That last
# clause is the whole point of the split and is not left as prose: every
# executable the `wm` component installs is run through ldd by
# scripts/gates/install-components.sh, which fails if libgtk, libglib or
# libgobject is named.
#
# The config-gui rules are inside if(CONFIG_GUI_ENABLED) so a GUI-disabled tree
# has nothing to install for that component -- `cmake --install --component
# config-gui` there stages zero files and exits 0, which is what lets the same
# gate run in both trees.
include(GNUInstallDirs)

# The window manager itself.
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT wm
)

# wm2-ctl is a SEPARATE rule from the window manager, not a second name on the
# same line, because it is in this package for a different reason: D-17 puts the
# display-free client in the window-manager package so a droplet with no desktop
# and no toolkit can still be driven over SSH. Whoever later wonders whether it
# belongs here should find the answer beside the rule.
install(TARGETS wm2-ctl
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT wm
)

# The session entry a display manager reads to offer this window manager as a
# session. share/xsessions is deliberately NOT share/applications: it is not on
# the path this project's own scanner walks, so the session entry can never
# appear in the root menu as though it were an application to launch.
install(FILES ${CMAKE_SOURCE_DIR}/packaging/wm2-born-again.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/xsessions
    COMPONENT wm
)

# The documentation the package ships.
install(FILES
        ${CMAKE_SOURCE_DIR}/docs/RELEASE-NOTES.md
        ${CMAKE_SOURCE_DIR}/LICENSE
        # Shipped beside LICENSE rather than inside it: the attribution and the
        # upstream terms used to be appended to the MIT text, which made GitHub
        # report the licence as "Other" instead of MIT. LICENSE is now the
        # canonical MIT text and nothing else, so it is recognised, and the
        # derivation it must not lose is stated here.
        ${CMAKE_SOURCE_DIR}/NOTICE
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
    COMPONENT wm
)

if(CONFIG_GUI_ENABLED)
    install(TARGETS wm2-config
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT config-gui
    )

    install(FILES ${CMAKE_SOURCE_DIR}/packaging/wm2-config.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
        COMPONENT config-gui
    )
endif()

# --- Tests (TEST-01, TEST-02, TEST-04) ---
option(BUILD_TESTS "Build test suite" ON)

if(BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.14.0
    )
    FetchContent_MakeAvailable(Catch2)

    enable_testing()
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)

    # XTEST input synthesis (D-08, TEST-05). Test-only on purpose: the shipped
    # wm2-born-again target must NOT gain libXtst -- the 512MB VPS constraint
    # argues against dragging test-only libraries into the binary. Declared
    # inside if(BUILD_TESTS) and linked into test_wm_process only.
    pkg_check_modules(XTST REQUIRED IMPORTED_TARGET xtst)

    # --- Environment preflight fixture (D-01, D-02, TEST-07) ---
    #
    # Setup-only ctest fixture: fails with a named cause before any behavioral
    # assertion runs. Deliberately NOT a CMake configure-time check -- that would
    # block plain binary builds on machines that never run tests.
    #
    # The explicit LABELS keeps the phase on ONE selection mechanism. This is a
    # hand-written add_test, not a Catch2 case, so the tags-as-labels option does
    # not reach it; without the manual label the only way to select it would be
    # by name, reintroducing the exact name-vs-label split D-33 exists to remove.
    # Labelling a fixture test does not change fixture semantics: ctest still
    # pulls this setup test in automatically for any -L-selected test that
    # declares FIXTURES_REQUIRED preflight_ok, whether or not this test itself
    # matched the filter.
    add_test(NAME preflight COMMAND ${CMAKE_SOURCE_DIR}/scripts/preflight.sh --quiet)
    set_tests_properties(preflight PROPERTIES
        FIXTURES_SETUP preflight_ok
        LABELS "preflight"
    )

    # RAII unit tests (X11 linked for inline deleter calls, but no display needed)
    add_executable(test_raii tests/test_raii.cpp)
    target_link_libraries(test_raii PRIVATE
        Catch2::Catch2WithMain
        PkgConfig::X11
    )
    target_include_directories(test_raii PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${XFT_INCLUDE_DIRS}
    )
    # D-33: TAG SELECTION CONVENTION -- applies to every test-discovery call in
    # this file, no exceptions.
    #
    # Catch2 registers each ctest test under its TEST_CASE *name*, so a Catch2
    # tag is invisible to `ctest -R` (which matches names). A tag-shaped -R
    # expression matches zero tests, and ctest exits 0 on zero matches -- making
    # every such gate green-on-nothing. The tags-as-labels option (Catch2 v3.14
    # extras/Catch.cmake:152) registers the tags as ctest LABELS, fixing this at
    # the registration site rather than at each call site.
    #
    # Consequently, every gate in Phase 8 selects by ANCHORED label and always
    # passes --no-tests=error:
    #     ctest -L '^wm_process$' --no-tests=error
    #     ctest -L '^(wm_lifecycle|wm_sizehints|wm_gravity)$' --no-tests=error
    # Anchoring is load-bearing: -L is an unanchored regex, so a bare
    # `-L config` would also drag in `wm_config_runtime`. --no-tests=error is the
    # backstop that turns a mistyped or renamed tag into exit code 8 instead of a
    # silent pass.
    #
    # The option is orthogonal to PROPERTIES: Catch2 emits the labels through a
    # separate set_tests_properties call, so nothing already attached is
    # clobbered. Do NOT add the test-renaming prefix/suffix options -- they
    # rename tests out from under this convention and cannot express multi-tag
    # targets.
    #
    # NOTE: the acceptance gates for this convention are line-counting greps
    # (label-option lines must equal discovery-call lines; the shared-display
    # bindings must number exactly four). Keep those literal tokens out of
    # comment prose, or the comments defeat the guards they describe.
    catch_discover_tests(test_raii ADD_TAGS_AS_LABELS)

    # Smoke tests (need Xvfb display)
    find_program(XVFB_EXECUTABLE Xvfb)
    if(XVFB_EXECUTABLE)
        add_executable(test_smoke tests/test_smoke.cpp)
        target_link_libraries(test_smoke PRIVATE
            Catch2::Catch2WithMain
            PkgConfig::X11
        )
        target_include_directories(test_smoke PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${XFT_INCLUDE_DIRS}
        )

        # Xvfb fixture: start before tests, stop after.
        #
        # The shared-display group below additionally takes a named resource
        # lock (RESEARCH Pitfall 3). A running WM owns SubstructureRedirectMask
        # exclusively and reparents every window created on its display, which
        # would corrupt the raw-Xlib geometry and property assertions in
        # test_client / test_ewmh. test_wm_process allocates its own display and
        # never touches this one, but the lock makes it structurally impossible
        # for `ctest -j` to interleave these four targets with each other or
        # with any future WM-bearing target that mistakenly lands here.
        set(XVFB_PID_FILE "${CMAKE_BINARY_DIR}/xvfb.pid")
        set(XVFB_LOG_FILE "${CMAKE_BINARY_DIR}/xvfb99.log")

        # Three fixes over the original one-liner. The first two are required
        # for `ctest -j`; the third is required for the suite to be testing what
        # it thinks it is testing.
        #   1. The backgrounded server's stdout/stderr go to a FILE. Previously
        #      it inherited ctest's output pipe and never closed it, so ctest
        #      blocked forever after the tests finished -- the whole suite hung.
        #   2. Readiness is a bounded poll on the server actually answering,
        #      not a flat `sleep 1` that can expire early under parallel load.
        #   3. THE SUITE NEVER ADOPTS A SERVER IT DID NOT START. "Something
        #      answers on :99" used to be the whole readiness test, so when an
        #      earlier run was killed before its cleanup step, its orphaned
        #      Xvfb kept :99, this command's own Xvfb exited at once with
        #      "Server is already active for display 99", and every later run
        #      silently ran on the orphan -- for fourteen hours on one host,
        #      with the orphan partly swapped out, until an X round trip that
        #      takes no time on a fresh server took more than the two seconds
        #      one case allows. The lock file the server writes carries its
        #      pid; the fixture succeeds only when that pid is the one it
        #      spawned and still alive, and otherwise fails with the server's
        #      own log so the orphan is found rather than used.
        #
        # The two paths are passed to bash as positional parameters and quoted
        # inside the script, so a build directory with a space in its name works.
        #
        # -nolisten tcp and no -ac: a test server takes local connections only,
        # with the server's default access list, the same as the per-fixture
        # servers the window-manager tests allocate for themselves.
        add_test(NAME start_xvfb
            COMMAND bash -c "rm -f -- \"$1\"; Xvfb :99 -screen 0 1024x768x24 -nolisten tcp +render -noreset >\"$2\" 2>&1 & pid=$!; echo \"$pid\" > \"$1\"; n=0; while [ $n -lt 100 ]; do if ! kill -0 $pid 2>/dev/null; then echo 'wm2: the Xvfb this suite started on :99 exited; :99 belongs to another server and the suite will not adopt it. Its log:' >&2; cat \"$2\" >&2; exit 1; fi; if DISPLAY=:99 xdpyinfo >/dev/null 2>&1; then if [ \"$(tr -d ' ' < /tmp/.X99-lock 2>/dev/null)\" = \"$pid\" ]; then exit 0; fi; echo 'wm2: :99 answered but its lock file names a different server; the suite will not adopt it' >&2; exit 1; fi; n=$((n+1)); sleep 0.1; done; echo 'wm2: Xvfb :99 did not become ready' >&2; cat \"$2\" >&2; exit 1" bash "${XVFB_PID_FILE}" "${XVFB_LOG_FILE}"
        )
        set_tests_properties(start_xvfb PROPERTIES FIXTURES_SETUP xvfb_display)

        add_test(NAME stop_xvfb
            COMMAND bash -c "kill \"$(cat -- \"$1\")\" 2>/dev/null || true" bash "${XVFB_PID_FILE}"
        )
        set_tests_properties(stop_xvfb PROPERTIES FIXTURES_CLEANUP xvfb_display)

        # Discover smoke tests with fixture requirement set per-test
        catch_discover_tests(test_smoke ADD_TAGS_AS_LABELS
            PROPERTIES
                ENVIRONMENT "DISPLAY=:99"
                FIXTURES_REQUIRED "xvfb_display;preflight_ok"
                RESOURCE_LOCK "x_display_99"
        )
    else()
        message(WARNING "Xvfb not found -- smoke tests disabled")
    endif()

    # Event loop unit tests (need X11 headers and a display -- TEST-08)
    add_executable(test_eventloop tests/test_eventloop.cpp)
    target_link_libraries(test_eventloop PRIVATE
        Catch2::Catch2WithMain
        PkgConfig::X11
    )
    target_include_directories(test_eventloop PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${XFT_INCLUDE_DIRS}
    )
    catch_discover_tests(test_eventloop ADD_TAGS_AS_LABELS
        PROPERTIES
            ENVIRONMENT "DISPLAY=:99"
            FIXTURES_REQUIRED "xvfb_display;preflight_ok"
            RESOURCE_LOCK "x_display_99"
    )

    # Auto-raise timer unit tests (pure chrono arithmetic, no display needed)
    add_executable(test_autoraise tests/test_autoraise.cpp)
    target_link_libraries(test_autoraise PRIVATE
        Catch2::Catch2WithMain
    )
    target_include_directories(test_autoraise PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${XFT_INCLUDE_DIRS}
    )
    catch_discover_tests(test_autoraise ADD_TAGS_AS_LABELS)

    # Menu paint-decision unit tests (08.5-13, TEST-08).
    #
    # Registered exactly like test_autoraise above and for the same reason: NO
    # ENVIRONMENT, NO FIXTURES_REQUIRED and NO RESOURCE_LOCK, because it needs
    # no display. Its inputs are synthetic histograms and integers, so it cannot
    # flake -- and that inability is the whole point of splitting the menu
    # open-paint-sample path into units. Binding a fixture or a display here
    # would silently destroy it.
    #
    # PkgConfig::X11 is linked for the Xlib TYPE declarations include/MenuPaint.h
    # needs (Window, None); no Xlib call is made from this binary.
    add_executable(test_menupaint tests/test_menupaint.cpp)
    target_link_libraries(test_menupaint PRIVATE
        Catch2::Catch2WithMain
        PkgConfig::X11
    )
    target_include_directories(test_menupaint PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/tests
        ${XFT_INCLUDE_DIRS}
    )
    catch_discover_tests(test_menupaint ADD_TAGS_AS_LABELS)

    # Client lifecycle tests (need Xvfb display, TEST-03)
    if(XVFB_EXECUTABLE)
        add_executable(test_client tests/test_client.cpp)
        target_link_libraries(test_client PRIVATE
            Catch2::Catch2WithMain
            PkgConfig::X11
        )
        target_include_directories(test_client PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${XFT_INCLUDE_DIRS}
        )
        catch_discover_tests(test_client ADD_TAGS_AS_LABELS
            PROPERTIES
                ENVIRONMENT "DISPLAY=:99"
                FIXTURES_REQUIRED "xvfb_display;preflight_ok"
                RESOURCE_LOCK "x_display_99"
        )

        # Xft PoC test (need Xvfb display + Xft/fontconfig)
        add_executable(test_xft_poc tests/test_xft_poc.cpp)
        target_link_libraries(test_xft_poc PRIVATE
            Catch2::Catch2WithMain
            PkgConfig::X11
            PkgConfig::XEXT
            PkgConfig::XFT
            PkgConfig::FONTCONFIG
        )
        target_include_directories(test_xft_poc PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${XFT_INCLUDE_DIRS}
        )
        catch_discover_tests(test_xft_poc ADD_TAGS_AS_LABELS
            PROPERTIES
                ENVIRONMENT "DISPLAY=:99"
                FIXTURES_REQUIRED "xvfb_display;preflight_ok"
                RESOURCE_LOCK "x_display_99"
        )

        # EWMH compliance tests (need Xvfb display)
        add_executable(test_ewmh tests/test_ewmh.cpp)
        target_link_libraries(test_ewmh PRIVATE
            Catch2::Catch2WithMain
            PkgConfig::X11
        )
        target_include_directories(test_ewmh PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${XFT_INCLUDE_DIRS}
        )
        catch_discover_tests(test_ewmh ADD_TAGS_AS_LABELS
            PROPERTIES
                ENVIRONMENT "DISPLAY=:99"
                FIXTURES_REQUIRED "xvfb_display;preflight_ok"
                RESOURCE_LOCK "x_display_99"
        )

        # --- Process-level WM tests (D-07, TEST-05) ---
        #
        # Runs the REAL wm2-born-again binary as a child process. Deliberately
        # carries NO shared-display ENVIRONMENT binding and NO
        # `FIXTURES_REQUIRED xvfb_display`: this target allocates, locks and
        # tears down its own display per fixture instance. A running WM owns
        # SubstructureRedirectMask exclusively and would reparent the windows
        # test_client / test_ewmh assert on (RESEARCH Pitfall 3).
        add_executable(test_wm_process tests/test_wm_process.cpp)
        target_link_libraries(test_wm_process PRIVATE
            Catch2::Catch2WithMain
            PkgConfig::X11
            PkgConfig::XTST
        )
        target_include_directories(test_wm_process PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/tests
            ${XFT_INCLUDE_DIRS}
        )

        # Scratch directory for per-fixture display locks, captured child stderr,
        # and ASan log_path output. Under the build tree, never a bare fixed /tmp
        # name (threat T-8-TMP).
        set(WM_PROCESS_WORKDIR "${CMAKE_BINARY_DIR}/wm-process-tests")
        file(MAKE_DIRECTORY "${WM_PROCESS_WORKDIR}")

        target_compile_definitions(test_wm_process PRIVATE
            WM2_BINARY_PATH="$<TARGET_FILE:wm2-born-again>"
            WM2_LSAN_SUPPRESSIONS="${CMAKE_SOURCE_DIR}/tests/lsan.supp"
            WM2_TEST_WORKDIR="${WM_PROCESS_WORKDIR}"
        )

        # The tests exec the binary, so it must exist before they run.
        add_dependencies(test_wm_process wm2-born-again)

        # TIMEOUT is a structural backstop, not the primary mechanism: every
        # case here is already deadline-bounded internally, and the [wm_circulate]
        # case additionally has to prove a WEDGED WM fails rather than hangs.
        # If a future edit ever loses one of those internal deadlines, ctest kills
        # the case instead of stalling the whole suite. Generous against the
        # observed per-case cost (~11s here, more under ASan) so it can never be
        # the thing that reddens a healthy run.
        catch_discover_tests(test_wm_process ADD_TAGS_AS_LABELS
            PROPERTIES
                FIXTURES_REQUIRED preflight_ok
                TIMEOUT 240
        )

        # --- Degraded-capability tests (XDIS-03, TEST-05) ---
        #
        # Same shape as test_wm_process above -- own display per fixture
        # instance, so deliberately NO shared-display binding and NO
        # `FIXTURES_REQUIRED xvfb_display`.
        #
        # Two additions over that target. libXext, because the fallback
        # assertions ask the SERVER whether a frame carries a bounding shape
        # mask (a positive protocol-level check, not "nothing crashed"). And a
        # source-directory definition, because the single-funnel guard reads
        # src/Border.cpp as data and counts call sites in it.
        #
        # This target accumulates further tag groups as later plans in the
        # phase land their fallback coverage. That is the point of selecting by
        # per-test label rather than by target name (D-33): one file can carry
        # several unrelated capability groups without any of the gates
        # over-selecting.
        add_executable(test_wm_fallbacks tests/test_wm_fallbacks.cpp)
        target_link_libraries(test_wm_fallbacks PRIVATE
            Catch2::Catch2WithMain
            PkgConfig::X11
            PkgConfig::XEXT
            # XDIS-04: the [xft_norender_spike] group opens the same rotated
            # FcMatrix pattern the production loader opens and asks the public
            # XRender entry point whether the extension is there. Both are
            # linked explicitly rather than leaning on libXft's transitive
            # dependencies, so the spike cannot start answering a different
            # question if that transitive edge ever changes.
            PkgConfig::XFT
            PkgConfig::FONTCONFIG
            PkgConfig::XRENDER
        )
        target_include_directories(test_wm_fallbacks PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/tests
            ${XFT_INCLUDE_DIRS}
            ${FONTCONFIG_INCLUDE_DIRS}
        )

        target_compile_definitions(test_wm_fallbacks PRIVATE
            WM2_BINARY_PATH="$<TARGET_FILE:wm2-born-again>"
            WM2_LSAN_SUPPRESSIONS="${CMAKE_SOURCE_DIR}/tests/lsan.supp"
            WM2_TEST_WORKDIR="${WM_PROCESS_WORKDIR}"
            WM2_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )

        # The tests exec the binary, so it must exist before they run.
        add_dependencies(test_wm_fallbacks wm2-born-again)

        catch_discover_tests(test_wm_fallbacks ADD_TAGS_AS_LABELS
            PROPERTIES
                FIXTURES_REQUIRED preflight_ok
        )

        # --- Screen-geometry tests (XDIS-01, D-27) ---
        #
        # Same shape as the two targets above -- own display per fixture
        # instance, so deliberately NO shared-display binding and NO
        # `FIXTURES_REQUIRED xvfb_display`. Unlike test_wm_fallbacks this one
        # needs no extra X extension library: every assertion is core-protocol
        # geometry read back from the server.
        #
        # The suite runs its server at 1280x1024 (requested from the test file
        # through the fixture's extra-Xvfb-argument hook, not from here), which
        # is the largest geometry any Phase 8 test needs. Plan 08-05 shrinks
        # from that size to observe a resolution change; this server's RANDR
        # maximum equals the size it was started with, so it can never grow.
        add_executable(test_wm_geometry tests/test_wm_geometry.cpp)
        # XTEST is here for the drag cases only: the tab-reachability clamp can
        # only be proven by a real pointer drag, and the fake events must come
        # from a connection with grab control enabled (RESEARCH Pitfall 10),
        # because Client::move() holds a pointer grab for the whole drag.
        target_link_libraries(test_wm_geometry PRIVATE
            Catch2::Catch2WithMain
            PkgConfig::X11
            PkgConfig::XTST
        )
        target_include_directories(test_wm_geometry PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/tests
            ${XFT_INCLUDE_DIRS}
        )

        target_compile_definitions(test_wm_geometry PRIVATE
            WM2_BINARY_PATH="$<TARGET_FILE:wm2-born-again>"
            WM2_LSAN_SUPPRESSIONS="${CMAKE_SOURCE_DIR}/tests/lsan.supp"
            WM2_TEST_WORKDIR="${WM_PROCESS_WORKDIR}"
        )

        # The tests exec the binary, so it must exist before they run.
        add_dependencies(test_wm_geometry wm2-born-again)

        # ADD_TAGS_AS_LABELS is required by D-33, or the tag this file carries
        # is not selectable by the anchored label gates the phase uses.
        catch_discover_tests(test_wm_geometry ADD_TAGS_AS_LABELS
            PROPERTIES
                FIXTURES_REQUIRED preflight_ok
        )

        # --- Focus-policy tests (FOCUS-02, TEST-05) ---
        #
        # Cloned from test_wm_geometry above -- own display per fixture
        # instance, so deliberately NO shared-display binding and NO
        # `FIXTURES_REQUIRED xvfb_display`.
        #
        # One addition over that target: the XTEST library. Focus is driven by
        # real pointer input here, and it must come from a connection with grab
        # control enabled (RESEARCH Pitfall 10) -- the WM takes pointer grabs
        # during interaction, and Client::eventButton() ignores synthetic
        # events outright, so nothing else reaches the paths under test.
        add_executable(test_wm_focus tests/test_wm_focus.cpp)
        target_link_libraries(test_wm_focus PRIVATE
            Catch2::Catch2WithMain
            PkgConfig::X11
            PkgConfig::XTST
        )
        target_include_directories(test_wm_focus PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/tests
            ${XFT_INCLUDE_DIRS}
        )

        target_compile_definitions(test_wm_focus PRIVATE
            WM2_BINARY_PATH="$<TARGET_FILE:wm2-born-again>"
            WM2_LSAN_SUPPRESSIONS="${CMAKE_SOURCE_DIR}/tests/lsan.supp"
            WM2_TEST_WORKDIR="${WM_PROCESS_WORKDIR}"
        )

        # The tests exec the binary, so it must exist before they run.
        add_dependencies(test_wm_focus wm2-born-again)

        # ADD_TAGS_AS_LABELS is required by D-33, or [wm_focus] is not
        # selectable by the anchored label gates this plan's criteria use.
        catch_discover_tests(test_wm_focus ADD_TAGS_AS_LABELS
            PROPERTIES
                FIXTURES_REQUIRED preflight_ok
                TIMEOUT 240
        )

        # --- Configuration socket, against the real WM (CGUI-02) ---
        #
        # Cloned from test_wm_focus above, XTEST included: the held-grab case
        # opens the root menu with a REAL button press, and the WM takes a
        # pointer grab while the menu is up, so the fake events have to come
        # from a connection with grab control enabled (RESEARCH Pitfall 10).
        #
        # Own display per fixture instance, so deliberately NO shared-display
        # binding and NO `FIXTURES_REQUIRED xvfb_display`. The socket is named
        # after the display, which is what lets these run beside every other
        # fixture-bound target.
        # src/SocketServer.cpp is compiled straight in (the test_config pattern)
        # so a case can assert the PUBLISHED path is the one this build's own
        # DISC-02 resolution produces -- an agreement, not two independent
        # spellings that happen to match today.
        add_executable(test_wm_socket
            tests/test_wm_socket.cpp
            src/SocketServer.cpp
        )
        # PkgConfig::XEXT is for the shape extension, and only for it: the
        # overlapping-sets case counts re-frames as ShapeNotify events, for the
        # reason 09-04 established -- the server suppresses ConfigureNotify for
        # a reconfigure that changes nothing, so that counter cannot see a
        # redundant re-layout and a case resting on it could not fail.
        target_link_libraries(test_wm_socket PRIVATE
            Catch2::Catch2WithMain
            PkgConfig::X11
            PkgConfig::XEXT
            PkgConfig::XTST
        )
        target_include_directories(test_wm_socket PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/tests
            ${XFT_INCLUDE_DIRS}
        )

        target_compile_definitions(test_wm_socket PRIVATE
            WM2_BINARY_PATH="$<TARGET_FILE:wm2-born-again>"
            WM2_LSAN_SUPPRESSIONS="${CMAKE_SOURCE_DIR}/tests/lsan.supp"
            WM2_TEST_WORKDIR="${WM_PROCESS_WORKDIR}"
            # D-14's ceiling is also guarded at the SOURCE level: one case reads
            # the status assembly out of src/Manager.cpp and asserts it does not
            # reach any per-window identity accessor, so an edit that starts
            # publishing titles is caught by a test rather than by a later
            # security review.
            WM2_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )

        # The tests exec the binary, so it must exist before they run.
        add_dependencies(test_wm_socket wm2-born-again)

        # ADD_TAGS_AS_LABELS is required by D-33, or [wm_socket] is not
        # selectable by the anchored label gates this plan's criteria use.
        catch_discover_tests(test_wm_socket ADD_TAGS_AS_LABELS
            PROPERTIES
                FIXTURES_REQUIRED preflight_ok
                TIMEOUT 240
        )

        # --- Live configuration through wm2-ctl (CGUI-02, CGUI-04) ---
        #
        # Cloned from test_wm_socket above, minus XTEST: nothing here is driven
        # by pointer input. The window manager is driven entirely through the
        # REAL wm2-ctl binary as a child process, which is what makes that tool
        # the protocol's reference client (D-17) rather than a second
        # implementation nobody checks.
        #
        # Own display per fixture instance, so deliberately NO shared-display
        # binding and NO `FIXTURES_REQUIRED xvfb_display`.
        #
        # Named test_wm_config_live rather than the research's
        # test_config_live_apply so it SORTS with the other fixture-driven
        # test_wm_* binaries instead of with the display-free test_config* ones
        # -- that prefix is what the existing naming convention distinguishes.
        #
        # src/Config.cpp, src/ConfigFileWriter.cpp and src/SocketServer.cpp are
        # compiled straight in (the test_config pattern) for the display-free
        # cases: one asserts the settable key list and the file writer's managed
        # key list name exactly the same settings, and one asserts the path
        # wm2-ctl resolves is the path this build's own DISC-02 resolution
        # produces.
        add_executable(test_wm_config_live
            tests/test_wm_config_live.cpp
            src/Config.cpp
            src/ConfigFileWriter.cpp
            src/SocketServer.cpp
        )
        # PkgConfig::XEXT is for the shape extension, and only for it: the
        # idempotency case counts ShapeNotify events, because the server
        # suppresses ConfigureNotify for a reconfigure that changes nothing and
        # a redundant re-layout would therefore be invisible.
        #
        # PkgConfig::XTST arrived with plan 09-05, for the menu-colour and
        # menu-entry cases: the root menu opens only on a REAL button press
        # (Client::eventButton refuses send_event), so those cases drive it
        # through the same XTestDriver the runtime suite uses. The
        # wm2-born-again target must still NOT gain libXtst -- see the comment
        # beside the pkg_check_modules call at the top of this file.
        target_link_libraries(test_wm_config_live PRIVATE
            Catch2::Catch2WithMain
            PkgConfig::X11
            PkgConfig::XEXT
            PkgConfig::XTST
        )
        target_include_directories(test_wm_config_live PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/tests
            ${XFT_INCLUDE_DIRS}
        )

        target_compile_definitions(test_wm_config_live PRIVATE
            WM2_BINARY_PATH="$<TARGET_FILE:wm2-born-again>"
            WM2_LSAN_SUPPRESSIONS="${CMAKE_SOURCE_DIR}/tests/lsan.supp"
            WM2_TEST_WORKDIR="${WM_PROCESS_WORKDIR}"
            # The absolute path of the built tool. Handed in rather than
            # searched for on PATH, so a wm2-ctl installed on the developer's
            # machine can never be the one under test.
            WM2_CTL_PATH="$<TARGET_FILE:wm2-ctl>"
        )

        # Both binaries are exec'd, so both must exist before these run.
        add_dependencies(test_wm_config_live wm2-born-again wm2-ctl)

        # ADD_TAGS_AS_LABELS is required by D-33, or [wm_config_live] is not
        # selectable by the anchored label gates this plan's criteria use.
        catch_discover_tests(test_wm_config_live ADD_TAGS_AS_LABELS
            PROPERTIES
                FIXTURES_REQUIRED preflight_ok
                TIMEOUT 300
        )

        # --- The settings window: its model, and that it opens (CGUI-01, D-20) ---
        #
        # REGISTERED UNCONDITIONALLY, not gated on the configuration GUI being
        # built, and that is the whole point. D-20 asks for a GTK smoke test
        # "skipped with a reason when the GUI was not built"; a target that only
        # existed when the GUI did would make the skip unreachable, and on a
        # GTK-less host the anchored `-L '^wm2_config_smoke$'` gate would select
        # zero tests -- which --no-tests=error turns into a failure with no
        # explanation rather than a skip with one.
        #
        # So the binary is always built, its display-free half (the form model,
        # the reset and save semantics, the banner sentence) always runs, and
        # only the cases that need a window to open skip -- with the reason
        # printed. The path of the built GUI is handed in as a compile
        # definition, so nothing ever searches PATH and a wm2-config installed
        # on the developer's machine can never be the one under test; its
        # ABSENCE is what the skip branch keys on.
        #
        # apps/wm2-config/FormState.cpp and ProtocolClient.cpp are compiled
        # straight in (the test_config pattern). Neither includes a GTK, GLib or
        # X11 header -- the GLib main-loop integration is a two-line adapter in
        # apps/wm2-config/main.cpp -- which is what lets this target link
        # Catch2 and X11 alone and still exercise the GUI's own client rather
        # than a hand-rolled stand-in.
        add_executable(test_wm2_config_smoke
            tests/test_wm2_config_smoke.cpp
            apps/wm2-config/FormState.cpp
            apps/wm2-config/ProtocolClient.cpp
            src/Config.cpp
            src/ConfigFileWriter.cpp
            src/SocketServer.cpp
        )
        target_link_libraries(test_wm2_config_smoke PRIVATE
            Catch2::Catch2WithMain
            PkgConfig::X11
            # XTEST, for ONE case: the 09-07 tracer, which proves that
            # click-to-focus committed through the page's own model changes how
            # the running desktop gives focus to real synthesised pointer
            # input. A value echo would pass against a window manager that
            # stored the setting and never consulted it. Test-only, as
            # everywhere else here -- the shipped wm2-born-again target must
            # still NOT gain libXtst.
            PkgConfig::XTST
        )
        target_include_directories(test_wm2_config_smoke PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/tests
            ${XFT_INCLUDE_DIRS}
        )
        target_compile_definitions(test_wm2_config_smoke PRIVATE
            WM2_BINARY_PATH="$<TARGET_FILE:wm2-born-again>"
            WM2_LSAN_SUPPRESSIONS="${CMAKE_SOURCE_DIR}/tests/lsan.supp"
            WM2_TEST_WORKDIR="${WM_PROCESS_WORKDIR}"
            # No WIDGET on the Appearance page can be exercised without a
            # screen, so the guards about its wiring -- that the font value is
            # read through the chooser INTERFACE rather than the getter
            # deprecated since GTK 3.22, that no key D-09 puts on another page
            # has wandered onto this one, and that a refused live `set` puts the
            # form back -- read its source instead. The [wm_socket] suite
            # already guards D-14's ceiling this way.
            WM2_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )
        add_dependencies(test_wm2_config_smoke wm2-born-again)
        if(CONFIG_GUI_ENABLED)
            target_compile_definitions(test_wm2_config_smoke PRIVATE
                WM2_CONFIG_PATH="$<TARGET_FILE:wm2-config>"
            )
            add_dependencies(test_wm2_config_smoke wm2-config)

            # ...but the page's COLOUR VOCABULARY is not a widget and needs no
            # screen (X1). gdk_rgba_parse() is a string parser: it opens no
            # display, and the canonicaliser that decides which spelling ever
            # reaches the form and the user's file is a pure function over it.
            # That decision is the one standing between a CSS colour GDK accepts
            # and a startup the window manager refuses to complete, so it is
            # tested by CALLING it rather than by reading the source around it.
            #
            # THE SECOND REFERENCE TO THE GTK IMPORTED TARGET IN THIS FILE, and
            # the only one outside the wm2-config target itself. It is confined
            # to this branch: with BUILD_CONFIG_GUI=OFF the page is not compiled
            # in, the cases that call into it SKIP with a reason (which is what
            # keeps the nogtk tree's same-suite accounting satisfied), and this
            # test binary links no toolkit at all.
            target_sources(test_wm2_config_smoke PRIVATE
                apps/wm2-config/AppearancePage.cpp
            )
            target_link_libraries(test_wm2_config_smoke PRIVATE PkgConfig::GTK3)
        endif()

        # ADD_TAGS_AS_LABELS is required by D-33, or [wm2_config_smoke] is not
        # selectable by the anchored label gates this plan's criteria use.
        catch_discover_tests(test_wm2_config_smoke ADD_TAGS_AS_LABELS
            PROPERTIES
                FIXTURES_REQUIRED preflight_ok
                TIMEOUT 300
        )

        # --- Window-rule application tests (RULES-02) ---
        #
        # Cloned from test_wm_focus above, minus XTEST: nothing here is driven by
        # pointer input. Rules reach the WM through a real config file in an
        # isolated XDG tree handed to the child, so this target needs no
        # additional link or definition beyond the fixture's three.
        #
        # Own display per fixture instance, so deliberately NO shared-display
        # binding and NO `FIXTURES_REQUIRED xvfb_display`.
        add_executable(test_wm_rules tests/test_wm_rules.cpp)
        target_link_libraries(test_wm_rules PRIVATE
            Catch2::Catch2WithMain
            PkgConfig::X11
        )
        target_include_directories(test_wm_rules PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/tests
            ${XFT_INCLUDE_DIRS}
        )

        target_compile_definitions(test_wm_rules PRIVATE
            WM2_BINARY_PATH="$<TARGET_FILE:wm2-born-again>"
            WM2_LSAN_SUPPRESSIONS="${CMAKE_SOURCE_DIR}/tests/lsan.supp"
            WM2_TEST_WORKDIR="${WM_PROCESS_WORKDIR}"
        )

        # The tests exec the binary, so it must exist before they run.
        add_dependencies(test_wm_rules wm2-born-again)

        # ADD_TAGS_AS_LABELS is required by D-33, or the tag this file carries
        # is not selectable by the anchored label gates this plan's criteria use.
        catch_discover_tests(test_wm_rules ADD_TAGS_AS_LABELS
            PROPERTIES
                FIXTURES_REQUIRED preflight_ok
                TIMEOUT 240
        )

        # --- Lifecycle, size-hint and gravity tests (TEST-05, plan 08-11) ---
        #
        # Cloned from test_wm_rules above, plus XTEST: the destroy-while-focus-
        # tracking case needs a real pointer entry to put focus tracking in
        # flight, and a synthetic event would not reach that path
        # (RESEARCH Pitfall 10, and Client::eventButton ignores send_event).
        # The size-hint group drives real resize drags on the frame's resize
        # handle for the same reason -- an interactive resize is the path the
        # constraint function sits on, and a configure request takes a different
        # route entirely.
        #
        # Own display per fixture instance, so deliberately NO shared-display
        # binding and NO `FIXTURES_REQUIRED xvfb_display`.
        add_executable(test_wm_lifecycle tests/test_wm_lifecycle.cpp)
        target_link_libraries(test_wm_lifecycle PRIVATE
            Catch2::Catch2WithMain
            PkgConfig::X11
            PkgConfig::XTST
        )
        target_include_directories(test_wm_lifecycle PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/tests
            ${XFT_INCLUDE_DIRS}
        )

        target_compile_definitions(test_wm_lifecycle PRIVATE
            WM2_BINARY_PATH="$<TARGET_FILE:wm2-born-again>"
            WM2_LSAN_SUPPRESSIONS="${CMAKE_SOURCE_DIR}/tests/lsan.supp"
            WM2_TEST_WORKDIR="${WM_PROCESS_WORKDIR}"
        )

        # The tests exec the binary, so it must exist before they run.
        add_dependencies(test_wm_lifecycle wm2-born-again)

        # ADD_TAGS_AS_LABELS is required by D-33, or the three tags this file
        # carries are not selectable by the anchored label gates.
        catch_discover_tests(test_wm_lifecycle ADD_TAGS_AS_LABELS
            PROPERTIES
                FIXTURES_REQUIRED preflight_ok
                TIMEOUT 300
        )

        # --- EWMH state, fullscreen/maximize and malformed-property tests
        #     (TEST-05, plan 08-12) ---
        #
        # Cloned from test_wm_lifecycle above. XTEST is linked for the same
        # reason it is there -- the fullscreen/maximize group needs the pointer
        # parked and moved through real device events rather than warps that the
        # WM's crossing handlers would treat differently -- and so that a later
        # case in this file can reach for a real click without a build change.
        #
        # Own display per fixture instance, so deliberately NO shared-display
        # binding and NO `FIXTURES_REQUIRED xvfb_display`.
        add_executable(test_wm_state tests/test_wm_state.cpp)
        target_link_libraries(test_wm_state PRIVATE
            Catch2::Catch2WithMain
            PkgConfig::X11
            PkgConfig::XTST
        )
        target_include_directories(test_wm_state PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/tests
            ${XFT_INCLUDE_DIRS}
        )

        target_compile_definitions(test_wm_state PRIVATE
            WM2_BINARY_PATH="$<TARGET_FILE:wm2-born-again>"
            WM2_LSAN_SUPPRESSIONS="${CMAKE_SOURCE_DIR}/tests/lsan.supp"
            WM2_TEST_WORKDIR="${WM_PROCESS_WORKDIR}"
        )

        # The tests exec the binary, so it must exist before they run.
        add_dependencies(test_wm_state wm2-born-again)

        # ADD_TAGS_AS_LABELS is required by D-33, or the three tags this file
        # carries are not selectable by the anchored label gates.
        catch_discover_tests(test_wm_state ADD_TAGS_AS_LABELS
            PROPERTIES
                FIXTURES_REQUIRED preflight_ok
                TIMEOUT 300
        )

        # --- Config-to-runtime, terminating error paths and stress
        #     (TEST-05, plan 08-13) ---
        #
        # Cloned from test_wm_state above. XTEST is linked because the config
        # group drives the root menu and the tab button with real device events:
        # the menu and the long-press delete both run inside NESTED event loops
        # that hold a pointer grab, and only a connection with grab control
        # enabled can reach them.
        #
        # Own display per fixture instance, so deliberately NO shared-display
        # binding and NO `FIXTURES_REQUIRED xvfb_display`.
        add_executable(test_wm_runtime tests/test_wm_runtime.cpp)
        # XEXT is for reading back the button's BOUNDING shape: the button window
        # is deliberately larger than the square it paints, so the painted size
        # can only be asserted by asking the server for the shape.
        target_link_libraries(test_wm_runtime PRIVATE
            Catch2::Catch2WithMain
            PkgConfig::X11
            PkgConfig::XTST
            PkgConfig::XEXT
        )
        target_include_directories(test_wm_runtime PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/tests
            ${XFT_INCLUDE_DIRS}
        )

        target_compile_definitions(test_wm_runtime PRIVATE
            WM2_BINARY_PATH="$<TARGET_FILE:wm2-born-again>"
            WM2_LSAN_SUPPRESSIONS="${CMAKE_SOURCE_DIR}/tests/lsan.supp"
            WM2_TEST_WORKDIR="${WM_PROCESS_WORKDIR}"
        )

        # The tests exec the binary, so it must exist before they run.
        add_dependencies(test_wm_runtime wm2-born-again)

        # D-11's Configure entry is a PATH probe, so the case that proves the
        # entry appears has to put a REAL wm2-config on the window manager's
        # PATH. Handed in as a path, exactly as test_wm2_config_smoke below
        # takes it, so nothing ever searches the developer's own PATH and a
        # wm2-config installed on this machine can never be the one under test.
        # Its ABSENCE is what the case's skip branch keys on, which is why the
        # definition is conditional rather than the target being conditional.
        if(CONFIG_GUI_ENABLED)
            target_compile_definitions(test_wm_runtime PRIVATE
                WM2_CONFIG_PATH="$<TARGET_FILE:wm2-config>"
            )
            add_dependencies(test_wm_runtime wm2-config)
        endif()

        # ADD_TAGS_AS_LABELS is required by D-33, or the eleven tags this file
        # carries are not selectable by the anchored label gates. Eleven is the
        # figure computed by
        #     grep -oE '\[wm_[a-z_]+\]' tests/test_wm_runtime.cpp | sort -u | wc -l
        # -- recompute it with that command rather than by eye whenever a tag is
        # added or removed. It read "four" until 08.5-06, which was already
        # wrong by six before that plan added [wm_timestamp].
        #
        # A longer TIMEOUT than its siblings: the stress group churns 100+
        # windows against the WM and the config group starts eleven separate
        # fixtures, each of which pays a full Xvfb + WM startup. Under the
        # sanitizer that is roughly twice as slow again.
        catch_discover_tests(test_wm_runtime ADD_TAGS_AS_LABELS
            PROPERTIES
                FIXTURES_REQUIRED preflight_ok
                TIMEOUT 600
        )

        # --- Resource budget: resident memory and idle CPU (D-32, TEST-08,
        #     plan 08-14) ---
        #
        # Cloned from test_wm_runtime above, minus XTEST: this file drives no
        # device events at all. It deliberately issues NO X traffic during its
        # idle windows, because any request would be work the WM is then charged
        # for by the very sample that is supposed to find it idle.
        #
        # Own display per fixture instance, so deliberately NO shared-display
        # binding and NO `FIXTURES_REQUIRED xvfb_display`.
        add_executable(test_wm_resource tests/test_wm_resource.cpp)
        target_link_libraries(test_wm_resource PRIVATE
            Catch2::Catch2WithMain
            PkgConfig::X11
        )
        target_include_directories(test_wm_resource PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/tests
            ${XFT_INCLUDE_DIRS}
        )

        target_compile_definitions(test_wm_resource PRIVATE
            WM2_BINARY_PATH="$<TARGET_FILE:wm2-born-again>"
            WM2_LSAN_SUPPRESSIONS="${CMAKE_SOURCE_DIR}/tests/lsan.supp"
            WM2_TEST_WORKDIR="${WM_PROCESS_WORKDIR}"
        )

        # The tests exec the binary, so it must exist before they run.
        add_dependencies(test_wm_resource wm2-born-again)

        # ADD_TAGS_AS_LABELS is required by D-33, or the [wm_resource_budget]
        # tag is not selectable by the anchored label gate
        # `ctest -L '^wm_resource_budget$'` -- which would then match NOTHING and,
        # with --no-tests=error, fail loudly rather than certify an empty set.
        #
        # The TIMEOUT is generous for a specific, non-negotiable reason: each of
        # the three cases pays a full Xvfb + WM startup, maps twenty clients, and
        # two of them then sit through a THIRTY-SECOND idle window that cannot be
        # shortened without coarsening the one-percent bar. Under the sanitizer,
        # in a loaded full-suite run, that is several times slower again. A
        # timeout here exists to stop a wedged case hanging the suite forever, not
        # to assert that any of this is fast.
        catch_discover_tests(test_wm_resource ADD_TAGS_AS_LABELS
            PROPERTIES
                FIXTURES_REQUIRED preflight_ok
                TIMEOUT 600
        )

        # --- Targeted post-readiness reparent reproducer (TEST-08, plan
        #     08.5-09) ---
        #
        # A DIAGNOSTIC INSTRUMENT that lives in the suite. Its purpose is to make
        # a wedged reparent READABLE -- five diagnostic channels captured while
        # the window manager is still wedged -- rather than to certify anything.
        #
        # ITS CASES ARE BUDGET-DRIVEN AND CHEAP BY DEFAULT: one fixture and one
        # mapped client each, seconds of wall clock. The sampling budget is
        # widened only by an explicit measurement run that sets the knobs in the
        # environment. Do NOT "optimise" the default budget away, and do NOT
        # raise it here -- a budget baked into the build is a budget the ordinary
        # gate suite pays on every run.
        #
        # Cloned from test_wm_state above. Neither XTEST nor XEXT is linked:
        # nothing here drives synthetic input and nothing reads back a shape.
        #
        # Own display per fixture instance, so deliberately NO shared-display
        # binding and NO `FIXTURES_REQUIRED xvfb_display`.
        add_executable(test_wm_repro tests/test_wm_repro.cpp)
        target_link_libraries(test_wm_repro PRIVATE
            Catch2::Catch2WithMain
            PkgConfig::X11
        )
        target_include_directories(test_wm_repro PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/tests
            ${XFT_INCLUDE_DIRS}
        )

        # The fourth definition is the source-tree root: the case locates the
        # capture script under scripts/diag/ through it, so no path to the script
        # is fixed in the test source.
        target_compile_definitions(test_wm_repro PRIVATE
            WM2_BINARY_PATH="$<TARGET_FILE:wm2-born-again>"
            WM2_LSAN_SUPPRESSIONS="${CMAKE_SOURCE_DIR}/tests/lsan.supp"
            WM2_TEST_WORKDIR="${WM_PROCESS_WORKDIR}"
            WM2_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )

        # The tests exec the binary, so it must exist before they run.
        add_dependencies(test_wm_repro wm2-born-again)

        # ADD_TAGS_AS_LABELS is required by D-33, or the single tag this file
        # carries is not selectable by `ctest -L '^wm_repro$' --no-tests=error`,
        # which would then match NOTHING.
        #
        # TIMEOUT is the 300-second class its two nearest siblings use, NOT the
        # 600 that test_wm_runtime and test_wm_resource carry. Those two hold a
        # stress group and a pair of thirty-second idle windows respectively; the
        # default budget here is seconds, and a measurement run deliberately
        # bypasses ctest so this timeout never applies to it.
        catch_discover_tests(test_wm_repro ADD_TAGS_AS_LABELS
            PROPERTIES
                FIXTURES_REQUIRED preflight_ok
                TIMEOUT 300
        )
    endif()

    # Config parsing unit tests (pure data transformation, no X11 needed)
    add_executable(test_config tests/test_config.cpp src/Config.cpp)
    target_link_libraries(test_config PRIVATE
        Catch2::Catch2WithMain
    )
    target_include_directories(test_config PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )
    catch_discover_tests(test_config ADD_TAGS_AS_LABELS)

    # --- Version-1 wire contract codec (CGUI-02, D-20's protocol half) ---
    #
    # Display-free BY CONSTRUCTION, exactly like test_rules below: there is no
    # PkgConfig::X11 here, no Xft include path, no display fixture and no
    # FIXTURES_REQUIRED, because include/ConfigProtocol.h includes nothing but
    # the C++ standard library. This target must be runnable on a host with no
    # X server at all -- if it ever needs one, the header has grown a dependency
    # the window manager, wm2-ctl and wm2-config would all have inherited.
    #
    # The codec is header-only, so there is no production .cpp to compile in.
    add_executable(test_config_protocol tests/test_config_protocol.cpp)
    target_link_libraries(test_config_protocol PRIVATE
        Catch2::Catch2WithMain
    )
    target_include_directories(test_config_protocol PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )
    # ADD_TAGS_AS_LABELS is required, or the [config_protocol] tag is not
    # selectable by the anchored `-L '^config_protocol$'` gate this plan uses.
    catch_discover_tests(test_config_protocol ADD_TAGS_AS_LABELS)

    # --- Configuration socket decision, paths and boundary verdicts (CGUI-02) ---
    #
    # Display-free BY CONSTRUCTION, exactly like the codec target above: no
    # PkgConfig::X11, no Xft include path, no display fixture and no
    # FIXTURES_REQUIRED, because neither include/SocketServer.h nor
    # src/SocketServer.cpp includes an X11, Xft, GTK or GLib header. This target
    # must be runnable on a host with no X server at all -- if it ever needs
    # one, the socket module has grown a dependency the window manager, wm2-ctl
    # and wm2-config would all have inherited.
    #
    # The implementation is compiled straight into the executable (the
    # test_config pattern) rather than linked from a library, because the
    # project ships no library target.
    add_executable(test_config_socket
        tests/test_config_socket.cpp
        src/SocketServer.cpp
    )
    target_link_libraries(test_config_socket PRIVATE
        Catch2::Catch2WithMain
    )
    target_include_directories(test_config_socket PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )
    # ADD_TAGS_AS_LABELS is required, or the [config_socket] tag is not
    # selectable by the anchored `-L '^config_socket$'` gate this plan uses.
    catch_discover_tests(test_config_socket ADD_TAGS_AS_LABELS)

    # --- Surgical config-file writer (CGUI-03, D-02/D-13, D-20's file half) ---
    #
    # Display-free for the same reason as the codec target above: neither
    # include/ConfigFileWriter.h nor src/ConfigFileWriter.cpp touches X11, Xft
    # or GTK, so this target links Catch2 alone and runs with no X server.
    #
    # src/Config.cpp is compiled in because the round-trip cases close the loop
    # through Config::applyFile() and Config::applyKeyValue(): the writer and
    # the parser have to be proven inverses over the managed key set, and that
    # needs both halves in one binary (the test_config "compile the source
    # straight into the consumer" pattern above).
    add_executable(test_config_writer
        tests/test_config_writer.cpp
        src/ConfigFileWriter.cpp
        src/Config.cpp
    )
    target_link_libraries(test_config_writer PRIVATE
        Catch2::Catch2WithMain
    )
    target_include_directories(test_config_writer PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )
    # ADD_TAGS_AS_LABELS is required, or the [config_writer] tag is not
    # selectable by the anchored `-L '^config_writer$'` gate this plan uses.
    catch_discover_tests(test_config_writer ADD_TAGS_AS_LABELS)

    # --- Window-rule model, matcher and parser unit tests (RULES-01) ---
    #
    # Display-free BY CONSTRUCTION, not by luck: the rules header and its
    # implementation carry no X11 include of any kind, so this target links
    # neither PkgConfig::X11 nor any display fixture and declares no
    # ENVIRONMENT binding. That is the whole reason the rules model is plain
    # data -- every parser and matcher edge case is reachable in milliseconds
    # with no X server, so this suite is cheap enough to run on every commit.
    #
    # Sources are compiled directly into the executable (the test_config
    # pattern immediately above) rather than linked from a library, because the
    # project ships no library target. The config implementation is compiled in
    # because the parser half of RULES-01 lives in Config::applyKeyValue().
    #
    # The acceptance guard for this plan counts literal source-path occurrences
    # in this file (the WM source list plus this target), so those paths are
    # kept out of the prose above on purpose.
    add_executable(test_rules tests/test_rules.cpp src/Rules.cpp src/Config.cpp)
    target_link_libraries(test_rules PRIVATE
        Catch2::Catch2WithMain
    )
    target_include_directories(test_rules PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )
    # D-33: ADD_TAGS_AS_LABELS is required, or the tag this file carries is not
    # selectable by the anchored label gates this plan's criteria use.
    catch_discover_tests(test_rules ADD_TAGS_AS_LABELS)

    # DesktopEntry (.desktop) parsing unit tests (pure data transformation, no X11 needed)
    add_executable(test_desktopentry tests/test_desktopentry.cpp src/DesktopEntry.cpp)
    target_link_libraries(test_desktopentry PRIVATE
        Catch2::Catch2WithMain
    )
    target_include_directories(test_desktopentry PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )
    # The [packaging] cases parse the two .desktop files this repository SHIPS,
    # out of the source tree, rather than a copy written by the test -- so the
    # test needs to know where the source tree is. Same shape as the
    # WM2_SOURCE_DIR definitions the [wm_socket] and [wm2_config_smoke] targets
    # already carry for the same reason.
    target_compile_definitions(test_desktopentry PRIVATE
        WM2_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
    catch_discover_tests(test_desktopentry ADD_TAGS_AS_LABELS)

    # BinaryScanner (ELF DT_NEEDED) unit tests (pure data transformation, no X11 needed)
    add_executable(test_binaryscanner tests/test_binaryscanner.cpp src/BinaryScanner.cpp)
    target_link_libraries(test_binaryscanner PRIVATE
        Catch2::Catch2WithMain
    )
    target_include_directories(test_binaryscanner PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )
    catch_discover_tests(test_binaryscanner ADD_TAGS_AS_LABELS)

    # AppCache (hand-rolled JSON cache + merge/rescan orchestration) unit
    # tests (pure data transformation, no X11 needed). Links DesktopEntry.cpp
    # and BinaryScanner.cpp since loadOrRescan()'s rescan path calls both.
    add_executable(test_appcache tests/test_appcache.cpp src/AppCache.cpp src/DesktopEntry.cpp src/BinaryScanner.cpp)
    target_link_libraries(test_appcache PRIVATE
        Catch2::Catch2WithMain
    )
    target_include_directories(test_appcache PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )
    catch_discover_tests(test_appcache ADD_TAGS_AS_LABELS)
endif()
