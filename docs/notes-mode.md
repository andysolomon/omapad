# Notes mode

Choose **Notes → Notes Mode** (Command-Option-N on Mac, Ctrl-Alt-N on Linux).
The Editor button returns to your previous editor document. Existing code tabs and
unsaved code stay open; writing preferences do not change your coding preferences.

## Capture and find

- **New Note**, or Command/Ctrl-N in Notes mode, starts a blank Markdown note without
  a filename dialog. The first meaningful line supplies its title; changing the title
  does not rename its file. New Note switches from Read to Write so typing works immediately.
- Notes live in `~/Documents/omapad Notes` by default. **Choose Notes Folder** selects
  another library, including an existing Mac Markdown folder. This release indexes
  direct `.md` and `.markdown` children, excluding symbolic links and hidden files.
- The library sorts by modification time. Search matches every space-separated term
  anywhere in a note, ignoring case. Tags come from body text such as `#personal` or
  `#work/ideas`; selecting `work` includes its child tags. Search and tags combine.
- **Import**, or Command/Ctrl-O in Notes mode, copies UTF-8 text into a new note. It
  preserves the source file and saves the previous note before switching.
- **Move Note to Trash** asks for confirmation and uses the system Trash.

## Write and read

**Write**, **Split**, and **Read** choose the source editor, source plus preview, or
preview alone. The sidebar button toggles focused writing. Formatting controls add
headings, bold, italic, bullets, tasks, quotes, inline code, and links; each operation
can be undone. The toolbar displays save status and a word count.

Preview uses native text widgets on AppKit and GTK. It supports basic Markdown
headings, emphasis, lists, tasks, quotes, code fences and HTTP(S) links. Raw HTML stays
literal text; scripts and remote images are not executed or loaded. It is not a full
CommonMark engine: tables, mathematical notation, Mermaid, embedded images and
complex nested Markdown are not rendered. **Export HTML** creates a standalone,
escaped HTML document. Existing editor print/PDF commands remain available for the
source document.

## Saving and recovery

Notes save after approximately one to two seconds of idle editing. Switching notes,
changing libraries, leaving Notes mode, and normal quitting flush pending changes.
Files use stable generated names and remain ordinary Markdown that other tools can
read. Autosave uses a temporary file, flushes it, then renames it into place.

Before saving, omapad compares the disk contents with the version originally opened.
If another app changed or removed the file, saving stops and the editor retains your
text. Use **File → Save a Copy As** to preserve your version, then **File → Reload from
Disk** to accept the external version, or restore the expected disk contents and use
**Retry Save**. Failed saves block a mode/library change and normal quit. Session
snapshots retain dirty text and the disk baseline across restart when session backup
is enabled. A read-only or unavailable folder never reports a successful save.

The library and view choice persist across launches. Notes support UTF-8 files up to
16 MiB; interactive preview is capped at 1 MiB. Scans stop at 10,000 notes or 64 MiB
of indexed content. There is no account, database, built-in sync or recursive vault.
A folder managed by an external sync tool remains that tool's responsibility.

## Reference and evidence

Workflow inspiration: [andysolomon/mac-markdown-workspace](https://github.com/andysolomon/mac-markdown-workspace),
MIT, source revision `437b6b8d9baa6a902a92da8d40f42b73fd5f3912`. This is a new native
implementation, not an embedded Electron application or a copied renderer.

Source inspection traced:

| Behavior | Reference source |
| --- | --- |
| Derived titles, snippets, tags and matching | `src/services/notesModel.ts` |
| File library and stable identities | `src/services/notesFileStore.ts`, `notesStore.ts` |
| Note switching, search, tags and autosave orchestration | `src/components/shell/NotesShell.tsx` |
| Writing presentation and preview modes | `src/components/SourceEditor.tsx`, `Workspace.tsx`, `Preview.tsx` |
| Markdown formatting | `src/services/editorBridge.ts` |
| Import/export and shortcuts | `src/hooks/useFileOperations.ts`, `useKeyboardShortcuts.ts` |

REA statically analyzed the published v1.0.0 Linux release's `app.asar`, including
`.vite/build/main-yfjWduVU.js` and `.vite/build/preload.js`. It identified 27 bridge
members, 25 main IPC handlers and 25 paired renderer transmissions. This supports
the storage/IPC architecture mapping; the UI behavior above is grounded in source
inspection, not a claim of observing the reference app running.

- Release ZIP SHA-256: `76b9348a45335615988ecf96bc25741342cd36c69c3ad07462ddfad93c1c604c`
- `app.asar` SHA-256: `61f548da1fc1f324c0a0694245a9ea03f59d1202ad8ee3cdfdcaf0b18f3d765e`
- Focused REA summary evidence: `ev_a1cb527680a1b5550b4b211b869bda70e3adbc732e39a0b7a9dff2f13242633e`

The focused analysis parsed two JavaScript sources without parse failures. It used
bounded semantic extraction, left 469 archive files unselected, and did not execute
the reference release. Omissions include the reference's cloud/mobile ambitions,
DOCX export and its web rendering stack. omapad uses its existing native editor,
undo history, session snapshots and platform file dialogs.

## Validation

The shared regression suite passes 3,928 checks on macOS and 4,018 on both tested
Linux hosts. New cases cover title/tag parsing, combined filtering, stable note
identity, unrelated dirty code buffers, undoable formatting, imports, HTML escaping,
idle autosave, same-timestamp external edits, replacement symlinks, unavailable
folders, conflict recovery across restart, and capture from Read view.

Native Mac checks exercise New Note, typing, autosave, Write/Split/Read, focus mode,
return to Editor, and quit/relaunch using an isolated support directory. GTK checks
exercise new-note shortcuts, typing, autosave, mode switching and session restoration
on Wayland. The Read-to-Write focus transition is additionally checked with real
keyboard events in an isolated Xvfb display because the remote desktop sessions are
locked. The app and tests also build with the normal CMake and GNUmakefile targets.
