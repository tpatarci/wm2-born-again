# wm2-born-again

A minimal X11 window manager with sideways tabs, for a VPS you reach over VNC or
RDP. It is a modernised resurrection of **wm2**, written by **Chris Cannam** in
1997.

![licence MIT](https://img.shields.io/badge/licence-MIT-blue) ![C++17](https://img.shields.io/badge/C%2B%2B-17-blue) ![tests 615](https://img.shields.io/badge/tests-615-brightgreen)

## Why this exists

wm2 was absolute greatness, in my honest opinion, and I am not worthy of it. It
did exactly what it set out to do, it did it in a few thousand lines, and it had
a look nothing else has had since.

This is my puny attempt, working with an AI assistant, to refresh wm2 so that it
is a bit more usable in this day and age: it builds with a current compiler
against current X libraries, it speaks enough of the modern window-manager
conventions that today's applications behave, it can be configured without
recompiling it, and it is aimed squarely at the case I actually have — a small
virtual server with a remote desktop on it.

Everything you like about how it looks and feels is Chris Cannam's design. The
parts that are new are mine, and so are their faults.

## What it is, and what it is not

It keeps wm2's idea of a window manager. Frames with a tab down the left-hand
side, the title drawn sideways on that tab, one small button at the top of the
tab, a resize corner at the bottom right. No icons, no taskbar, no pager, no
keyboard bindings, no virtual desktops. Click the desktop background and the
menu appears; that is where new windows and the application list live.

What the modernisation added:

- **A configuration file** and live changes, so nothing needs a recompile:
  colours, both fonts, frame thickness, focus policy, delays, the new-window
  command, and your own menu entries.
- **`wm2-ctl`**, a small command-line client that reads and writes those settings
  on a running desktop. It links no X libraries and no toolkit.
- **`wm2-config`**, an optional GTK 3 settings window, for the same settings with
  colour and font pickers. Skip it and the window manager loses nothing.
- **An application menu** built from the `.desktop` files installed on the
  machine, grouped into the categories they declare.
- **Modern window-manager conventions** (EWMH) where they matter, so
  applications that ask to be fullscreen, maximised, kept above others, or
  treated as a dock get what they asked for.
- **Xft and fontconfig fonts**, including the sideways label, in place of the
  original's bitmap fonts and bundled rotation code.
- **Behaviour on servers that are missing pieces**: it checks for the Shape,
  RANDR and RENDER extensions and degrades along a stated path rather than
  failing, because remote-desktop servers differ in what they offer.

What it is deliberately not: **single-screen**. It treats the X screen as one
rectangle however many monitors are behind it. On a VPS with one virtual display
that is the right model; on a two-monitor desktop, maximise will fill both. See
[Limitations](docs/RELEASE-NOTES.md#limitations) in the release notes, which is
the first section there on purpose.

## Requirements

A modern Linux with X11. Ubuntu 22.04 and 24.04 are what it is built and tested
on.

```
cmake (3.20+)  g++ (C++17)  pkg-config
libx11  libxext  libxft  libfontconfig  libxrandr
libgtk-3  (optional — only for the settings window)
```

On Debian or Ubuntu:

```sh
sudo apt install cmake g++ pkg-config libx11-dev libxext-dev libxft-dev \
                 libfontconfig-dev libxrandr-dev libgtk-3-dev
```

## Build and install

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --parallel
sudo cmake --install build/release --component wm         --prefix /usr
sudo cmake --install build/release --component config-gui --prefix /usr
```

The `wm2` component installs the window manager and `wm2-ctl` and needs no GTK.
The `config-gui` component installs the settings window and does; leave it out on
a server without GTK. `DESTDIR=` works if your packaging tool stages files.

## Running it

It is a window manager, so something has to start it as the session. Over VNC
that is the server's startup script — for TigerVNC, `~/.vnc/xstartup`:

```sh
#!/bin/sh
unset SESSION_MANAGER
unset DBUS_SESSION_BUS_ADDRESS
eval $(dbus-launch --sh-syntax)
exec wm2-born-again
```

Start nothing else there. A second window manager will simply refuse to run, and
a desktop-drawing program will cover the root window that the menu lives on.

For a login manager instead, the installed `/usr/share/xsessions/wm2-born-again.desktop`
offers it as a session to pick.

## Configuring it

Three routes to the same settings, and all of them agree, because they go
through one validator:

```sh
$EDITOR ~/.config/wm2-born-again/config   # the file; takes effect on reload
wm2-ctl set frame-thickness 9             # a running desktop, immediately
wm2-ctl get menu-entries
wm2-ctl status
wm2-config                                # the settings window
```

Every key, its range and its default are documented in
[docs/RELEASE-NOTES.md](docs/RELEASE-NOTES.md), and a build gate fails if that
document and the binary ever disagree about them.

## Credits

**Chris Cannam** wrote wm2 in 1997 and everything that makes it worth reviving.
His original source is kept in this repository under `upstream-wm2/` for
reference, unmodified.

wm2 itself stood on other people's work, and those credits belong here too:

- **David Hogan**, whose *9wm* wm2 began as a derivative of.
- **Andy Green**, whose idea the sideways tabs were.
- **Alan Richardson**, whose *xvertext* 2.0 rotated wm2's tab labels. This
  version no longer uses it — the sideways text is drawn by Xft with a rotation
  matrix — so the credit is historical rather than a dependency.

The modernisation was done with an AI assistant, deliberately and openly: this
repository doubles as a record of what that kind of collaboration produces,
including the review records, the security audit, and the list of things the
tests do *not* cover, under `.planning/`.

## Licence

MIT. See [LICENSE](LICENSE), and [NOTICE](NOTICE) for the derivation and the
upstream terms.

The original wm2 carried no formal licence text. Chris Cannam's terms, in his own
README, were: *"If you want to hack the code into something else for your own
amusement, please go ahead. Feel free to modify and redistribute, as long as you
retain the original copyrights as appropriate."* Those copyrights are retained in
`LICENSE`, and MIT keeps the same requirement.
