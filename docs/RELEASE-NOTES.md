# wm2-born-again — Release Notes

A modernised resurrection of Chris Cannam's wm2 (1997), adapted for VPS droplets
reached over VNC or RDP. The sideways-tab look is unchanged and deliberately so;
the internals underneath it are not.

**Version 1.0.0** is the first released version. It covers everything below: the
appearance and focus work, window rules, the fallback paths for servers missing
an extension, the configuration file, `wm2-ctl`, and the settings window. See
[README.md](../README.md) for what the project is and who it is owed to.

These notes cover the behaviour that makes the window manager
honest about the servers it runs on: what it does when an X extension is missing,
which windows are allowed to steal your focus, how to write a rule that moves a
window where you want it, and — the part most likely to surprise you — how it
treats a display made of more than one physical screen.

Read the [Limitations](#limitations) section before the feature list. It is
first on purpose.

---

## Limitations

### Single-screen only: the screen is one rectangle, however many monitors are behind it

**wm2-born-again is a single-screen window manager. It treats the whole X screen
as a single rectangle, regardless of how many physical outputs or CRTCs it
spans.**

Concretely: if your session presents a 3840x1080 screen made of two side-by-side
1920x1080 monitors, this window manager sees one 3840x1080 desktop. A window
being kept on-screen is clamped to *that whole rectangle*, not to the nearest
monitor. Maximise fills both monitors. Fullscreen covers both monitors. A window
nudged back into view may land straddling the bezel.

**This is a deliberate design choice, not an oversight.** The target this project
is built for is a VPS droplet reached over VNC or RDP, where the session is one
virtual display of one size. Per-output awareness — Xinerama or RANDR CRTC
geometry, "maximise on the monitor the window is mostly on", per-monitor
workareas — is a real feature that a real desktop needs, and it is a *possible
future capability*, listed as out of scope for this version rather than as
something already half-built.

What you should expect today:

| You have | What happens |
|---|---|
| One monitor, or one virtual remote display | Everything behaves as you would expect. This single-screen case is the tested configuration. |
| Two monitors as one X screen | Windows are managed correctly, but maximise, fullscreen and on-screen clamping all use the combined rectangle. |
| Two monitors as two X screens (`:0.0`, `:0.1`) | Not supported. The window manager manages one screen. |

The window manager does track **resolution changes**: if RANDR is available and
the screen is resized under it — which is exactly what happens when you reconnect
a VNC client at a different size — windows are reflowed back into the new
rectangle rather than stranded off the edge.

### Other things not in this release

- **No virtual desktops or workspaces.** There is one desktop. `_NET_NUMBER_OF_DESKTOPS`
  is fixed at 1. This is why window rules have no "send to workspace" action; see
  [Window rules](#window-rules) below.
- **No keyboard shortcuts or key bindings.** The mouse and the root menu are the
  whole interface.
- **No compositing, transparency, icons or a system tray.** wm2's philosophy, kept.
- **No session management (XSMP).**

---

## Appearance

The silhouette is unchanged — the sideways tab down the left edge of every
window, with the title running down it and a small button at its top. What
changed in this release is the surface.

**Ubuntu, in bold, for tab labels and menus — by default.** The default chain is
`Ubuntu,Noto Sans,DejaVu Sans,Sans`, so a host without the Ubuntu family still
resolves something sensible; nothing about the fallback ladder changed. Bold is
deliberate rather than decorative: on a server without the RENDER extension —
TightVNC, for one — Xft falls back to unantialiased rendering, and bold survives
that where lighter weights go ragged. The tab label is bold and the menu is not,
which is why they are two settings rather than one.

**The two fonts are settings now.** `tab-font` sets the face the sideways tab
label is drawn in, `menu-font` the face of the root menu, each taking a
fontconfig pattern — `Monospace:size=14`, `Noto Sans:bold:size=11` — in the
config file or on the command line as `--tab-font=` and `--menu-font=`. There is
no separate size key on purpose: the pattern already carries the size, and two
ways of saying it could disagree.

**The defaults are sized in pixels, so a remote session's DPI cannot change
them.** `size=12` in a fontconfig pattern means twelve *points*, and a point is
a fraction of an inch — so the actual pixel size depends on the DPI the X
server reports, and a VNC or RDP server's DPI is not something either of us can
predict. The same desktop could come up with a noticeably larger tab label on
one viewer than on another. The shipped defaults now say `pixelsize=13`, which
means thirteen pixels wherever it runs. A pattern you set yourself can still
say `size=12` if that is what you want.

A pattern fontconfig cannot resolve is substituted rather than refused, and the
tab's fallback ladder is deliberately not configurable, so no font value can
leave you without a window manager.

**A font change applies to windows already on screen.** Change `tab-font` with
`wm2-ctl` and every tab already open is re-measured and redrawn in the new face,
tab width and all; change `menu-font` and the next menu you open uses it. If the
new pattern has no usable face at all, the change is refused and the face you
had stays loaded — you cannot end up with unlabelled tabs by mistyping a font
name.

**A silver palette with black text.** The defaults are a single cool-cast
family, `#C8CACC` for the tab and menu and `#F0F1F3` for the frame and its
button. Every silver is very slightly blue — the blue channel is the highest of
the three in each of them — which is what makes it read as metal rather than as
concrete. On a 16-bit remote session that cast quantises away and you get plain
grey, which is simply the older look rather than a broken one.

**Flat surfaces, as the original had them.** The tab, its button and the frame
are each a single flat colour inside a one-pixel black outline. There is no
highlight or shadow line along any edge, on the focused window or any other —
the 1997 wm2 was flat and this release is flat again.

Knowing which window has the focus does not need an edge: an unfocused window's
frame is hidden entirely, so activity is already something you can see, and it
was that way before any raised edge existed. That is also why there is nothing
to configure here and no way to get it inconsistent.

The two shades a raised edge would be drawn in are still worked out from
whichever tab background you configure, so a future look that wants one back
would get shades belonging to your palette rather than a fixed near-white line.
Nothing draws with them today.

All nine colours remain configurable in the config file and on the command line,
and as of this release the two fonts, `tab-font` and `menu-font`, are
configurable the same way. The nine, in full, so there is no "and the rest" for
you to go looking for:

| Key | What it colours |
|---|---|
| `tab-foreground` | the title text running down the sideways tab |
| `tab-background` | the tab itself |
| `frame-background` | the window frame around a focused window |
| `button-background` | the small button at the top of the tab |
| `borders` | the outlines of the frame and the tab |
| `menu-foreground` | the root menu's text |
| `menu-background` | the root menu's background |
| `menu-highlight` | the row of the root menu under the pointer |
| `menu-borders` | the root menu's border |

Each takes anything the X server can parse — a name like `slategray`, or a
`#RRGGBB` value. A value the server refuses is refused rather than substituted,
and the colour you had stays.

---

## Focus behaviour

### Focus-stealing prevention is on by default

A window that maps itself while you are typing somewhere else **does not get your
focus**. The window manager compares the window's `_NET_WM_USER_TIME` against the
time of your last real interaction; a window that cannot show it was opened
because of something *you* did is mapped, framed and left unfocused.

This is on by default because the alternative — the behaviour most window
managers shipped for a decade — is that a slow-starting application steals your
keystrokes half a sentence into a different window.

If you want the old behaviour:

```
focus-stealing-prevention = false
```

or on the command line, `--no-focus-stealing-prevention`.

The same arbitration applies to `_NET_ACTIVE_WINDOW` requests: a request from a
pager or a taskbar is honoured, because a pager only sends one when a human
clicked it. A request from an application asking for itself is subject to the
same user-time test as a map.

### The three focus-policy settings now actually work

**This is a bug fix, and it is worth stating plainly: `click-to-focus`,
`raise-on-focus` and `auto-raise` were parsed and then ignored.** They had been
dead settings since the configuration struct was written. They are wired to
behaviour now, and each one is covered by a test that drives the real binary
with real pointer events.

If you had these in your config file and wondered why nothing changed — nothing
was changing. It will now.

**The documented defaults were corrected to match what the binary already did**,
rather than the binary being changed to match the documentation. Nobody's
existing session behaves differently as a result:

| Setting | Default | What it does |
|---|---|---|
| `click-to-focus` | `false` | When false, focus follows the pointer. When true, you must click. |
| `raise-on-focus` | `true` | A window that takes focus is also raised. |
| `auto-raise` | `true` | A window under a pointer that has stopped moving is raised, after `auto-raise-delay` (default 400 ms). |

### Right-click circulation no longer freezes the window manager

Right-clicking the root window to cycle through windows could wedge the window
manager at 100% CPU, permanently, and it did not take an exotic situation to
trigger — a right-click on a freshly started session with nothing open was enough.
Fixed, with a regression test that asserts both that the window manager is still
*responsive* and that it is still *idle*, because "alive" and "working" are
different claims and the bug satisfied the first.

---

## Window rules

Rules let you say "this application always opens there, that size, without a
frame". They live in the same `key = value` configuration file as everything else
— there is no second file format and no numbered-key scheme to keep in step.

```
rule-match-class    = Firefox
rule-match-instance = navigator
rule-position       = 100,100
rule-size           = 800x600

rule-match-type     = dialog
rule-no-decorate    = true
```

### Where a rule begins

One sentence, and it is the only thing you have to remember:

> **A new rule begins at the first `rule-match-*` line that follows an action
> line — or at the very first rule line in the file.**

Consecutive match lines attach to the rule that is currently open and are AND-ed
together, so the first block above matches a window that is *both* class `Firefox`
*and* name `navigator`. Action lines attach to that same open rule. Keys that are
not rule keys are transparent: putting `frame-thickness = 9` in the middle of a
rule does not end it.

### What you can match on

| Key | What it matches |
|---|---|
| `rule-match-class` | The `WM_CLASS` hint. The forgiving one: it is tested against **both** WM_CLASS fields, so "Firefox" works whether that is the instance name or the class name. |
| `rule-match-instance` | The `WM_CLASS` **instance name** only. The precise one. |
| `rule-match-title` | The window's **title** — the text the application puts in its own title bar, and the text this window manager paints down the sideways tab. |
| `rule-match-type` | An EWMH window type. Exactly four are accepted: `normal`, `dock`, `dialog`, `notification`. |
| `rule-match-mode` | How the text matches — exact or substring. |

Criteria combine with AND: every one you set must match. A rule with no criteria
at all matches **nothing**, so a typo that drops your only match line disables
that rule rather than applying it to every window on the screen.

**A title rule looks at the title the window has when it opens.** Rules are
applied once, at the moment a window appears, and are not re-checked afterwards.
So a rule keyed on a title the application only sets later will not fire — and,
just as deliberately, renaming a document will not make its window jump to the
position some rule specifies. That second half is the reason for the first: a
window that re-positioned itself every time its title changed would be unusable
in a text editor or a browser. Most window managers resolve it the same way.

If you want to catch an application whose title changes, match on its class
instead — `rule-match-class` and `rule-match-instance` read the `WM_CLASS` hint,
which applications set once and rarely change.

> **If you used `rule-match-name` before v1.0**, rename it to
> `rule-match-instance`. It compared the `WM_CLASS` instance name while its name
> said `WM_NAME`, which was confusing enough to have put a wrong sentence in this
> very document. The old spelling is gone rather than kept as an alias, so a
> configuration still using it will warn about an unknown key on startup.

**Only those four window types are accepted.** `utility`, `splash` and `toolbar`
are *not*: the window manager collapses them into `normal` internally, so a rule
naming one could never fire. Writing one produces a warning and leaves the
criterion unset — and since a rule with no criteria matches nothing, a rule whose
only line was `rule-match-type = utility` silently does nothing at all. Use
`normal` for those windows.

### The four actions that ship

| Action | Effect |
|---|---|
| `rule-no-decorate` | The window is managed but gets no frame and no tab. |
| `rule-position` | `x,y` — where the window is placed when it maps. |
| `rule-size` | `WxH` — the size it is given when it maps. |
| `rule-skip-taskbar` | `true`/`false` — sets `_NET_WM_STATE_SKIP_TASKBAR` so pagers and taskbars leave the window out. |

**There is deliberately no "send to workspace" action.** This window manager is
single-desktop by design, so there is no second workspace for a rule to send a
window to. The requirement that originally named a fourth workspace action was
amended rather than left as a promise that could never be kept — implementing it
would mean acquiring a desktop model this project does not intend to have.

### Rules are configuration-file only

There are no `--rule-*` command-line flags, and that is a decision rather than an
omission. Repeated ordered groups do not fit a command line: there is no way to
express "this `--rule-position` belongs to *that* `--rule-match-class`". Every
other setting has a flag; these do not.

---

## Behaviour on servers that are missing pieces

Remote-desktop X servers vary enormously in which extensions they offer. Three
matter to this window manager, and none of the three is fatal any more. You can
check what a given server offers with the capability-capture script shipped in
this repository:

```
bash scripts/capture-display-capabilities.sh :2 my-session
```

### Without the Shape extension

The frames and tabs are **rectangular** instead of shaped. Everything works —
moving, resizing, the tab, the button, the menu — it simply does not have the
cut-out silhouette. Importantly, on a server without Shape the window manager
issues **no Shape requests at all**, rather than issuing them and having each one
rejected.

You can see this fallback on a server that *does* have Shape by setting
`WM2_FORCE_NO_SHAPE=1` in the environment, which is how it is tested.

### Without RANDR

Screen geometry is read once at startup and the window manager does not learn
about later resolution changes. Windows are not reflowed when you reconnect a
client at a different size, so a window that was on the right of a wide desktop
can end up off the edge of a narrower one. Everything else is unaffected. The
equivalent environment lever for testing is `WM2_FORCE_NO_RANDR=1`.

### Without XRender

Font rendering falls back to the X server's core glyph path inside libXft. This
was measured on a RENDER-less server during development, and the good news is
better than expected: **the sideways tab still renders rotated**. The wm2 visual
identity survives a server with no RENDER extension.

What can still differ, and what you should not be surprised by: on a server that
is *also* short of fonts, a tab label may render unrotated, or be absent
altogether. The window manager degrades down a ladder — the preferred font family
chain, then a generic sans chain, then an unrotated face, then no label — and it
keeps running and keeps managing your windows at every rung. It will not exit
because it could not find a font. Earlier versions did exactly that.

If your labels are missing or wrong, the fontconfig section of the capability
capture above tells you which of those rungs your server landed on.

---

## Remote-desktop support status

The stated target set for this project is TigerVNC, TightVNC, XRDP and X2Go.

| Target | Status |
|---|---|
| **Xvfb** (headless) | **Validated continuously.** The entire automated suite — hundreds of cases, including everything that drives the real compiled binary — runs on Xvfb on every build. |
| **Xephyr** (nested) | **Validated.** Runtime smoke transcript captured in the release evidence bundle. |
| **TigerVNC** | **Validated.** Real session, human at the client. SHAPE, RANDR and RENDER all present; capability transcript, root properties and window tree committed under the phase evidence bundle. |
| **XRDP** | **Validated.** As above, same extension result. |
| **X2Go** | **Partially validated.** `nxagent 3.5.99.26` — the X server X2Go uses — was captured headlessly, nested on a local Xvfb. It advertises SHAPE, RANDR and RENDER (23 extensions in total), and the window manager frames clients on it at the standard `+24+8` client offset. What was **not** exercised is X2Go's own path: its agent start-up wrapper, with an NX compression proxy over SSH in front of the agent. Transcript committed under `.planning/phases/08.5-v1.0-closeout/evidence/x2go-nxagent/`, whose scope note states that difference. See below. |
| **TightVNC** | **Validated.** Headless capability capture against `TightVNC 1.3.10` (from `tightvncserver 1:1.3.10-5`), started directly, with no viewer attached and no network-exposing flag. It advertises seven extensions in total — SHAPE present, RANDR absent, RENDER absent — which makes it the only server in this project's evidence that genuinely lacks the extensions the fallbacks were built for. The window manager started on it, both fallback ladders announced themselves on stderr, a client was framed, and no X protocol error was logged. Transcript committed under `.planning/phases/08.5-v1.0-closeout/evidence/tightvnc/`, which records beside it what the capture does not settle. See below. |

All four stated targets now have a committed capability transcript, each captured
at a known commit. Two of them — TigerVNC and XRDP — were exercised in real
sessions with a person at the client. The other two — TightVNC, and `nxagent`,
the server that sits behind X2Go — were captured headlessly, with no viewer
attached and nobody in the loop. On that evidence the project requirement
covering "compatible with all four out of the box" is **met**.

Four transcripts is not the same thing as four fully exercised targets, and the
table above keeps those apart on purpose. What a headless capture settles is the
extension surface a server offers, and that this window manager starts, frames
and manages windows on it. What it does not settle is how that target *feels* to
use — whether the tab label looks right, whether a drag or a menu behaves over a
real connection. That is what the two sections below, and the interaction
checklist in the release evidence bundle, are for.

### X2Go: measured at the agent, untested through the proxy

X2Go is **partially** validated for this release, and the two halves are worth
keeping apart.

**What was measured.** `nxagent 3.5.99.26` — the X server X2Go runs — was
captured headlessly, nested on a local Xvfb, with this window manager started on
it and a client framed. It advertises SHAPE, RANDR and RENDER, 23 extensions in
total, and the window manager's own capability probes agree with `xdpyinfo`. The
client is reparented at the standard `+24+8` offset and appears in
`_NET_CLIENT_LIST`. The whole capture took about two minutes, with nobody sitting
through it.

That corrects two things an earlier draft of this document asserted. It said the
X2Go server package was absent from the validation host; `/var/log/dpkg.log`
records `x2goserver-common:all 4.1.0.3-5` installed at **2026-08-29 17:43**, so
it was there all along. And it said none of the TigerVNC or XRDP result carried
over, on the grounds that `nxagent`'s extension surface was unknown. It is known
now, and it is the same surface.

**What was not measured, and this is the part that matters.** X2Go does not
simply run `nxagent`. It starts the agent through `x2gostartagent`, with an **NX
compression proxy** between agent and client over SSH — and that proxy sits in
front of exactly the extension surface everything above depends on. Nothing in
this capture exercises it. Specifically untested:

- menus and drag-move under compression and network latency;
- the long-press delete timing, the one interaction with a real clock in it;
- resize behaviour when a session is reconnected at a different geometry;
- whatever the proxy does or does not forward for SHAPE.

Nothing was judged by eye either. This was a headless capture: no screenshot, and
no verdict on how the sideways tab looks.

**So this is a floor, not a tick.** The transcript directory carries a `SCOPE.md`
that says so in the same words, because three green files in a directory named
for X2Go read, at a glance, like "X2Go: tested".

What to expect meanwhile: X2Go is unexercised, not unsupported. If the sideways
tab renders wrongly or frames come out unshaped under `nxagent`, that is a real
bug worth reporting — and given the RENDER dependency it is the likeliest place
for one to be hiding.

### TightVNC: measured, and the result disproved the reason for skipping it

TightVNC **is** validated for this release, headlessly. It turned out to be the
most informative capture in the bundle, because it is the only one taken against
a server that genuinely lacks the extensions this window manager builds fallbacks
for.

| Server | Extensions | SHAPE | RANDR | RENDER |
|---|---|---|---|---|
| TigerVNC 1.12.0 | many | yes | yes | yes |
| **TightVNC 1.3.10** | **7** | **yes** | **no** | **no** |

TigerVNC 1.12 is a current server; TightVNC's Unix server side is still 1.3.10,
from 2009. An earlier draft of this document skipped TightVNC on the argument
that a TigerVNC transcript was evidence enough for it, the two servers being
related. On the two extensions that argument named, the two servers do not
overlap at all. The argument was retired rather than softened, and why it failed
is kept in `COMPILED_CODE_BEHAVIOR_CHECKLIST.md`, because the general form of it
is worth recognising: a family resemblance between two servers is not a
measurement of either one.

**What the window manager did about it.** It started, and both fallback ladders
announced themselves on stderr rather than degrading in silence:

```
  Shape extension available.
wm2: warning: no xrandr extension, screen geometry will track resolution changes
     via the root window only
wm2: warning: no xrender extension, tab labels will be drawn through the core
     X11 glyph path
```

The client was framed normally — frame, tab, button and resize handle all
present — and no X protocol error was logged. There is one visible difference
from the RENDER-capable targets, and it is expected: the client sits at `+21+8`
inside its frame here rather than at `+24+8`, because the tab width is measured
from whatever font fontconfig resolves, and a lower rung of the font ladder
resolves a different one. The frame narrows to match. The geometry follows the
font, exactly as it is designed to.

**What this does not tell you.** Nothing was judged by eye: no viewer was
attached and no screenshot was taken, so whether the core-X11 glyph path *looks*
acceptable is unanswered — and that is precisely where a fallback tends to be
ugly rather than broken. No interaction was exercised over a real TightVNC
connection. And resolution changes were not tested, which on a server with no
RANDR is the interesting case, since the fallback watches the root window
instead. Those limits travel with the transcript, in its own `SCOPE.md`.

### A note on how "compatible" should be read

Each validated target is validated by a session on one server version, on one
host, on one day. That is what a manual compatibility check *is*; it is not
continuous testing, and a different server version could behave differently. The
committed transcripts record the exact server, extension set and commit each
result belongs to, so a later surprise can be attributed rather than argued about.

---

## Resource use

The project constrains itself to running comfortably on a 512 MB VPS alongside a
VNC server. From this release that is a test the suite enforces rather than a
sentence in a document:

- **Resident memory** with twenty windows open is asserted against a fixed budget
  established from a separate calibration run. Measured on the reference host:
  **11.8 MB** with twenty windows managed, against a 24 MB budget. A managed
  window costs roughly six kilobytes.
- **Idle CPU** with twenty windows open and no input is asserted to stay under 1%
  of wall clock over a thirty-second window. Measured: **zero CPU ticks**. The
  event loop blocks; it does not poll.

### The settings window costs a fifth of a 512 MB machine while it is open

This is the number this release most wants you to see, because it is larger than
the project expected and it was measured rather than estimated.

Measured on a real TigerVNC 1.12.0 session, 1280x1024x24, release build, with
the settings window open and each of its three pages visited:

| Process | Resident | Of a 512 MB machine |
|---|---|---|
| `wm2-config`, the first one opened in a session | **98.7 MB** | 19% |
| `wm2-config`, a second one opened in the same session | 49.4 MB | 10% |
| `wm2-born-again`, running alongside it | 11.8 MB | 2% |
| the VNC server itself (`Xvnc`) | 74.2 MB | 14% |

**What that means on the target machine.** A 512 MB droplet running this window
manager and a VNC server sits at about **86 MB, a sixth of the machine**. Open
the settings window and that becomes about **189 MB, better than a third** — the
settings window alone is roughly a fifth of the whole machine for as long as it
is up. That is affordable, because you open it, change something and close it,
and it would not be affordable as a resident part of the desktop. It is not:
nothing starts it for you, and closing it gives the memory back.

The window manager's 11.8 MB here is the same figure the automated budget
measures on Xvfb, which is worth saying: the window manager costs the same on a
real VNC server as it does headless.

The cost is GTK's, not this project's. `wm2-config` is a few thousand lines over
a toolkit that maps a large amount of shared library, theme, icon and font
machinery into every process that links it, and the split above is a property of
that machinery rather than of anything in this repository.

**Two figures rather than one, and the difference is not explained.** The first
settings window opened on a freshly started X server holds about twice what
every later one on the same server holds. That split reproduces on Xvfb and on
TigerVNC alike, on three runs of each. It is *not* the per-user fontconfig cache
(a fresh home directory on every run still shows the low figure from the second
launch onward) and it is *not* the cost of being the first client on the server
(an `xclock` connected first changes nothing). What it actually is was not
determined, and is written down here as an open question rather than guessed at.
The higher figure is the one quoted above and the one the automated budget is set
against, because a real session opens the settings window once.

**If this matters to you, do not install the settings window.** That is what the
two-package split is for: the `wm` package has no GTK dependency at all, and
`wm2-ctl` changes every setting the window does, over the same socket, from a
shell. The measured figure for that route is the window manager's own 12 MB and
nothing else.

**Both figures are enforced.** The window manager's 24 MB budget and the
settings window's 160 MB budget are ctest cases, not sentences — the second reads
the same `/proc` field through the same reader, so the two numbers above are
comparable rather than merely adjacent. The remote-desktop session that produced
the release figures is recorded, server version and commit included, under
`.planning/phases/09-config-gui-ipc/evidence/remote-desktop/`.

---

## The settings window

`wm2-config` is a small GTK 3 window with three pages that edits the same
configuration this document describes — the colours and fonts, the focus and
timing behaviour, and your own root-menu entries. It is an **optional, separately
packaged** program: the window manager neither requires it nor links anything it
uses.

**It changes the desktop as you type, and writes only when you say so.** When a
window manager is running it connects to the same socket `wm2-ctl` uses, so a
colour or a font applies to the windows already on your screen immediately. Save
is what writes the configuration file. Revert puts the form back to the values on
disk, and closing with something unsaved asks rather than assuming.

**With no window manager to talk to it still works**, in file-only mode, and says
so in a banner across the top: *"Not connected to a running wm2-born-again;
changes take effect at next start"*. That is the case where you are fixing a
configuration over SSH before starting a session.

**A save edits your file rather than rewriting it.** Keys you did not touch keep
their place, their spelling and the comments around them. Resetting a setting
*removes* its key from your file rather than writing the built-in default into
it, so whatever the system-wide file said shows through again — including a
system-wide file that changed since you opened the window, which the settings
window re-reads whenever the window manager does.

**A save writes your own menu entries and nobody else's.** Manual root-menu
entries add up across the layers — a system-wide file's entries come first, then
yours — and the Menu page shows you the whole list, because that is what the
running desktop has. What a save writes into *your* file is only the part your
file owns, so a system-wide entry is never copied into your file and never
appears twice on the menu afterwards. Resetting the page removes your entries
and leaves the system-wide ones showing through, which is what resetting means
everywhere else in this window.

**A colour you type is stored in the spelling the X server reads.** The colour
fields accept more spellings than the window manager can use — the toolkit
behind them understands the CSS forms a web page uses, and the X server
understands none of them — so whatever you type is converted to the plain
hexadecimal form before it goes anywhere: type `rgb(200,202,204)`, or
`SteelBlue`, or `#c8cacc`, and what the form holds, what the desktop applies and
what a save writes is `#C8CACC`. It is the same colour either way. The
conversion matters because a colour the X server cannot parse is fatal to the
window manager at *startup*, so a spelling that survived a save could stop your
desktop from coming up the next time.

**A change the window manager refuses is taken back out of the form.** If the
running desktop rejects something you set — a colour the X server will not
allocate, a value outside its range — the field goes back to the value that was
in force when the change was sent, rather than sitting there looking accepted,
so a save can never write a setting the desktop has already turned down.

**A save waits for the desktop to answer.** The window manager's answer to a
change comes back a moment after the change is sent, so a Save pressed in that
moment would write a value nothing had accepted yet — and if the answer were a
refusal, the refused value would already be in your file with nothing left to
put it back to. Save therefore waits for the last answer to arrive and then
runs, saying so on the status line while it waits. In practice that is a
fraction of a second and you will not see it; when the desktop has gone away
entirely the save runs anyway, against your file alone.

**A setting's tooltip names the file it is set in, not the file whose value it
happens to match.** A key you set in your own file to the same value the
system-wide file already gave it is still *your* setting, and says so; after a
save, an edited setting names your file and a reset one names whichever layer is
now supplying the value. Where there is more than one system-wide configuration
directory, the file named is the one that actually sets that setting — not
simply the last directory on the list — so following the tooltip takes you to a
file that mentions what you were looking at.

**A save it cannot do safely, it refuses rather than guesses.** Four cases, each
reported in the window rather than left to be discovered later:

- **The file is a symbolic link.** If `~/.config/wm2-born-again/config` is a link
  into a dotfiles repository, saving would replace the *link* with a regular file
  and every later edit would go somewhere the repository could not see. The save
  is refused and says so; edit the file the link points at.
- **The file exists but could not be read.** A permission, a name that is too
  long, a link loop — anything that stops the save reading what is there is a
  read failure, never treated as "there is no file here". Nothing is replaced.
- **A value begins or ends with a space.** The configuration file trims values as
  it reads them, so ` xterm` would come back as `xterm` and the file and the
  running desktop would quietly disagree. Spaces *inside* a value are fine.
- **Somebody else is saving at the same time.** Two settings windows, or one
  window and a hand edit through another program, no longer overwrite each
  other: a save waits for the one in progress and then re-reads, so both sets of
  edits survive. The wait is bounded at two seconds. If whatever holds the file
  is still holding it then — a backup tool, a dotfile syncer, a shell running
  `flock` over `~/.config/wm2-born-again` — the save is **refused** with *"another
  program is writing … try saving again in a moment"* rather than freezing the
  window: nothing is read, nothing is written, and your edits are still in the
  window to save again.

**How to open it.** When `wm2-config` is installed and on the window manager's
`PATH` at the moment the window manager starts, the root menu carries a
`Configure...` entry at the bottom of its top level. Install it afterwards and
the entry appears the next time the window manager starts — the check is made
once, deliberately, rather than on every menu you open. It also installs an
application entry, so it appears under **Settings** in the root menu's list of
discovered applications, and it can of course be run from a shell.

Its resident memory is worth knowing before you install it; see
[Resource use](#resource-use) above.

---

## Changing settings while it runs

A running window manager listens on a **Unix domain socket of its own**, and
this release ships `wm2-ctl`, a small command that talks to it. That is the
whole feature: you can change a setting on a desktop that is already open,
from a shell, without restarting the window manager and without closing and
reopening a single window.

`wm2-ctl` is part of the window-manager package rather than of any graphical
tool, and it links no X11, no Xft and no GTK. It needs no display of its own.
The point is a droplet reached over SSH with nothing on the screen: you can
still ask the window manager what it is doing and tell it to change.

Four subcommands, and no others:

| Command | What it does |
|---|---|
| `wm2-ctl status` | Print what the window manager is doing — its version, the protocol version, how long it has been up, the screen size, and how many windows it is managing and hiding. |
| `wm2-ctl get KEY` | Print the value the window manager is **actually using**, which after a `set` is not necessarily what any file on disk says. |
| `wm2-ctl set KEY VALUE` | Change a setting on the running desktop, now. |
| `wm2-ctl reload` | Re-read the configuration files from disk and apply the result. |

`wm2-ctl --help` lists every settable key, and the list is generated from the
same table the window manager's own `--help` comes from, so the two tools
cannot advertise different settings.

**A `set` changes the running desktop and writes nothing.** No file is touched,
so nothing you edited by hand is rewritten and no comment of yours is lost —
and `wm2-ctl reload` therefore discards a `set` and puts the window manager
back on what the files say. That is deliberate: it makes "try it and see" free
of consequences. If you want a change to survive a restart, put it in the
config file and reload.

**A value the config file would silently correct is refused here instead.** The
socket uses the *same* parser the config file uses — there is no second, laxer
route into the window manager's state — but where the file clamps a
`frame-thickness` of 500 down to 50 and warns on stderr, `wm2-ctl` tells you the
value is out of range and changes nothing. A file is read once by somebody who
can look at the warning; a command has somebody waiting for an answer, and
answering "yes" to a request nobody made is worse than saying no.

The same applies to the text settings. The config file trims the spaces off both
ends of a value and drops one longer than 256 characters, so `wm2-ctl set` does
the same: a value with surrounding spaces is accepted and stored trimmed —
`wm2-ctl get` afterwards shows you what a file would have read back, not the
bytes you sent — and a value over the limit is refused, naming it, as is one
with a newline inside it, which a file of one value per line has no way to
hold. Nothing reaches the running desktop by this route that a file could not
have carried.

Exit codes, so a shell script can tell the cases apart:

| Code | Meaning |
|---|---|
| `0` | Acknowledged. |
| `1` | The window manager refused the request — the reason is on stderr, naming the key. |
| `2` | There is no window manager to talk to. |
| `3` | Usage error: `wm2-ctl` could not work out what was being asked. |

`1` and `2` are deliberately different: "running and refused" is not the same
situation as "not running", and a script that treats them alike will do the
wrong thing in one of them.

**Which settings apply live: all of them.** There is no setting here that waits
for a restart. Every setting `wm2-ctl --help` names changes the desktop you are
looking at, at the moment you set it, with no window closing and no restart:

| Setting | What moves |
|---|---|
| the nine colours | every frame, tab, button, outline and the next root menu repaint in the new colour |
| `frame-thickness` | the geometry of every frame, tab and resize handle already on screen |
| `tab-font` | every open tab is re-measured and redrawn — the tab gets wider or narrower with the face |
| `menu-font` | the next root menu's row height |
| `click-to-focus`, `raise-on-focus`, `auto-raise`, `focus-stealing-prevention` | the very next interaction |
| `auto-raise-delay`, `pointer-stopped-delay`, `destroy-window-delay` | the next interaction that waits on them |
| `new-window-command`, `exec-using-shell` | what the menu's **New** entry runs next time you choose it |
| the manual root-menu entries | what the next root menu you open contains; a menu that is already open is left alone and picks the change up the next time |

**A reload that moves several of these at once applies all of them.** A file
that changes `frame-thickness` *and* `tab-font` together re-lays every open frame
out once and redraws its tab label in the new face — not the new thickness
wearing the old glyphs.

**A window that is fullscreen at the time keeps up too.** A fullscreen window has
no frame on the screen to change — its decoration is taken away for as long as it
is fullscreen — so a colour or a `frame-thickness` set while it is fullscreen has
nothing to act on at that moment. The change is not lost: the window comes back
out of fullscreen wearing whatever the settings are *then*, not the ones it went
in with.

A value the running window manager cannot use — a colour the X server will not
parse, a font pattern with no usable face — is **refused**, and everything keeps
the value it had. You cannot leave the window manager without a colour or
without a face by mistyping one. A file that changes *two* fonts and gets one of
them wrong is refused whole: neither face is swapped, so `wm2-ctl get tab-font`
never names a face that is not what you are looking at.

**A reload that cannot look at a configuration file is refused.** A file that
is simply not there is the ordinary state of a machine nobody has configured,
and a reload on such a machine loads whatever the other layers say and succeeds.
A file that cannot be *examined* is a different thing: a directory above it
whose permissions were changed, a symbolic link pointing round in a circle, a
disk going bad. Rather than read that as "there is no file here" — which would
apply the remaining layers and report success, silently dropping every setting
that file owns — the reload is refused, naming the file and what the system said
about it, and the desktop keeps what it had.

This covers **every** file the reload would read, not only your own: the
system-wide files are checked by the same rule, so a system-wide configuration
that was in force at startup and has since become unreachable stops the reload
instead of vanishing from it.

**Three settings say "not now" while a menu is open or a window is being
dragged.** `tab-font`, `menu-font` and `frame-thickness` move geometry that an
open root menu or a move/resize drag has already measured — the menu's row
height, the tab's width, the frame's thickness — so setting one of them mid-grab
would leave the menu highlighting a different row from the one it activates, or
the dragged window jumping sideways. They are refused with
*"a menu or a drag is in progress; try again in a moment. Only `tab-font`,
`menu-font` and `frame-thickness` are affected, and a reload that moves any of
them is refused whole"* (`wm2-ctl` exit code `1`), and the same request succeeds
the moment you let go. Every other setting applies under a grab exactly as it
does at rest.

The last clause is worth reading twice, because it is the one surprise here: a
**reload** applies a whole configuration file, so if the file moves one of those
three while a menu is open, the reload is refused **entirely** — the six colours
it also changed are not applied either. Nothing is half-applied, which is the
point; run `wm2-ctl reload` again once the menu is closed and the whole file goes
in at once.

**A notice never looks like a refusal.** Because a reload notice and the reply
to your own `wm2-ctl reload` are the same kind of message, a reload triggered by
something else — a settings window pressing *Re-read files*, a second
`wm2-ctl reload` — can arrive on your connection ahead of your own reply.
`wm2-ctl` skips it and waits for the answer to the request it made, so a `set`
that succeeded always exits `0`.

**The manual menu entries travel as one value.** `wm2-ctl get menu-entries`
prints your whole list in the config file's own key order, with `;` between
records, and `wm2-ctl set menu-entries ...` replaces the whole list with what
you send:

```
wm2-ctl set menu-entries \
  'menu-entry-name=Editor;menu-entry-command=/usr/bin/vim;menu-entry-category=Custom'
```

An empty value removes every manual entry. It is one value rather than a
command per row because the entries are an ordered list — to change one, read
the list, change that record, and send it back.

**A `;` cannot appear inside a name, a command or a category.** It is what
separates one record from the next and there is no escape for it, on the socket
or in the config file: an entry whose value contains one is refused, with a
warning naming the key, and the rest of the file is read as usual. That rule is
what makes the round trip safe — the list `wm2-ctl get menu-entries` prints is
always a list `wm2-ctl set menu-entries` will take back unchanged. If a program
you want on the menu needs a `;` on its command line, put the command in a shell
script and name the script here.

**A menu entry the configuration file could not hold is refused on the socket
too.** The file stores one value per line and drops a value longer than 256
characters, so a name, a command or a category that breaks either rule is
refused where it arrives rather than accepted live and then lost at the next
reload — or refused later by a settings window's save, citing a rule you were
never shown. The refusal names which of the two rules it was. This is the same
principle the single-value settings follow above: nothing reaches the running
desktop by the socket that a file could not have carried.

**The whole list has to fit in one message.** Because it travels as a single
value, there is a limit on the total — a few dozen entries with ordinary names
and commands — and the limit is applied where the list is *built*: a config file
with more entries than will fit is read up to that point, the rest are dropped
from the end in file order, and a warning on stderr says how many went and why.
That way what the window manager is holding is always something it can tell a
settings window about, rather than a list it loads happily and then cannot
describe. The settings window refuses to send an over-long list for the same
reason, with a sentence naming the limit.

In the config file the same list is three keys per entry, in the same
begins-a-new-record shape the rule keys use:

```
menu-entry-name     = Editor
menu-entry-command  = /usr/bin/vim
menu-entry-category = Custom
```

`menu-entry-name` opens a new entry; `menu-entry-command` is the program it
runs, split on whitespace with **no** shell evaluation and no quote handling, so
an `&&` or a pipe you type is one literal argument rather than the start of a
second command (a semicolon is the one character that is refused outright, for
the reason just given); `menu-entry-category` is the submenu it appears under,
and defaults to `Custom` when you leave it out. There are no `--menu-entry-*`
command-line flags, for the same reason there are no `--rule-*` ones.

**One key is readable and not settable: `menu-categories`.** `wm2-ctl get
menu-categories` prints the category names the next root menu will show —
whatever discovery found on this machine, plus your own manual entries' — as one
`;`-separated value. It is refused by `set`, and refused as an unknown setting
rather than by a special case, because the list is *derived* from two things and
setting a view of two things would mean setting neither. It exists so the
settings window's category dropdown can offer the categories this window manager
actually shows instead of running a second copy of discovery that would disagree
with the first the moment a `.desktop` file changed.

**When the files change under it, every connected tool is told.** After a
`wm2-ctl reload` the window manager sends a notice to every program connected to
its socket — **except the one that asked for the reload**, which gets its own
reply and nothing else. So each program sees exactly one line per reload of its
own and exactly one notice per reload somebody else asked for, and no program has
to guess which of two identical lines was meant for it. The notice says only that
a reload happened; whatever wants a value asks for it, so the notice can never
become a second, drifting copy of your settings.

**Nothing about your windows crosses the socket.** No window title, no
application class, no window geometry, no window count broken down per window —
the `status` reply carries seven fixed fields about the *window manager* and
nothing about what you have open. This is enforced in the code and asserted by
tests rather than left as an intention.

**Only your own account can connect.** The socket lives in a directory created
mode 0700 and is itself mode 0600, and the window manager checks the connecting
process's credentials against its own user before it reads a single byte. Any
other user, root included, is refused and the connection is closed.

**`SIGHUP` still means exit**, exactly as it always has, and does *not* mean
reload. If you know Unix daemons you will assume otherwise, which is why it is
written down here: reloading is `wm2-ctl reload` and nothing else.

---

## Installing and packaging

**There are two packages, not one.** The build produces two CMake install
components, and a distributor is meant to ship them separately:

| Component | Contains | Depends on GTK |
|---|---|---|
| `wm` | the `wm2-born-again` window manager, the `wm2-ctl` command-line client, the session entry a display manager reads, and this document plus the licence | no |
| `config-gui` | the `wm2-config` settings window and its application entry | yes |

The window-manager package does not depend on GTK, and that is not a promise
made in prose: `bash scripts/gates/install-components.sh` stages both components
into a scratch directory, checks each one's file list is exactly what it should
be and nothing else, and runs `ldd` over every executable the `wm` component
installed, failing if GTK, GDK, GLib or GObject is named. The settings window is
a separate, entirely optional package. A machine that never wants a toolkit
installed can have the window manager and `wm2-ctl` and stop there.

`wm2-ctl` is in the window-manager package on purpose. A droplet reached only
over SSH — no desktop running, no toolkit installed — can still ask a running
window manager what it is doing and change its settings.

**The build option is `BUILD_CONFIG_GUI`, and it has three values:**

| Value | What it does |
|---|---|
| `AUTO` | **the default.** Look for `gtk+-3.0`; build the settings window if it is there, and say so either way. |
| `ON` | Require `gtk+-3.0`. Configuration fails by name if it is missing, rather than quietly producing no settings window. |
| `OFF` | Do not even look. Nothing about the build touches GTK. |

**On a host without the GTK development package, everything except the settings
window still builds, and configuration prints one line saying why:**

```
-- wm2-config: pkg-config cannot find gtk+-3.0 -- the configuration GUI will NOT be built (install libgtk-3-dev to get it; the window manager and wm2-ctl are unaffected)
```

If you expected a settings window and did not get one, that line — printed by the
`cmake` you already ran — is the answer. On Debian and Ubuntu the package to
install is `libgtk-3-dev`.

**Staging each component.** Configure and build once, then install each component
wherever your packaging tool wants it:

```
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --parallel

cmake --install build/release --component wm         --prefix /usr
cmake --install build/release --component config-gui --prefix /usr
```

Use `DESTDIR=/path/to/stage` in the environment if your tool stages into a
directory rather than installing into the prefix directly. Installing the
`config-gui` component from a tree built without the settings window installs
nothing and succeeds, so the same two commands work for a GTK-free build.

Files land under the standard directories: binaries in `bin`, the session entry
in `share/xsessions`, the settings window's application entry in
`share/applications` (which is what puts it under Settings in the root menu's
discovered-application list), and the documentation in
`share/doc/wm2-born-again`.

---

## For developers

`COMPILED_CODE_BEHAVIOR_CHECKLIST.md` at the repository root is the release and
signoff checklist for this project. It is an ongoing developer artifact rather
than a document belonging to any one release: a checkbox in it is not done until
there is an automated test, a recorded manual run, or an explicit accepted
exception with a named reason and owner.

Useful commands:

```
bash scripts/preflight.sh                       # every declared dependency, on this host
bash scripts/gates/build-all.sh                 # Debug, Release and sanitizer trees, full suite
bash scripts/gates/build-all.sh nogtk           # the same suite with the settings window switched off
bash scripts/gates/install-components.sh        # stage both packages; check neither one leaks a toolkit
bash scripts/gates/doc-keys.sh                  # every key this document names, and every key the binary accepts, in both directions
bash scripts/analysis/run-static-analysis.sh    # cppcheck and clang-tidy against the baseline
bash scripts/capture-display-capabilities.sh :2 label

wm2-ctl status                                  # what the running WM is doing
wm2-ctl get frame-thickness                     # the value it is actually using
wm2-ctl set frame-thickness 12                  # applied now, written nowhere
wm2-ctl reload                                  # re-read the config files
```

`./wm2-born-again --help` lists every setting, and every setting it lists is one
the binary will accept — the usage text is generated from the same table the
option parser is handed, so the two cannot drift apart.

**This document cannot drift apart from that table either**, and that is a gate
rather than a promise. `bash scripts/gates/doc-keys.sh` reads the accepted key
set out of the option table, the configuration parser's own key comparisons and
the protocol key constants, reads the keys this document presents out of its
backticked names and its configuration examples, and fails if either set has
something the other does not — naming the key, in whichever direction it went
missing. A key mentioned only inside an HTML comment does not count as
documented, because a reader cannot see one.

### The configuration socket

A running window manager listens on a Unix domain socket, so a configuration
tool can ask it questions and change settings without a restart. `wm2-ctl`
above is the reference client for what follows; this subsection is the wire
detail a second client would need.

The path is published on the root window as the property `_WM2_CONFIG_SOCKET`,
a `STRING` holding the socket's filesystem path:

```
xprop -root _WM2_CONFIG_SOCKET
```

Read the property rather than reconstructing the path. The path itself is
`$XDG_RUNTIME_DIR/wm2-born-again/socket<display>`, falling back to
`/tmp/wm2-born-again-<uid>/socket<display>` when `XDG_RUNTIME_DIR` is not set to
an absolute path, with every character of the display name outside
`[A-Za-z0-9._-]` replaced by `_` — so `:1` becomes `socket_1`. Two window
managers on two displays therefore never contend for one socket.

Only the user running the window manager may connect. The socket lives in a
directory created mode 0700 and is itself mode 0600, and every accepted
connection's peer credentials are checked against the window manager's own uid
before a single byte is read. Any other uid, root included, is closed and the
refusal is logged once per uid. A refused connection never reaches the protocol
at all.

Messages are one JSON object per line, terminated by a newline, and a line
longer than 4096 bytes is refused. The first message on a connection must be a
`hello` naming the client program and the protocol version; anything else closes
the connection. The check runs both ways: `wm2-config` and `wm2-ctl` require the
window manager's acknowledgement to name `wm2-born-again` at their own protocol
version before they send a single setting, so a same-user process squatting the socket
path, or whatever `--socket` points at, is refused by the name it gave itself.
This release answers `hello`, `status`, `get`, `set` and
`reload`. The `status` reply carries the window manager version, the protocol
version, uptime in seconds, the screen width and height, and counts of managed
and hidden windows — and nothing else. No window title, class or geometry is
ever sent over the socket.

A `set` is validated for kind and range before it reaches the parser and is then
applied through the same `Config::applyKeyValue()` the config file goes through.
There is exactly one parser, and the socket uses it: no value can reach the
window manager's state by this route that the file route would have rejected.

If the socket cannot be created the window manager says so on stderr and carries
on managing windows normally; only the configuration connection is lost. In that
case the property is not published, and any stale one a previous window manager
left on the root window is removed — so its presence keeps meaning what it says:
there is a socket at that path, right now.

The same holds when the socket goes away. On a clean exit the window manager
unlinks the socket and deletes the property together, and if the listening
descriptor ever fails while the window manager is running it says so on stderr,
shuts the socket down and deletes the property then. A client that finds no
`_WM2_CONFIG_SOCKET` on the root window has its answer without connecting;
a client holding a path from earlier should re-read the property rather than
assume it is still current.
