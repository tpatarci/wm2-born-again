---
quick_id: 261004-vp6
type: quick
subsystem: ui
tags: [x11, xlib, xft, fontconfig, dpi, palette, menu, border, catch2]

requires:
  - phase: 08.5
    provides: "the silver palette, the derived bevel shades and the allocateShadeOf() helper this task neutered the draw sites of"
  - phase: 09
    provides: "tab-font/menu-font as config keys, the live-apply path, and the one-grab menu loop whose label lambdas carry the inversion"
provides:
  - "A flat window frame, tab and button: no highlight or shadow line on either surface, active or inactive"
  - "frame-background and button-background default to #F0F1F3"
  - "tab-font and menu-font default to DejaVu Sans at pixelsize 13, so the rendered size does not follow the X server's reported DPI"
  - "menu-highlight defaults to #000000 and the selected menu row is a solid bar whose label inverts to the menu background colour"
  - "A measured DPI-independence case with a point-sized negative control"
affects: [phase-10-native-config-tool, ui-refresh, release-notes]

actuals:
  tokens: 19025   # chars/4 over the realized diff, c47facb..990aa67 (76100 chars)
  tasks: 3
  commits: 3
  plan_head_before: c47facb0e7db00c4456089580fcd56b28bd26442
  plan_head_after: 990aa6718c78881e91dec9b2f76d5fa1057e56a6

tech-stack:
  added: []
  patterns:
    - "A removed visual treatment leaves a documented no-op function plus its call sites, with the dropped geometry described in prose above it, rather than unreachable code behind an early return"
    - "A test for an absence carries its own non-vacuity anchor: a colour floor proving the sampled rectangle is the right surface, plus an independent read of the state the old assertion inferred"
    - "A DPI-independence claim is only a measurement when a point-sized control is asserted to differ in the same case"

key-files:
  created: []
  modified:
    - src/Border.cpp
    - src/Buttons.cpp
    - include/Border.h
    - include/Config.h
    - tests/test_wm_runtime.cpp
    - tests/test_wm_config_live.cpp
    - tests/test_config.cpp
    - tests/test_xft_poc.cpp
    - docs/RELEASE-NOTES.md
    - scripts/preflight.sh
    - scripts/capture-display-capabilities.sh
    - apps/wm2-config/AppearancePage.h

key-decisions:
  - "The live-apply bevel case asserts the OBSERVABLE truth (the tab repaints in the new body colour and no shade of either palette is on screen) rather than the structural one (the GCs are still re-derived), because nothing draws with them and a case cannot assert a fact it has no way to reach"
  - "The [wm_bevel] vacuity guard is a tab-background pixel FLOOR, not a dominance test: the tab window is a shaped L, so measured ~19900 px of its bounding box is desktop against ~1500 px of tab"
  - "The category-submenu menu-label case was rewritten too, beyond the plan's named scope, because it counts ink inside the bar the same way and reddened on the same assertions; that also settles the submenu coverage question by measurement rather than by shared-code-path argument"
  - "include/Border.h's member comment block was corrected despite the plan listing it as staying true: 'drawn on the ACTIVE window only' and 'the active window also LIFTS' are now false"
  - "The palette comment's 'R < G < B by 2 per channel' was replaced by the counted arithmetic; by-2 was false of the values it covered"
  - "menu-highlight equal to menu-background is named as a self-inflicted invisible label rather than guarded; a validator rejecting equal colours would be a policy about taste"

requirements-completed: []

coverage:
  - id: D1
    description: "Neither the active nor the inactive window's tab or button carries a pixel of the derived bevel highlight (#F2F3F3) or shadow (#898A8B)"
    verification:
      - kind: integration
        ref: "tests/test_wm_runtime.cpp#Neither the active nor the inactive window's tab wears a bevel"
        status: pass
      - kind: integration
        ref: "tests/test_wm_config_live.cpp#a live tab-background change repaints the tab and puts no bevel shade on screen either side of it"
        status: pass
      - kind: other
        ref: "grep -v '^[[:space:]]*//' src/Border.cpp | grep -c XDrawSegments  == 0"
        status: pass
    human_judgment: false
  - id: D2
    description: "frame-background and button-background default to #F0F1F3; tab/menu background still #C8CACC and every outline still #000000"
    verification:
      - kind: unit
        ref: "tests/test_config.cpp#Config defaults match upstream Config.h"
        status: pass
    human_judgment: false
  - id: D3
    description: "An Xft face loaded from the shipped tab-font and menu-font defaults measures the same ascent and descent at dpi=96 and dpi=120, and the superseded size=12 spelling measurably does not"
    verification:
      - kind: integration
        ref: "tests/test_xft_poc.cpp#the shipped font defaults measure the same at 96 and 120 dpi"
        status: pass
    human_judgment: false
  - id: D4
    description: "In the root menu and a category submenu, the row under the pointer is a solid bar in the menu-highlight colour whose label is drawn in the menu-background colour, with zero menu-foreground ink inside the bar; every unselected row's label stays menu-foreground"
    verification:
      - kind: integration
        ref: "tests/test_wm_runtime.cpp#A selected root-menu row is a bar carrying an inverted label, and the row just left keeps its own"
        status: pass
      - kind: integration
        ref: "tests/test_wm_runtime.cpp#A selected category submenu row is a bar carrying an inverted label too"
        status: pass
      - kind: integration
        ref: "tests/test_wm_config_live.cpp#every menu colour set over the socket reaches the next menu opened"
        status: pass
    human_judgment: false
  - id: D5
    description: "The selected row keeps its label after a hover, after an Expose repaint with the row selected, and the row just left keeps its own -- the three claims [wm_menulabel] exists to defend, now measured in the inverted ink"
    verification:
      - kind: integration
        ref: "tests/test_wm_runtime.cpp#A selected root-menu row is a bar carrying an inverted label, and the row just left keeps its own"
        status: pass
    human_judgment: false
  - id: D6
    description: "docs/RELEASE-NOTES.md states the flat look and the new default values, and no comment in include/Config.h or src/Border.cpp still claims a bevel is drawn"
    verification:
      - kind: other
        ref: "bash scripts/gates/doc-keys.sh"
        status: pass
      - kind: other
        ref: "grep -rn 'DCDEE0\\|A8ACB0' include src apps docs scripts tests  -> empty"
        status: pass
    human_judgment: true
    rationale: "Whether the shipped prose now reads correctly and completely is a judgment about wording, not a property a gate can decide. doc-keys.sh compares the config KEY SETS only, never the prose claims or the default values."
  - id: D7
    description: "The flat look, the pixel sizing and the inverted bar as they actually look on a desktop"
    verification: []
    human_judgment: true
    rationale: "Three visual changes driven by an operator design handoff. Pixel assertions prove the shades are absent, the metrics are DPI-independent and the ink is inverted; whether the result looks right is the operator's call, and the operator already has a standing request for a look at the window manager's appearance (recorded in project memory)."

duration: 1h 55m
completed: 2026-10-05
status: complete
---

# Quick Task 261004-vp6: Flat-look restore Summary

**The tab and its button are flat again as the 1997 original had them, the frame lightens to #F0F1F3, both shipped fonts are sized in pixels so a VNC server's DPI cannot resize the tab label, and the selected menu row is a black bar carrying a silver label.**

## Performance

- **Duration:** 1h 55m
- **Started:** 2026-10-04T22:35Z (approximate; wall clock crossed midnight into 2026-10-05)
- **Completed:** 2026-10-05T00:30Z
- **Tasks:** 3 of 3
- **Files modified:** 12

## Accomplishments

- `Border::drawBevel()` and `Border::drawButtonBevel()` are documented no-ops. Both signatures and all six call sites stay; the removed geometry is described in prose above the definitions rather than left unreachable behind an early return. `m_bevelLightGC`, `m_bevelShadowGC`, the `allocateShadeOf()` derivation at construction and the live re-derivation on a tab-background change are all kept, per the design handoff, with the consequence recorded where they are allocated.
- `frame-background` and `button-background` default to `#F0F1F3`. `tab-font` defaults to `DejaVu Sans:bold:pixelsize=13` and `menu-font` to `DejaVu Sans:pixelsize=13`.
- `menu-highlight` defaults to `#000000`, and the selected menu row is a solid bar whose label is drawn in the menu background colour. The change is in the two label lambdas and nowhere else: they capture the selection variables by reference, so the Expose repaint and the hover redraw cannot answer the ink question differently. `paintPopup`, `setOuterSel` and `setSubSel` needed no edit.
- Four visual claims the suite previously asserted the OPPOSITE of now carry changed assertions, each observed RED first. No test was deleted or weakened; two gained a negative half they did not have.
- A new DPI-independence case reads the patterns from `Config` rather than repeating the literals, so reverting either default reddens it, and asserts the superseded point-sized spelling differs across the two DPIs as its control.

## Task Commits

1. **Task 1 (tracer): Flat tab and button, end to end** — `7c678d1` (feat)
2. **Task 2: Lighter frame and button, both fonts sized in pixels** — `0a523ce` (feat)
3. **Task 3: Menu selection becomes an inverted bar, and the gate chain** — `990aa67` (feat)

Measured, not narrated: `git rev-list --count c47facb..990aa67` = 3.

## Measured results

### RED-first evidence

Every changed visual claim was observed failing against the previous binary before the production code changed.

| Case | RED reading | GREEN after |
|---|---|---|
| `[wm_bevel]` active tab | `activeLight == 0` → 86, `activeShadow == 0` → 69 | 0 and 0 |
| live tab-background | `countOf(before, shippedHighlight) == 0` → 80; `countOf(after, darkHighlight) == 0` → 80 | 0 and 0 |
| new DPI case | `tab96 == tab120` → 19 vs 23; `menu96 == menu120` → 19 vs 23 | 13 == 13 both |
| DPI negative control | already green: `size=12` gives 19 px at 96 dpi, 24 px at 120 dpi | unchanged |
| root `[wm_menulabel]` | `inkHighlighted > 0` → 0; `inkReExposed > 0` → 0; `fgInBarHovered == 0` → 5; `fgInBarReExposed == 0` → 5 | inverted ink > 0, FG in bar 0 |
| submenu `[wm_menulabel]` | `subInkHighlighted > 0` → 0; `subFgInBar == 0` → 8 | inverted ink > 0, FG in bar 0 |

### Gate chain, each run alone on display :99

Quoted from the logs.

| Gate | Result |
|---|---|
| `scripts/gates/build-all.sh debug` | `100% tests passed, 0 tests failed out of 615` — `build-all OK: debug`, EXIT=0 |
| `scripts/gates/build-all.sh release` | `100% tests passed, 0 tests failed out of 615` — `link audit OK (24 entries, all in the intended runtime set)`, `build-all OK: release`, EXIT=0 |
| `scripts/gates/doc-keys.sh` | `doc-keys OK: 36 accepted keys, every one documented, and docs/RELEASE-NOTES.md presents no key the binary refuses`, EXIT=0 |
| `scripts/gates/install-components.sh` | `install-components OK: wm and config-gui staged separately from build/debug`; `bin/wm2-born-again: no GTK/GLib/GObject linkage`, `bin/wm2-ctl: no GTK/GLib/GObject linkage`, EXIT=0 |

One test did not run in both `build-all` configurations: `a socket directory owned by somebody else is refused (Skipped)` — #493 in debug, #510 in release. It skipped identically before this task (it is a root-ownership precondition, not something this change touched) and is recorded here as an observation rather than as a regression.

The debug gate was run TWICE, and the second run is the one that counts. The first ran before two comment/prose-only edits (removing the last two uppercase `DCDEE0` / `A8ACB0` tokens for the plan's sweep) which landed in the Task 3 commit. The second ran at the committed tree `990aa67` and reported the same `0 tests failed out of 615`. `doc-keys.sh` — the only gate that reads `RELEASE-NOTES.md` — was also re-run after those edits and reported the same 36 keys both ways.

### Live menu-colour case, re-run explicitly as the plan required

`tests/test_wm_config_live.cpp:1806`, "every menu colour set over the socket reaches the next menu opened" — **Passed, 2.07 sec**. Planning expected it to survive; this is the measurement. It survives because its `pollUntil` waits for a highlight pixel AND a foreground pixel, and both are still present after the inversion: the bar is still the highlight colour, and rows 1..n keep their foreground labels.

### Sweep

- `grep -v '^[[:space:]]*//' src/Border.cpp | grep -c XDrawSegments` = **0**
- `grep -v '^[[:space:]]*//' src/Border.cpp | grep -c 'drawBevel\|drawButtonBevel'` = **8** (two definitions plus six call sites; floor is 8)
- `grep -c allocateShadeOf src/Border.cpp` = **3**
- `grep -rn 'DCDEE0\|A8ACB0' include src apps docs scripts tests` = **empty**

Every surviving `size=12` is accounted for:

| Where | Why it stays |
|---|---|
| `src/Border.cpp:291`, `src/Border.cpp:690`, `src/Manager.cpp:800` | generic fallback rungs, frozen by decision 09-01 |
| `scripts/preflight.sh:195` (third pattern), `scripts/capture-display-capabilities.sh:273-276` | the same generic rungs, checked/transcribed |
| `tests/test_wm_fallbacks.cpp:512` | the generic ladder rung |
| `tests/test_xft_poc.cpp` 23, 41, 105, 160, 208; `tests/test_wm_fallbacks.cpp:502` | Xft proof-of-concept fixtures, unrelated to the shipped defaults |
| `tests/test_wm_config_live.cpp` 3088, 3107, 3161, 3219; `tests/test_wm_runtime.cpp` 1403, 1670 | arbitrary configured test values |
| `include/x11wrap.h:285` | a grammar example in a comment |
| `tests/test_xft_poc.cpp` 239, 253, 287 | NEW: the negative control and its explanation |
| `include/Config.h` 99, 102, 114, 115 | NEW: the points-vs-pixels rationale and the record of the superseded literals |
| `docs/RELEASE-NOTES.md` 92, 98 | NEW: the user-facing explanation of points vs pixels |

## Files Created/Modified

- `src/Border.cpp` — both draw functions are documented no-ops; six comment sites that claimed a bevel is drawn corrected
- `src/Buttons.cpp` — the two row-label lambdas choose their ink from the selection; nothing else changed
- `include/Border.h` — the member comment block for the two shade GCs corrected (see deviation 2)
- `include/Config.h` — three default values, the palette comment block, the font comment block
- `tests/test_wm_runtime.cpp` — `[wm_bevel]` rewritten with two new vacuity guards; both `[wm_menulabel]` cases re-expressed in the inverted ink with a negative half each; one stale historical comment corrected
- `tests/test_wm_config_live.cpp` — the live bevel case re-pointed at the observable truth
- `tests/test_config.cpp` — three default assertions and two re-titled cases
- `tests/test_xft_poc.cpp` — the new DPI-independence case with its point-sized control
- `docs/RELEASE-NOTES.md` — the look section, the palette values, the pixel sizing, the inverted bar, two table cells, the live-apply row
- `scripts/preflight.sh` — the two shipped patterns the fixture resolves
- `scripts/capture-display-capabilities.sh` — the two shipped patterns in the evidence transcript
- `apps/wm2-config/AppearancePage.h` — the Pango/fontconfig example in a conversion comment

## Decisions Made

Recorded in the frontmatter `key-decisions`. The two load-bearing ones:

**The live-apply case now asserts an observable fact instead of a structural one.** The bevel GCs are still re-derived when `tab-background` changes live, and that is deliberate — but nothing draws with them, so the re-derivation reaches no pixel and no external observation can see it. The case asserts what it can still reach: the tab repaints in the new body colour (the non-vacuity anchor), and no shade of either palette is on screen before or after. Asserting the re-derivation through a stand-in would have been asserting something else.

**The `[wm_bevel]` vacuity guard is a floor, not a dominance test.** The plan specified "each tab histogram's dominant pixel is the tab background". Measured against the tree, that is false and always was: the tab window is a shaped L, so most of its bounding rectangle is root background. The capture reads ~19900 px of desktop white against ~1500 px of `#C8CACC`. A `> 200` floor on the tab background is used instead — the same floor this file's other tab-colour cases use — and it still reddens the moment the capture is mis-aimed.

## Deviations from Plan

### 1. [Rule 1 - Plan expectation did not match the tree] The `[wm_bevel]` dominance guard is impossible

- **Found during:** Task 1, on the first RED run
- **Issue:** The plan's vacuity guard (a) was "each tab histogram's dominant pixel is the tab background `#C8CACC`". It failed with `16777215 (0xffffff) == 13159116 (0xc8cacc)`: the tab is a SHAPED L and the root background shows through most of its bounding box. Histogram as read: `{0xffffff x19920, 0x000000 x1689, 0xc8cacc x1539, 0xdcdee0 x1375}`. The guard could never have passed, flat look or not.
- **Fix:** Replaced with a `countOf(tab, tabBg) > 200` floor on both captures, with the measurement written into the comment so the next reader does not re-propose dominance. Guard (b) — `_NET_ACTIVE_WINDOW == second` — is unchanged from the plan and passes.
- **Files modified:** `tests/test_wm_runtime.cpp`
- **Verification:** RED re-confirmed with the corrected guards (`activeLight == 0` → 86, `activeShadow == 0` → 69, both REQUIREs passed), then green.
- **Committed in:** `7c678d1`

### 2. [Rule 2 - Missing correction] `include/Border.h`'s member comment block was false

- **Found during:** Task 1
- **Issue:** The plan listed `include/Border.h` 318-336 among the sites that "stay TRUE and must not be corrected". Lines 318 and 326-329 say the bevel is "drawn on the ACTIVE window only" and that "the active window also LIFTS". Both are false after this task. The plan's own objective and success criteria require a comment set that does not contradict the code.
- **Fix:** Rewrote the block: the shades are still allocated and no longer drawn with, the one-pixel-never-two rationale is kept as the record, and activity is described as the frame appearing. `include/Border.h` was not in the plan's `files_modified` list; it is now in the task commit.
- **Files modified:** `include/Border.h`
- **Verification:** Both `build-all` gates green; no behaviour change (comment only).
- **Committed in:** `7c678d1`

### 3. [Rule 2 - Missing correction] Three more `src/Border.cpp` comments claimed the draw sites check for null GCs

- **Found during:** Task 1
- **Issue:** Beyond the six sites the plan named, `src/Border.cpp` 94-96, 519-521 and the live-apply block at 486-493 said a null GC is what "every draw site treats as no bevel". The draw sites no longer check anything.
- **Fix:** Reworded all three; the live-apply block also now records that the re-derivation still participates in the allocate-before-release discipline, so a `tab-background` whose shades will not allocate is still refused as a whole.
- **Files modified:** `src/Border.cpp`
- **Verification:** Both `build-all` gates green; the live tab-background case green.
- **Committed in:** `7c678d1`

### 4. [Rule 1 - Plan scope incomplete] The category-submenu `[wm_menulabel]` case also reddens

- **Found during:** Task 3
- **Issue:** `<planning_corrections>` item 4 identified the root-menu `[wm_menulabel]` case as the load-bearing omission from the brief, and Task 3 named only that one. The SECOND case in the same file — "Highlighting a category submenu row does not erase its label either" — counts foreground ink inside the submenu's highlight band in exactly the same way, and reddened on `subInkHighlighted > 0` (0), `subInkHighlighted * 2 >= subInkNormal` (0 >= 8) and the new `subFgInBar == 0` (8).
- **Fix:** Rewritten the same way: unselected rows counted in foreground, the selected row in menu background, plus the negative half. A `subFgInBar = -1` sentinel makes "the bar was never located" fail rather than pass. This also answers the submenu-coverage question the plan left open: the submenu's own ink IS measured, so the summary does not have to record a coverage gap or claim the submenu is covered by a shared code path.
- **Files modified:** `tests/test_wm_runtime.cpp`
- **Verification:** Observed RED (8 px of foreground inside the bar, 0 inverted), green after `src/Buttons.cpp` changed.
- **Committed in:** `990aa67`

### 5. [Rule 2 - Missing correction] The palette block's contrast claim and the release notes' silver list

- **Found during:** Tasks 2 and 3
- **Issue:** "Every value keeps black text above 8.8:1 contrast" becomes false once `menuHighlight` is `#000000`. Separately, the release notes' silver-palette paragraph listed `#A8ACB0` as a member of the cool-cast family; the new highlight is not a silver and does not belong in that list.
- **Fix:** The contrast claim now names the bar as the exception by construction and gives its actual figure (silver on black, about 12.8:1). The silver paragraph lists the two silvers and the highlight is described in its own new section as a bar.
- **Files modified:** `include/Config.h`, `docs/RELEASE-NOTES.md`
- **Verification:** `doc-keys.sh` green (36 keys both ways, and no new backticked hyphenated token was introduced — the `<planning_corrections>` item 1 prohibition held).
- **Committed in:** `0a523ce`, `990aa67`

### 6. [Rule 1 - Plan expectation did not match the tree] `scripts/preflight.sh`'s first two patterns were ALREADY stale

- **Found during:** Task 2
- **Issue:** The plan said the first two preflight patterns "are the shipped faces and are now stale". They were `Noto Sans,DejaVu Sans,Sans:size=12` and `...:bold:size=12` — missing the leading `Ubuntu,` the shipped defaults carried, so they had not matched the shipped faces since plan 09-01. Same in `scripts/capture-display-capabilities.sh`.
- **Fix:** Both replaced with the new defaults, and the comments now name `include/Config.h` as the source rather than a source line number that will drift.
- **Files modified:** `scripts/preflight.sh`, `scripts/capture-display-capabilities.sh`
- **Verification:** `scripts/preflight.sh` run standalone as the first step of Task 2's verify command: passed, both new patterns resolving to readable font files, before the 42-test config selection ran green.
- **Committed in:** `0a523ce`

### 7. [Process] Three task commits landed on `main`

- **Issue:** The executor's pre-commit protocol halts when HEAD is on the default branch. The orchestrator's brief states worktree isolation is off for this project (`workflow.use_worktrees=false`), that `git switch` and `git checkout -b` are blocked by a hook, and that execution is sequential in the main checkout on `main`. `.planning/config.json` has `git.branching_strategy: "none"`. No alternative branch was available, and the previous quick task (`260906-ldw`, `94cfaa3`) also committed on `main`.
- **Resolution:** Committed on `main` as instructed, and recorded here rather than silently. HEAD was asserted attached and on `main` before each commit. Nothing was pushed, no PR opened, no tag created.

---

**Total deviations:** 6 corrections plus 1 process note — 3 plan expectations that did not match the tree (1, 6, and the `[wm_bevel]` guard), 3 missing comment/prose corrections (2, 3, 5), 1 plan scope gap (4).
**Impact on plan:** No scope creep. Every correction either fixed a plan instruction that was factually wrong about the tree, or removed a shipped sentence the code now contradicts — which is what the plan's objective asked for. Two plan-named prohibitions were honoured exactly: no `XDrawSegments` identifier appears anywhere in `src/Border.cpp` including its comments, and no new backticked hyphenated token was introduced in the release notes.

## Issues Encountered

- The `[wm_bevel]` dominance guard failure cost one extra RED cycle. Resolved by measuring the histogram rather than reasoning about the tab's shape, which is what the comment now records.
- The ink counts in `[wm_menulabel]` are small (5 px root, 8 px submenu of exactly-matching pixels) because Xft antialiases the label. The inverted counts are comparable, so the `ink * 2 >= inkNormal` magnitude comparisons survived unchanged and were not loosened.

## Things deliberately left undone

- **`apps/wm2-config/AppearancePage.cpp:118-140` still emits `size=` POINTS** when a user picks a face in the GUI font chooser, so a GUI pick reintroduces the DPI dependence for that user. Named in the plan's `<planning_corrections>` as out of scope, already documented as lossy in that file, and driven by an explicit user action rather than by a shipped default. v1.1 backlog candidate.
- **The generic fallback rungs keep `size=12`** (`src/Border.cpp` 291/690, `src/Manager.cpp:800`). Decision 09-01 froze those literals: a fallback the user can also break is not a fallback.
- **No `tab-bevel` config key.** The handoff offered one as optional and the plan declined it; the flat look is unconditional. Adding a key would also have to pass `doc-keys.sh` on both sides.
- **No static-analysis gate was run**, and none was in this task's gate set. `m_bevelLightGC` / `m_bevelShadowGC` are now written and never read for drawing. If cppcheck or clang-tidy later flags them as unread members, the plan's instruction is to record it as a finding rather than delete the GCs or edit the analysis baseline. Recorded here as the expected finding.
- **`#F0F1F3` has no 16-bit-quantisation measurement.** The palette comment's claim that a RGB565 session quantises the cool cast away is inherited from 08.5-02 and was not re-measured for the new value, whose step is 1 and 2 rather than 2 and 2. Not asserted anywhere as a result.
- **One pre-existing skipped test** (`a socket directory owned by somebody else is refused`) was left skipped. It is a root-ownership precondition unrelated to this change and skipped identically before it.

## Next readiness

- The three visual changes are in and measured. The operator's standing request for a look at the window manager's appearance (project memory, `ui-refresh-reminder`) now has three more changes behind it, and D7 in the coverage block routes that to a human.
- Phase 10 (the toolkit-free Xlib settings tool) inherits the new defaults through `Config` and the same socket, so nothing there needs changing for this task.
- No push, no PR, no tag, no review requested. The CodeRabbit allowance was not spent.

## Self-Check: PASSED

All 12 modified files present. All three commit hashes resolve (`7c678d1`, `0a523ce`, `990aa67`). The five changed defaults read back from `include/Config.h` as `#F0F1F3`, `#F0F1F3`, `#000000`, `DejaVu Sans:bold:pixelsize=13`, `DejaVu Sans:pixelsize=13`.

---
*Quick task: 261004-vp6*
*Completed: 2026-10-05*
