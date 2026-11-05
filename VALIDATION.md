# Validation — omapad 1.1.0

Verified on 2026-11-04 on an Intel Mac and two x86_64 Linux machines running Omarchy.

| Target | Build | Core tests |
| --- | --- | --- |
| macOS / AppKit | CMake Release | 3,777 checks, zero failures |
| Omarchy machine 1 / GTK3 | GCC, GNUmakefile | 3,867 checks, zero failures |
| Omarchy machine 2 / GTK3 | GCC, GNUmakefile | 3,867 checks, zero failures |

The tests exercise the real Scintilla editing widgets. `testInstallerAuditGaps` covers what the audit below found missing.

## 1.1.0: audit of the real installer

The installer `npp.8.9.8.1.Installer.x64.exe` has SHA-256 `26f3bcced788fadbac4e7e577b430ea2ce6060cf3bcad993ef4612ecd7d743da`, matching the official checksum list. It was read without running anything in it, and compared with the Notepad++ source for the same tag.

- Its `notepad++.exe` and plugin DLLs are byte-identical to the portable build's.
- Themes, `langs.model.xml` and `stylers.model.xml` match omapad's byte for byte. Other data files differ only in line endings, except our disclosed change to `functionList/rust.xml`. `overrideMap.xml` is left out on purpose.
- Every preference in the table is now read by code outside the table. That is a check that nothing is left unused, not a test of each one's effect.
- `Highlight comment/php/asp zone`: the 8.9.8.1 source stores this preference and never reads it. omapad's does what the label says, so here it goes beyond Notepad++.

Linux, through GTK's Broadway backend and a headless browser: ghost typing, `-titleAdd`, `-monitoringMode`, `--help`, read-only and monitoring marks, alternate icons, pin button, buttons on the active tab only, the new, list and close buttons, a dark tone, toolbar colour, border width, Document List and Character Panel columns, Shortcut Mapper categories, the `<<` button and the Style Configurator's font columns.

macOS, drawn offscreen without showing the window or taking focus: `-titleAdd`, filled and coloured toolbar, pin button, buttons on the active tab only, the new, list and close buttons, border width, and both panels' columns.

**Not checked in 1.1.0 beyond the core tests, or not at all:**

- The Mac tray icon, Edit with omapad in the Services menu, and File Association on either platform, which would change this machine's real settings.
- Saving with administrator rights on either platform, which needs a real password prompt.
- `doLocalConf.xml` and `asNotepad.xml` in a real install. `disableNppAutoUpdate.xml` is covered by a test.
- Custom toolbar icons drawn in a real window, and `.ico` files on a Mac.
- Peek on the Document Map, which has no test.
- Shift+Enter in the Find window, in a real window.
- Whether the new default keys clash with Hyprland's bindings on a real session.
- Anything on a live Wayland session, or by hand in a live Mac window.

## 1.0.0 results

Linux, through GTK's Broadway backend and a headless browser, with isolated support directories: the toolbar, Document List, Character Panel, Project Panel, tab colour and pin, Incremental Search, Mark tab, Windows dialog, Post-It and Style Configurator chooser, plus `-n3` on the command line.

macOS: the real window built offscreen and drawn to images, without showing it or taking focus: toolbar, side panels, tab colours and pins, vertical and multi-line tabs, reduced tabs, Incremental Search bar and status bar.

**Not checked in 1.0.0:**

- Any of the new Mac commands with real clicks in a live window. The window has never been used by hand.
- Any of the new windows on a live Wayland session.
- Printing on paper, Print Now, and Check for Updates against a real release.
- Most commands reached only through their menus in the real windows. The core tests run them directly.
- Installing a plugin through the Import dialog in a real window. The tests load the example plugin from the plugins folder.
- Right-to-left text on either platform.

## 0.4.0 results

Linux, through GTK's Broadway backend and a headless browser, with isolated support directories:

- Save as PDF from the menu: 12 A4 pages with the header, footer, line numbers and word wrap, syntax
  colours on white, and 11 pt text measured in the PDF. The print dialog opens and Cancel returns
  to a working editor.
- Language editor: all six pages, a new language saved with a keyword and a picked colour, the file
  written correctly, and the open tab recoloured. Bundled Markdown highlights a `.md` file.

macOS:

- Save as PDF from the menu in the real app, before the print view moved to a hidden editor: 13
  Letter pages, the right way up. The final code then wrote PDFs through the same print code from
  the test suite, including one from a dark theme, which has a white background and dark text.
- The language editor was rendered to images offscreen with its real controls. The pages I viewed
  (Keywords, Comments and Numbers, Styles, Nesting) look right, and pressing OK through those controls
  keeps a language byte-for-byte. That check found and fixed a crash on pages starting with a heading.

Fixed in the bundled lexer during these checks: fold levels landed on the wrong line after a comment
that closes at the end of a line. Details are in `NOTICE`.

**Not checked in 0.4.0:**

- Printing on paper. A real printer was available on Linux and wasn't used. **Print Now** and printing
  from the dialog weren't run on either platform.
- The final Mac build's language editor, print dialog and menu commands in a live window with real
  clicks, because the screen was locked and then in use.
- Importing, exporting and removing languages in the real windows, on either platform. These commands
  are covered by the core tests.
- Clicking a colour well on macOS.
- Any of the new windows on a live Wayland session.

0.4.0 hasn't been installed on any of these machines.

## 0.3.0 results

Added in 0.3.0, checked by driving the real windows with isolated support directories:

- macOS: Function List, Document Map click, completion list and Return to accept, Preferences,
  and Shortcut Mapper with a captured key saved to settings and shown in the menu.
- Linux, through GTK's Broadway backend and a headless browser: the same panels, function hints,
  Function List filter and double-click, Document Map click, drag and wheel, Column Editor form,
  Shortcut Mapper capture, and a rebound Undo key. The installed binary found its resources.

**Not checked in 0.3.0:** these windows on a live Wayland session. One Linux machine's session
was locked and the other was in use, so Broadway stood in for Wayland. That leaves the placement
of the completion list and hint popups on Hyprland unconfirmed. Word completion's list was seen
on macOS only; on Linux it's covered by the core tests. Also not exercised: Apple Silicon hardware,
live compositor-wide theme changes, and every menu command.

## 0.2.0 results

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
all menus, file/tab drag-and-drop, and Apple Silicon hardware were not exercised. Printing isn't implemented. Linux window screenshot capture was unavailable in the current remote display
state; Linux interaction and persistence checks used the running Wayland applications.
