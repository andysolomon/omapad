# omapad

A native text editor for macOS and Linux, modelled on Notepad++. The Mac interface uses
AppKit; the Linux interface uses GTK3 and runs natively on Wayland, including Omarchy Quattro.
Both share the same C++ document, search, split-view, folder-workspace and session code.

omapad is an independent project, formerly called Folio. It is not affiliated with or endorsed
by Notepad++. It uses Scintilla 5.6.6, Lexilla 5.5.3, and the language data and themes bundled
with Notepad++ 8.9.8.1. This is version 1.1.0 of our own editor, not a Notepad++ release.

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

## Printing

**File → Print** (Mod+P) uses the system's print dialog. **Print Now** skips the dialog,
**Print Selection** prints only what's selected, and **Save as PDF** writes a PDF directly.

The printout has the document's syntax colours on white. If you're using a dark theme, it switches
to the default light theme for the printout, so the text stays readable on paper. The editor
itself doesn't change. Search and smart highlights don't print; token styles do.

**Settings → Preferences** sets line numbers, colours, margins, and the header and footer. Headers
and footers can use `$(FILE_NAME)`, `$(FULL_CURRENT_PATH)`, `$(SHORT_DATE)`, `$(LONG_DATE)`,
`$(TIME)`, `$(CURRENT_PRINTING_PAGE)` and `$(TOTAL_PAGES)`.

## User-defined languages

**Language → User Defined Language → Define Your Language** creates or changes a language: its
file extensions, keywords, comments, numbers, operators, delimiters, folding, colours and nesting.
**Import** and **Export** read and write Notepad++'s `.udl.xml` files, including ones from before
UDL 2.0. Exports are written as UDL 2.1.

Languages are stored in the `userDefineLangs` folder next to your settings. Like Notepad++, a user
language takes priority over a built-in one for the same extension. It also supplies the markers for
the comment commands, and an extension field can name a whole file, like `Fresh.conf`.

omapad comes with Notepad++'s Markdown language, so `.md` and `.markdown` files are highlighted,
using its dark variant on dark themes. Defining a language with the same name overrides it, and
removing yours brings the original back.

## Everything else

- **Plugins** are native libraries in the `plugins` folder next to your settings. See `plugins/example/example.c` for a complete one. A plugin runs with all of your permissions, so only install ones you trust.
- **Side panels:** Document List, Character Panel, Clipboard History, and three Project Panels that read and write Notepad++ workspace files.
- **Find:** Mark tab, Replace in Files, Find in Projects, Find All in All Opened Documents, Incremental Search, Find Characters in Range, and searching only in found lines.
- **Window modes:** toolbar, Post-It, Distraction Free, vertical and multi-line tabs, tab colours and pinned tabs.
- **Style Configurator** edits any theme and keeps your copy in the `themes` folder next to your settings. **Import Style Theme** takes Notepad++ theme files.
- **Sessions:** Load Session reads omapad's and Notepad++'s session files.
- **Macros and Run:** named macros, Run a Macro Multiple Times, and saved Run commands with Notepad++'s variables.
- **Encodings:** all of Notepad++'s character sets.
- **Command line:** Notepad++'s switches, including `-L`, `-x`, `-y`, `-r` with wildcards, `-monitoringMode`, `-fullReadOnly`, `-fullReadOnlySavingForbidden`, `-settingsDir=`, `-titleAdd=`, `-pluginMessage=`, `-noPlugin` and `-qn=`, `-qt=`, `-qf=` ghost typing. `omapad --help` lists them all.
- **Encodings on open:** the bundled uchardet detects legacy encodings, and HTML and XML files can declare their own.
- **Configuration files:** `tabContextMenu.xml` and `toolbarButtonsConf.xml` in your settings folder work as they do in Notepad++, and examples are installed with the program. Next to the program, `doLocalConf.xml` makes a portable copy, `asNotepad.xml` turns off tabs and sessions, `noEasterEggs.xml` turns off the quotes, and `disableNppAutoUpdate.xml` hides the update command.
- **File association:** Settings → File Association makes omapad the default for the extensions you list. **Edit with omapad** is in the Services menu on a Mac, and the desktop entry on Linux adds a new-window action.
- **Context menu:** Settings → Edit Popup ContextMenu opens the file that defines it.

## Included

| Area | Features |
| --- | --- |
| Documents | Tabs, recent files, reload, rename, read-only mode, Save All, Save a Copy, close to the left or right, file drag and drop, printing, Save as PDF |
| Editing | Line operations, sorting, case conversion, comments, column and multiple selection, multi-select commands, Column Editor, auto-indent, auto-completion, function hints |
| Search | Find, Replace, Count, Mark All, Find All, Find in Files, replace across open documents, next and previous result; normal, extended and PCRE2 regex modes |
| View | Split panes, Function List, Document Map, change history, five-colour token styles, wrap, whitespace symbols, indent guides, zoom, folding, bookmarks, brace matching |
| Settings | Preferences window, Shortcut Mapper, column guide, default line ending and encoding |
| Languages | Notepad++ language list and syntax highlighting; detection by filename, extension or first line; user-defined languages with Notepad++ file import and export |
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
| Print | Cmd+P | Ctrl+P |
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
- The language editor is an ordinary dialog, so changes appear when you press OK. Notepad++'s stays
  open and updates the document as you edit.
- User languages get word completion but not function completion, hints or a Function List, which
  Notepad++ can add with extra files.

### What's different from Notepad++

This is what we know differs, from comparing omapad with the 8.9.8.1 installer and its source. It doesn't prove nothing else does: every platform draws these windows differently, and much of it hasn't been used by hand. See [VALIDATION.md](VALIDATION.md).

| Area | Difference |
| --- | --- |
| Plugins | Notepad++ plugins are Windows DLLs and can't load here. omapad has its own plugin interface, `core/OmapadPlugin.h`, with an example in `plugins/example`. MIME Tools, Converter and NppExport are built in. Plugins Admin manages what's installed, but there's no online catalogue |
| Windows-only commands | Explorer, `cmd` and PowerShell became your file manager and Terminal, Edge opens if it's installed, and Safari is added on a Mac. Internet Explorer has no equivalent |
| Updates | Check for Updates compares with the latest GitHub release and opens its page. It doesn't install anything |
| Menu languages | The menus can use any of Notepad++'s 94 translations, for titles that match Notepad++'s English. Dialogs stay in English |
| Right-to-left text | Available on a Mac only. The Linux editing widget has no right-to-left drawing, and omapad says so if you choose the command |
| Instances | On a Mac, Finder always opens files in the running copy. **Open in New Instance** and `-multiInst` start another |
| Menu bar | Hide Menu has no effect on a Mac, where the menu bar belongs to the system |
| Default keys on Linux | Follow Notepad++. Ctrl+1 to 5 jump between styled text, so tabs are Ctrl+Numpad 1 to 9. A Mac keeps its own conventions where they clash, and Notepad++'s keys on a Mac aren't all adopted |
| System tray | Mac only. GTK3 has no tray on Wayland without another library |
| Window position | `-x` and `-y` work on a Mac and under X11. Wayland doesn't let a program place its own window |
| Dark mode tones | Linux only. The Mac window follows the system's appearance |
| Toolbar icons | On Linux, line icons are the theme's symbolic set and filled ones its full-colour set, and a colour forces line icons. On a Mac, filled uses each symbol's `.fill` form where there is one |
| Run dialog | History and browsing are controls in the form, not a combo box with a browse button |
| Network paths | No UNC path warning or server list, as there are no UNC paths |
| Preferences not offered | Windows rendering modes |

The dialogs work slightly differently: the language editor and Style Configurator apply changes when you press OK, rather than live.

## Provenance and license

The original feature inventory came from REA inspection of the official
`npp.8.9.8.1.portable.x64.zip`, verified against its published SHA-256 and GPG signature:
file inventory, PE menus/dialogs and `localization/english.xml`. Ghidra timed out importing the
executable; no decompiled code was used. Notepad++ source tag `v8.9.8.1` (commit `dd40fe45`)
supplied the bundled editing engine, language and keyword mapping, auto-completion API files and
Function List parser rules.

omapad's application code is new. The project is GPLv3. See [NOTICE](NOTICE), [LICENSE](LICENSE)
and [licenses](licenses) for bundled component attribution and license texts.
