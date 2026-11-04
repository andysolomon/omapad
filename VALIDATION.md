# Validation — omapad 0.2.0

Verified on 2026-11-03 on an Intel Mac and two x86_64 Linux machines running Omarchy.

| Target | Build | Core tests | Installed application |
| --- | --- | --- | --- |
| macOS / AppKit | CMake Release | 1,228 checks, zero failures | `/Applications/omapad.app`; ad-hoc signature verified |
| Omarchy machine 1 / GTK3 | GCC, GNUmakefile | 1,234 checks, zero failures | `~/.local/bin/omapad`; resources, icon and desktop entry installed |
| Omarchy machine 2 / GTK3 | GCC, GNUmakefile | 1,234 checks, zero failures | `~/.local/bin/omapad`; resources, icon and desktop entry installed |

The tests exercise the real Scintilla editing widgets, including encodings, file operations,
search, split views, clones, scrolling state, folders, session restore, themes and language lexers.
The totals differ by platform; both suites completed without failures.

Native application checks used isolated support directories and temporary sample files:

- macOS: opening a folder adds its sidebar; the native Clone to Other View menu creates two
  views; typed edits appear in both; quitting backs up changes without modifying the source file.
  The installed copy restores both views, edited text and folder workspace. The window was
  visually inspected. No runtime warnings were logged.
- Both Linux hosts: the installed app opens directly on Wayland (`xwayland: false`), finds its
  installed resources without overrides, and a second command-line launch opens its file in
  the existing process. The clone keyboard shortcut and typed editing work. Quit/relaunch
  preserves both panes, folder workspace and unsaved edits, while the original file is unchanged.
  No runtime warnings were logged.
- The macOS bundle plist, signature and bundled licenses were checked. Both Linux installations
  include the third-party license texts. `PKGBUILD` passes shell syntax validation; an Arch
  package build via makepkg was not separately run.

Fixed during these checks: GTK callback macro parsing, missing explicit integer/string headers
under GCC, Mac folder-open events incorrectly reaching the file reader, Linux executable name
following stack-size re-exec, and the desktop entry's window-class identifier. GNUmakefile now
tracks header dependencies.

The core tests cover theme loading and reload behavior. Live compositor-wide theme changes,
all menus, file/tab drag-and-drop, printing (not implemented), and Apple Silicon hardware were
not exercised. Linux window screenshot capture was unavailable in the current remote display
state; Linux interaction and persistence checks used the running Wayland applications.
