# omapad

A native text editor for macOS and Linux, modelled on Notepad++. The Mac interface uses
AppKit; the Linux interface uses GTK3 and runs natively on Wayland, including Omarchy Quattro.
Both share the same C++ document, search, split-view, folder-workspace and session code.

omapad is an independent project, formerly called Folio. It is not affiliated with or endorsed
by Notepad++. It uses Scintilla 5.6.6, Lexilla 5.5.3, and the language data and themes bundled
with Notepad++ 8.9.8.1. This is version 0.2.0 of our own editor, not a Notepad++ release.

## Build and install

### macOS

Requires macOS 12 or later, Xcode command line tools and CMake.

```sh
cmake -S . -B build
cmake --build build -j8
ctest --test-dir build --output-on-failure
codesign --force --deep --sign - build/omapad.app
ditto build/omapad.app /Applications/omapad.app
open /Applications/omapad.app
```

The build targets the current Mac's architecture. For a universal build, configure with
`-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`. The local build is ad-hoc signed, not notarized.

### Omarchy / Arch Linux

Requires `gcc`, `make`, `pkgconf` and `gtk3`. These were already available on the tested machines.

```sh
make -j8
make test
make install
omapad
```

Tests use real GTK editor widgets and need an active graphical session. From an SSH session,
set `XDG_RUNTIME_DIR` and `WAYLAND_DISPLAY` to the logged-in desktop's values. If Xvfb is
available, `xvfb-run -a make test` also works.

Installation defaults to `~/.local`: the executable is `~/.local/bin/omapad`, with resources,
licenses, icon and an application-menu entry under `~/.local/share`. `make uninstall` removes
those installed files and preserves preferences and sessions. `PREFIX` and `DESTDIR` are
supported. A `PKGBUILD` is included for `makepkg -si` from this source directory; its check
step also requires a graphical session.

Open files or add folders from the command line:

```sh
omapad notes.txt ~/my-project
```

Linux launches use a single application instance: subsequent launches open files in the existing
window. On macOS, use `open -a omapad notes.txt` or `open -a omapad ~/my-project`.

## Split view and Folder as Workspace

- **View → Split View → Move to Other View** moves a tab into the second pane.
  **Clone to Other View** shows the same document in both panes; edits and undo are shared.
  These commands are also available by right-clicking a tab.
- **F8** focuses the other pane. **Stack Views** changes from side-by-side to top-and-bottom.
  Vertical and horizontal scrolling can be synchronized independently.
- **File → Open Folder as Workspace** adds a folder to the sidebar. Multiple roots are supported.
  Expand folders and double-click a file to open it. Right-click for new file/folder, rename,
  move to trash, reveal, copy path, or Find in Files. Use Refresh after external folder changes.
- Sessions restore the panes, shared clones, active tabs, folder roots, expanded branches and
  unsaved text. Backup runs every seven seconds and on normal quit. Closing an edited document
  explicitly still asks whether to save it.

## Included

| Area | Features |
| --- | --- |
| Documents | Tabs, recent files, reload, Save All, Save a Copy, file drag and drop |
| Editing | Line operations, sorting, case conversion, comments, column/multiple selection, auto-indent |
| Search | Find, Replace, Count, Mark All, Find All, Find in Files, replace across open documents; normal, extended and regex modes |
| View | Split panes, wrap, whitespace symbols, indent guides, zoom, folding, bookmarks, brace matching |
| Languages | Notepad++ language list and syntax highlighting; detection by filename, extension or first line |
| Encoding | UTF-8, UTF-16, Windows-1252, Latin-1 and others; convert or reopen with an encoding |
| Themes | 22 bundled Notepad++ themes and automatic appearance; Linux follows Omarchy's palette live |
| Tools | MD5, SHA-1, SHA-256 and SHA-512 hashes; in-memory macro recording and playback |

With **Settings → Theme → Automatic (Follow Omarchy)**, Linux reads Omarchy's
`~/.local/state/omarchy/current/theme/colors.toml`, falling back to the older
`~/.config/omarchy/current/theme/colors.toml` location. Theme changes update the editor and
window colors. An explicitly selected theme overrides automatic following.

## Shortcuts

`Mod` in the shared menus means Command on macOS and Control on Linux.

| Action | macOS | Linux |
| --- | --- | --- |
| Find next / previous | Cmd+G / Cmd+Shift+G | F3 / Shift+F3 |
| Replace | Cmd+Option+F | Ctrl+H |
| Open folder | Cmd+Option+O | Ctrl+Alt+O |
| Move to other view | Cmd+Option+Right | Ctrl+Alt+Right |
| Clone to other view | Cmd+Option+Shift+Right | Ctrl+Alt+Shift+Right |
| Focus other view | F8 | F8 |

Omarchy's Super+C / Super+V mappings to Ctrl+Insert / Shift+Insert work with Scintilla's
standard editing bindings. A compositor shortcut can take precedence over an app shortcut;
the menu commands remain available.

## State and limitations

macOS stores settings, sessions and backups in `~/Library/Application Support/omapad`.
Linux uses `$XDG_CONFIG_HOME/omapad` (default `~/.config/omapad`) for settings and
`$XDG_STATE_HOME/omapad` (default `~/.local/state/omapad`) for sessions and backups.
The previous Folio application's data is separate and is not automatically migrated.

- Regex searches are line-oriented ECMAScript, rather than Notepad++'s Boost syntax.
  Expensive expressions can be slow. Find in Files skips regex lines over one million code points.
- No printing, plugins, user-defined languages, auto-completion, Function List, Document Map,
  saved macros, Shortcut Mapper, full Preferences window, or tail monitoring yet.
- Folder refresh is manual; it does not continuously watch the filesystem.
- Mac tab strips are custom drawn and do not expose individual tabs to VoiceOver yet.
  The Window menu and Cmd+1 through Cmd+9 provide keyboard access.
- Wayland does not support the app's Always on Top request. Use the compositor's float/pin controls.

For isolated development checks, `OMAPAD_SUPPORT_DIR` redirects settings and sessions to a test
folder. Linux also supports `OMAPAD_RESOURCES` for resource lookup and `OMAPAD_NON_UNIQUE=1` to
launch an independent test instance. `OMAPAD_OMARCHY_THEME` selects a test colors.toml file.

## Provenance and license

The original feature inventory came from REA inspection of the official
`npp.8.9.8.1.portable.x64.zip`, verified against its published SHA-256 and GPG signature:
file inventory, PE menus/dialogs and `localization/english.xml`. Ghidra timed out importing the
executable; no decompiled code was used. Notepad++ source tag `v8.9.8.1` (commit `dd40fe45`)
supplied the bundled editing engine and language/keyword mapping.

omapad's application code is new. The project is GPLv3. See [NOTICE](NOTICE), [LICENSE](LICENSE)
and [licenses](licenses) for bundled component attribution and license texts.
