# Validation — omapad 0.4.0

Verified on 2026-11-04 on an Intel Mac and two x86_64 Linux machines running Omarchy.

| Target | Build | Core tests |
| --- | --- | --- |
| macOS / AppKit | CMake Release | 2,272 checks, zero failures |
| Omarchy machine 1 / GTK3 | GCC, GNUmakefile | 2,278 checks, zero failures |
| Omarchy machine 2 / GTK3 | GCC, GNUmakefile | 2,278 checks, zero failures |

The tests exercise the real Scintilla editing widgets, including encodings, file operations,
search and PCRE2 regex, split views, clones, folders, session restore, themes, language lexers,
preferences, shortcuts, completion, hints, all 47 Function List rule files, multi-select, the
Column Editor, change history, token styles, the Document Map, user-defined language files and
colouring, and print pagination. On macOS they also write a real PDF through the app's print code.
The totals differ by platform; all suites completed without failures.

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
