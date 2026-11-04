# omapad

A native text editor for macOS and Linux, modelled on Notepad++. The Mac interface uses
AppKit; the Linux interface uses GTK3 and runs natively on Wayland, including Omarchy Quattro.
Both share the same C++ document, search, split-view, folder-workspace and session code.

omapad is an independent project, formerly called Folio. It is not affiliated with or endorsed
by Notepad++. It uses Scintilla 5.6.6, Lexilla 5.5.3, and the language data and themes bundled
with Notepad++ 8.9.8.1. This is version 0.3.0 of our own editor, not a Notepad++ release.

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
  unsaved text. Backup runs every seven seconds by default and on normal quit, and the interval
  can be changed in Preferences. Closing an edited document explicitly still asks whether to save it.

## Writing code

- **Auto-completion** offers the language's functions and keywords and the words in the document
  as you type, using Notepad++'s API files for 34 languages. **Function hints** show a call's
  parameters when you open it, and highlight the one you're on. **Path Completion** completes
  file names from disk. All of this is under **Edit → Auto-Completion** and in Preferences.
- **View → Function List** shows a document's classes and functions, using Notepad++'s parser
  rules for 47 languages. You can filter it or sort it by name, and double-click to go to one.
- **View → Document Map** shows the whole document in miniature. Click or drag on it to move there.
- **Edit → Multi-select All / Next** add other copies of the selection, so one edit changes them
  all. **Column Editor** inserts text or a numbered sequence down a column.
- **Change history** marks edited lines in the margin. **Search → Change History** goes between them.
- **Search → Style All Occurrences of Token** and **Style One Token** mark text in five colours
  that you can jump between, copy or clear.
- **Settings → Preferences** and **Settings → Shortcut Mapper** cover editor settings and let you
  change any menu command's shortcut.

## Included

| Area | Features |
| --- | --- |
| Documents | Tabs, recent files, reload, rename, read-only mode, Save All, Save a Copy, close to the left or right, file drag and drop |
| Editing | Line operations, sorting, case conversion, comments, column and multiple selection, multi-select commands, Column Editor, auto-indent, auto-completion, function hints |
| Search | Find, Replace, Count, Mark All, Find All, Find in Files, replace across open documents, next and previous result; normal, extended and PCRE2 regex modes |
| View | Split panes, Function List, Document Map, change history, five-colour token styles, wrap, whitespace symbols, indent guides, zoom, folding, bookmarks, brace matching |
| Settings | Preferences window, Shortcut Mapper, column guide, default line ending and encoding |
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
| Preferences | Cmd+, | Ctrl+, |
| Function completion | Ctrl+Space | Ctrl+Space |
| Word completion | Ctrl+Shift+Space | Ctrl+Shift+Space |
| Function hint | Ctrl+Option+Space | Ctrl+Alt+Space |
| Column Editor | Cmd+Option+C | Alt+C |

**Settings → Shortcut Mapper** changes any of these. A key that's already in use asks before moving.

Omarchy's Super+C / Super+V mappings to Ctrl+Insert / Shift+Insert work with Scintilla's
standard editing bindings. A compositor shortcut can take precedence over an app shortcut;
the menu commands remain available.

## State and limitations

macOS stores settings, sessions and backups in `~/Library/Application Support/omapad`.
Linux uses `$XDG_CONFIG_HOME/omapad` (default `~/.config/omapad`) for settings and
`$XDG_STATE_HOME/omapad` (default `~/.local/state/omapad`) for sessions and backups.
The previous Folio application's data is separate and is not automatically migrated.

- Regular expressions use PCRE2, not Notepad++'s Boost engine. The syntax is very close, and
  multi-line patterns, lookbehind and `\K` work, but a few Boost-only constructs don't. A search
  that hits PCRE2's step limit stops and says so.
- On Linux, changing a standard editing shortcut such as Undo adds the new key, but the editor's
  built-in key keeps working in the editor.

### Not in omapad yet

The full Notepad++ menu has more than this. These are the commands and features still missing:

| Area | Missing |
| --- | --- |
| Files and tabs | Printing, Load and Save Session, pinned tabs, tab colours, sorting tabs, the Windows dialog, Document List, Open in a new instance, read-only for all documents |
| Editing | Begin/End Select, Paste HTML, RTF or Binary content, Character Panel, Clipboard History, Hide Lines, right-to-left text, customized date and time, locale-order sorting, Redact Selection, Search on Internet |
| Search | Volatile Find, Incremental Search, Find Characters in Range, Cut and Paste to Bookmarked Lines |
| View | Project Panels, Post-It, Distraction Free mode, monitoring (`tail -f`), browser preview, control character display |
| Languages | User-defined languages |
| Encodings | About half of Notepad++'s character sets, including Arabic, Greek, Hebrew, Thai, Turkish and the OEM code pages |
| Settings | Style Configurator, theme import, context menu editing, many of Notepad++'s individual preferences |
| Macros and tools | Saved macros, Run a Macro Multiple Times, Run menu, Generate-to-file hash dialogs |
| Plugins | Notepad++ plugins are Windows DLLs and can't load here. omapad has no plugin system |

Windows-only commands such as Open in Explorer or `cmd`, Edge and Internet Explorer preview, and
the updater aren't planned.

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
supplied the bundled editing engine, language and keyword mapping, auto-completion API files and
Function List parser rules.

omapad's application code is new. The project is GPLv3. See [NOTICE](NOTICE), [LICENSE](LICENSE)
and [licenses](licenses) for bundled component attribution and license texts.
