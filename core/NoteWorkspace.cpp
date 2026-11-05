#include "WorkspaceInternal.h"
#include "Scintilla.h"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
namespace omapad {
namespace {
constexpr size_t maxNoteBytes = 16 * 1024 * 1024;
std::string digest(const std::string &text) { return hashHex(text.data(), text.size(), HashKind::SHA256); }
bool readNoteBytes(const std::string &path, std::string &out) {
    std::error_code ec;
    if (!fs::is_regular_file(fs::symlink_status(path, ec)) || ec || fs::file_size(path, ec) > maxNoteBytes || ec) return false;
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream buffer; buffer << file.rdbuf();
    if (file.bad()) return false;
    out = buffer.str(); return true;
}
int64_t noteTime(const std::string &path) {
    std::error_code ec; const auto time = fs::last_write_time(path, ec);
    return ec ? 0 : static_cast<int64_t>(time.time_since_epoch().count());
}
bool markdownFile(const fs::path &p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext == ".md" || ext == ".markdown";
}
// Autosave deliberately has no truncate-in-place fallback. Compare again immediately before
// rename so an external edit during temporary-file creation is also detected.
bool atomicNote(const std::string &path, const std::string &bytes, const std::string &expected, std::string &error) {
    std::string disk;
    if (!readNoteBytes(path, disk) || digest(disk) != expected) { error = "Changed or removed on disk. Reload the note or use Save a Copy to keep both versions."; return false; }
    std::string temp = (fs::path(path).parent_path() / ".omapad-note-XXXXXX").string();
    const int fd = mkstemp(temp.data());
    if (fd < 0) { error = std::strerror(errno); return false; }
    struct stat info;
    bool ok = fstat(fd, &info) == 0;
    if (stat(path.c_str(), &info) == 0 && fchmod(fd, info.st_mode & 0777) != 0) ok = false;
    size_t done = 0;
    while (ok && done < bytes.size()) {
        const ssize_t wrote = write(fd, bytes.data() + done, bytes.size() - done);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) { ok = false; break; }
        done += static_cast<size_t>(wrote);
    }
    if (ok && fsync(fd) != 0) ok = false;
    int savedError = errno;
    if (close(fd) != 0 && ok) { ok = false; savedError = errno; }
    if (ok && (!readNoteBytes(path, disk) || digest(disk) != expected)) {
        ok = false; error = "Changed on disk while saving. Your edit is still open; use Save a Copy.";
    }
    if (ok && rename(temp.c_str(), path.c_str()) != 0) { ok = false; savedError = errno; }
    if (!ok) { unlink(temp.c_str()); if (error.empty()) error = std::strerror(savedError); }
    return ok;
}
std::string escapeHtml(const std::string &text) {
    std::string out; for (char c : text) switch (c) {
        case '&': out += "&amp;"; break; case '<': out += "&lt;"; break; case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break; case '\'': out += "&#39;"; break; default: out += c;
    } return out;
}
std::string noteHtml(const NotesState &state) {
    std::string out = "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width\"><title>" + escapeHtml(state.title) + "</title><style>body{max-width:46rem;margin:3rem auto;padding:0 1.5rem;font:18px/1.65 system-ui;color:#263238;background:#fff}h1,h2,h3{line-height:1.25}pre,code{background:#f1f4f5;border-radius:4px}pre{padding:1rem;overflow:auto}blockquote{border-left:3px solid #218b8b;margin-left:0;padding-left:1rem}a{color:#147d92}.line{white-space:pre-wrap}</style></head><body>";
    for (const auto &block : state.preview) {
        if (block.kind == NoteBlock::Kind::Rule) { out += "<hr>"; continue; }
        const std::string tag = block.kind == NoteBlock::Kind::Heading ? "h" + std::to_string(std::clamp(block.level, 1, 6)) : block.kind == NoteBlock::Kind::Code ? "pre" : block.kind == NoteBlock::Kind::Quote ? "blockquote" : "p";
        out += "<" + tag + " class=\"line\">";
        if (block.kind == NoteBlock::Kind::List) out += "• ";
        if (block.kind == NoteBlock::Kind::Task) out += block.checked ? "☑ " : "☐ ";
        for (const auto &span : block.spans) {
            std::string t = escapeHtml(span.text);
            if (span.style & 1) t = "<strong>" + t + "</strong>";
            if (span.style & 2) t = "<em>" + t + "</em>";
            if (span.style & 4) t = "<code>" + t + "</code>";
            if ((span.style & 8) && (span.href.rfind("https://", 0) == 0 || span.href.rfind("http://", 0) == 0)) t = "<a href=\"" + escapeHtml(span.href) + "\">" + t + "</a>";
            out += t;
        }
        out += "</" + tag + ">\n";
    }
    return out + "</body></html>\n";
}
}

bool Workspace::isNoteLocation(const std::string &path) const {
    if (path.empty() || notesFolderPath.empty() || !markdownFile(path)) return false;
    const auto parent = fs::path(path).parent_path();
    if (parent.string() == notesFolderPath) return true;
    std::error_code ec;
    const auto canonical = fs::weakly_canonical(parent, ec);
    return !ec && canonical.string() == notesFolderPath;
}

bool Workspace::isNotePath(const std::string &path) const {
    if (path.empty() || notesFolderPath.empty() || !markdownFile(path)) return false;
    std::error_code ec;
    if (fs::is_symlink(fs::symlink_status(path, ec)) || ec) return false;
    const auto parent = fs::canonical(fs::path(path).parent_path(), ec);
    return !ec && parent.string() == notesFolderPath;
}

void Workspace::rememberNoteBaseline(const Document &document) {
    if (!isNotePath(document.path)) return;
    std::string disk;
    if (readNoteBytes(document.path, disk)) noteBaselines[document.path] = digest(disk);
}

bool Workspace::setNotesFolder(const std::string &path) {
    if (path.empty() || !flushNotes()) return false;
    std::error_code ec;
    const auto requested = fs::path(expandPath(path));
    fs::create_directories(requested, ec);
    if (!ec && !fs::is_directory(requested, ec)) ec = std::make_error_code(std::errc::not_a_directory);
    const auto directory = ec ? fs::path() : fs::canonical(requested, ec);
    if (ec) { notesError = "Cannot open notes folder: " + ec.message(); notifyNotes(); return false; }
    notesFolderPath = directory.string();
    settings.prefs["notesFolder"] = notesFolderPath;
    noteBaselines.clear(); noteSeenGeneration.clear(); notesQuery.clear(); notesTag.clear(); notesError.clear();
    saveSettings(); refreshNotes();
    if (notesEnabled) {
        if (!noteIndex.empty()) return openNote(noteIndex.front().path);
        return newNote();
    }
    return true;
}

bool Workspace::setNotesMode(bool enabled) {
    if (notesEnabled == enabled) return true;
    if (!enabled && !flushNotes()) return false;
    if (enabled) {
        if (notesFolderPath.empty()) {
            const std::string configured = settings.pref("notesFolder");
            const char *home = getenv("HOME");
            const std::string folder = configured.empty() ? (fs::path(home ? home : stateDir) / "Documents" / "omapad Notes").string() : configured;
            if (!setNotesFolder(folder)) return false;
        }
        notesPreviousDocument = activeTabPointer() ? activeTabPointer()->document : std::shared_ptr<Document>();
        notesPreviousPane = focus;
        notesEnabled = true;
        refreshNotes();

    } else {
        notesEnabled = false;
    }
    settings.prefs["notesMode"] = enabled ? "1" : "0";
    saveSettings();
    for (int pane = 0; pane < 2; ++pane) { applyViewPreferences(pane); applyLanguageTo(pane); }
    applyChromeState(); notifyNotes();
    if (enabled) {
        auto found = std::find_if(noteIndex.begin(), noteIndex.end(), [&](const NoteSummary &note) { return note.path == notesLastPath; });
        if (found != noteIndex.end()) openNote(found->path);
        else if (!noteIndex.empty()) openNote(noteIndex.front().path);
        else newNote();
    } else if (const auto prior = notesPreviousDocument.lock()) {
        for (int pn : {notesPreviousPane, 1 - notesPreviousPane})
            for (int i = 0; i < static_cast<int>(paneAt(pn).tabs.size()); ++i)
                if (paneAt(pn).tabs[i].document == prior) { focus = pn; activate(pn, i); host.focusPane(pn); return true; }
    }
    return true;
}

void Workspace::restoreNotesMode() {
    notesFolderPath = settings.pref("notesFolder");
    notesLastPath = settings.pref("notesLastPath");
    notesViewMode = std::clamp(static_cast<int>(settings.prefInt("notesView")), 0, 2);
    if (settings.prefOn("notesMode")) setNotesMode(true);
}

void Workspace::refreshNotes() {
    if (notesFolderPath.empty()) { noteIndex.clear(); notifyNotes(); return; }
    std::vector<NoteSummary> fresh;
    std::error_code ec;
    size_t bytes = 0;
    fs::directory_iterator it(notesFolderPath, ec), end;
    while (!ec && it != end && fresh.size() < 10000 && bytes < 64 * 1024 * 1024) {
        const fs::path p = it->path();
        if (p.filename().string().front() != '.' && markdownFile(p) && isNotePath(p.string())) {
            std::string text;
            if (readNoteBytes(p.string(), text) && isValidUtf8(text.data(), text.size())) {
                bytes += text.size(); fresh.push_back(describeNote(p.string(), text, noteTime(p.string())));
            }
        }
        it.increment(ec);
    }
    if (ec) notesError = "Cannot read notes folder: " + ec.message();
    for (const auto &document : documents) {
        if (!isNoteLocation(document->path)) continue;
        auto entry = std::find_if(fresh.begin(), fresh.end(), [&](const NoteSummary &n) { return n.path == document->path; });
        if (document->dirty()) {
            auto note = describeNote(document->path, textOf(*document), INT64_MAX - static_cast<int64_t>(documents.size()));
            if (entry == fresh.end()) fresh.push_back(std::move(note)); else *entry = std::move(note);
        }
    }
    std::sort(fresh.begin(), fresh.end(), [](const NoteSummary &a, const NoteSummary &b) { return a.modified != b.modified ? a.modified > b.modified : a.path < b.path; });
    noteIndex = std::move(fresh); notifyNotes();
}

NotesState Workspace::notesState() const {
    NotesState state;
    state.enabled = notesEnabled; state.focus = notesFocus; state.viewMode = notesViewMode;
    state.folder = notesFolderPath; state.query = notesQuery; state.tag = notesTag; state.total = noteIndex.size();
    std::map<std::string, int> tags;
    for (const auto &note : noteIndex) {
        std::set<std::string> perNote;
        for (const auto &tag : note.tags) {
            perNote.insert(tag);
            for (size_t i = tag.find('/'); i != std::string::npos; i = tag.find('/', i + 1)) perNote.insert(tag.substr(0, i));
        }
        for (const auto &tag : perNote) ++tags[tag];
        if (noteMatches(note, notesQuery, notesTag)) {
            state.notes.push_back(note); state.notes.back().content.clear();
        }
    }
    state.tags.assign(tags.begin(), tags.end());
    const Document *document = activeDocument();
    if (document && isNotePath(document->path)) {
        const std::string text = textOf(*document);
        state.activeNote = true; state.selectedPath = document->path;
        state.title = describeNote(document->path, text).title; state.words = noteWordCount(text);
        // Keep preview generation bounded independently of the editor's file-size limits.
        if (text.size() <= 1024 * 1024) state.preview = renderNote(text);
        else state.preview = renderNote("Preview is available for notes up to 1 MB. The full note is editable in Source.");
        state.status = document->dirty() ? "Unsaved changes" : "Saved";
    } else state.status = "Select a note, or create a new one";
    if (!notesError.empty()) { state.status = notesError; state.saveError = true; }
    return state;
}

void Workspace::notifyNotes() {
    if (notesNotifying) return;
    notesNotifying = true;
    host.notesChanged(notesState());
    notesNotifying = false;
}
void Workspace::setNotesQuery(const std::string &query) { notesQuery = query; notifyNotes(); }
void Workspace::setNotesTag(const std::string &tag) { notesTag = tag; notifyNotes(); }

bool Workspace::openNote(std::string path) {
    if (!isNotePath(path) || !flushNotes()) return false;
    std::string bytes;
    if (!readNoteBytes(path, bytes) || !isValidUtf8(bytes.data(), bytes.size())) { notesError = "This note could not be read as UTF-8 Markdown."; notifyNotes(); return false; }
    focus = 0;
    if (!openPath(path)) return false;
    // An existing clone may have selected pane 1; clone it into the notes pane without closing it.
    if (focus != 0) { cloneTabToOtherView(focus, paneAt(focus).active); focus = 0; }
    Document *document = activeDocument();
    if (document && !document->dirty()) {
        // openPath may have activated a cached tab. Read the validated UTF-8 bytes rather
        // than blessing a stale buffer with the hash of a newer external version.
        const bool bom = bytes.compare(0, 3, "\xef\xbb\xbf") == 0;
        const std::string content = bom ? bytes.substr(3) : bytes;
        document->encoding = encodingWithIdentifier(bom ? "utf8bom" : "utf8");
        if (!document->encoding) document->encoding = encodingWithIdentifier("utf8");
        if (textOf(*document) != content) loadInto(*document, content);
        document->mtime = noteTime(path); document->hasMtime = true;
        noteBaselines[path] = digest(bytes);
    }
    notesLastPath = path; settings.prefs["notesLastPath"] = path; saveSettings();
    applyNotesPreferences(focus); notifyNotes(); host.focusPane(focus);
    return true;
}

bool Workspace::newNote(const std::string &initial) {
    if (!notesEnabled) {
        if (!setNotesMode(true)) return false;
        if (initial.empty()) return true;
    }
    if (!flushNotes()) return false;
    if (initial.size() > maxNoteBytes || !isValidUtf8(initial.data(), initial.size())) { notesError = "Notes must be UTF-8 text under 16 MB."; notifyNotes(); return false; }
    std::string pattern = (fs::path(notesFolderPath) / "note-XXXXXX.md").string();
    const int fd = mkstemps(pattern.data(), 3);
    if (fd < 0) { notesError = "Cannot create note: " + std::string(std::strerror(errno)); notifyNotes(); return false; }
    close(fd);
    std::string error;
    if (!atomicNote(pattern, initial, digest(""), error)) { unlink(pattern.c_str()); notesError = "Cannot create note: " + error; notifyNotes(); return false; }
    notesQuery.clear(); notesTag.clear(); notesError.clear();
    if (notesViewMode == 2) { notesViewMode = 0; settings.prefs["notesView"] = "0"; }
    refreshNotes(); return openNote(pattern);
}

bool Workspace::saveNote(Document &document) {
    if (!document.dirty()) return true;
    if (!isNotePath(document.path) || savingForbidden) { notesError = "Note not saved: its folder changed, or saving is disabled. Your text is still open."; return false; }
    const auto baseline = noteBaselines.find(document.path);
    if (baseline == noteBaselines.end()) { notesError = "Note has no safe disk baseline. Save a Copy or reload before autosaving."; return false; }
    const std::string text = textOf(document);
    std::string bytes;
    if (!encode(text, document.encoding, bytes) || bytes.size() > maxNoteBytes) { notesError = "Cannot save this note in its encoding or size. Use Save a Copy."; return false; }
    std::string error;
    if (!atomicNote(document.path, bytes, baseline->second, error)) { notesError = "Not saved — " + error; return false; }
    noteBaselines[document.path] = digest(bytes);
    document.mtime = noteTime(document.path); document.hasMtime = true; document.missing = false;
    document.forceDirty = false;
    { BackgroundDocument background(host, document); background.view.send(SCI_SETSAVEPOINT); }
    document.modified = false;
    removeBackup(document); notifyPlugins("fileSaved", document.path);
    return true;
}

bool Workspace::flushNotes() {
    bool ok = true;
    for (const auto &document : documents) {
        if (!document->dirty() || !isNoteLocation(document->path)) continue;
        if (!saveNote(*document)) ok = false;
    }
    if (ok) notesError.clear();
    if (notesEnabled) { refreshNotes(); refreshChrome(); }
    return ok;
}

void Workspace::notesTick() {
    if (!notesEnabled) return;
    bool changed = false;
    for (const auto &document : documents) {
        if (!document->dirty() || !isNoteLocation(document->path)) continue;
        auto last = noteSeenGeneration.find(document->path);
        if (last != noteSeenGeneration.end() && last->second == document->editGeneration) {
            if (saveNote(*document)) { notesError.clear(); changed = true; }
        }
        noteSeenGeneration[document->path] = document->editGeneration;
    }
    // Include unsaved titles/tags promptly, while admitting external new/deleted files every five seconds.
    if (changed || tickCount % 5 == 0) refreshNotes();
    else {
        for (auto &note : noteIndex) for (const auto &document : documents) if (document->path == note.path && document->dirty()) note = describeNote(note.path, textOf(*document), note.modified);
        notifyNotes();
    }
}

void Workspace::applyNotesPreferences(int pane) {
    const Pane &p = paneAt(pane);
    if (!notesEnabled || p.active < 0 || !isNotePath(p.tabs[p.active].document->path)) return;
    SciView &v = view(pane);
    v.send(SCI_SETWRAPMODE, SC_WRAP_WORD); v.send(SCI_SETVIEWWS, SCWS_INVISIBLE); v.send(SCI_SETVIEWEOL, 0);
    v.send(SCI_SETINDENTATIONGUIDES, SC_IV_NONE); v.send(SCI_SETEDGEMODE, EDGE_NONE); v.send(SCI_SETCARETLINEVISIBLE, 0);
    for (int i = 0; i < 5; ++i) v.send(SCI_SETMARGINWIDTHN, i, 0);
    v.send(SCI_SETMARGINLEFT, 0, 22); v.send(SCI_SETMARGINRIGHT, 0, 22);
    v.send(SCI_AUTOCCANCEL); v.send(SCI_CALLTIPCANCEL);
}

void Workspace::formatNote(const std::string &action) {
    if (!notesEnabled || !activeDocument() || !isNotePath(activePath()) || s(SCI_GETREADONLY)) return;
    long start = s(SCI_GETSELECTIONSTART), end = s(SCI_GETSELECTIONEND);
    if (action == "bold" || action == "italic" || action == "code" || action == "link") {
        const std::string selected = rangeOf(start, end);
        const std::string before = action == "bold" ? "**" : action == "italic" ? "*" : action == "code" ? "`" : "[";
        const std::string after = action == "link" ? "](https://)" : before;
        s(SCI_BEGINUNDOACTION); replaceRange(start, end, before + selected + after, false);
        if (action == "link") s(SCI_SETSEL, start + 1 + selected.size() + 2, start + 1 + selected.size() + 10);
        else s(SCI_SETSEL, start + before.size(), start + before.size() + selected.size());
        s(SCI_ENDUNDOACTION);
    } else {
        std::string prefix;
        if (action == "heading") prefix = "# "; else if (action == "bullet") prefix = "- ";
        else if (action == "task") prefix = "- [ ] "; else if (action == "quote") prefix = "> "; else return;
        const long first = s(SCI_LINEFROMPOSITION, start);
        long last = s(SCI_LINEFROMPOSITION, end);
        if (last > first && end == s(SCI_POSITIONFROMLINE, last)) --last;
        s(SCI_BEGINUNDOACTION);
        for (long line = last; line >= first; --line) {
            const long pos = s(SCI_POSITIONFROMLINE, line);
            const std::string existing = rangeOf(pos, s(SCI_GETLINEENDPOSITION, line));
            if (action == "task" && existing.rfind("- [ ] ", 0) == 0) replaceRange(pos, pos + 6, "- [x] ", false);
            else if (action == "task" && existing.rfind("- [x] ", 0) == 0) replaceRange(pos, pos + 6, "- [ ] ", false);
            else replaceRange(pos, pos, prefix, false);
        }
        s(SCI_ENDUNDOACTION);
    }
    refreshNotes(); host.focusPane(focus);
}

bool Workspace::runNotesCommand(Cmd cmd, int arg, const std::string &text) {
    switch (cmd) {
    case Cmd::NotesMode: setNotesMode(!notesEnabled); return true;
    case Cmd::NotesNew: newNote(); return true;
    case Cmd::NotesFolder: { std::string folder; if (host.chooseFolder(notesFolderPath, folder)) setNotesFolder(folder); return true; }
    case Cmd::NotesReveal: if (!notesFolderPath.empty()) openExternally(notesFolderPath); return true;
    case Cmd::NotesRefresh: refreshNotes(); return true;
    case Cmd::NotesRetry: flushNotes(); return true;
    case Cmd::NotesView: notesViewMode = std::clamp(arg, 0, 2); settings.prefs["notesView"] = std::to_string(notesViewMode); saveSettings(); notifyNotes(); return true;
    case Cmd::NotesFocus: notesFocus = !notesFocus; notifyNotes(); return true;
    case Cmd::NotesFormat: formatNote(text); return true;
    case Cmd::NotesImport: {
        std::vector<std::string> paths;
        if (host.chooseFilesToOpen(notesFolderPath, paths)) for (const auto &path : paths) {
            std::string content;
            if (!readNoteBytes(path, content) || !isValidUtf8(content.data(), content.size())) { notesError = "Import needs a UTF-8 text file under 16 MB."; notifyNotes(); break; }
            if (!newNote(content)) break;
        }
        return true;
    }
    case Cmd::NotesTrash: {
        Document *document = activeDocument();
        if (!document || !isNotePath(document->path)) return true;
        const std::string path = document->path;
        if (host.ask("Move this note to Trash?", describeNote(path, textOf(*document)).title, {"Cancel", "Move to Trash"}) != 1 || !flushNotes()) return true;
        std::string error;
        if (!host.moveToTrash(path, error)) { notesError = "Could not move note to Trash: " + error; notifyNotes(); return true; }
        for (int pane = 0; pane < 2; ++pane) for (int i = static_cast<int>(paneAt(pane).tabs.size()) - 1; i >= 0; --i)
            if (paneAt(pane).tabs[i].document->path == path) closeTab(pane, i);
        noteBaselines.erase(path); noteSeenGeneration.erase(path); refreshNotes();
        if (!noteIndex.empty()) openNote(noteIndex.front().path); else newNote();
        return true;
    }
    case Cmd::NotesExportHtml: {
        const NotesState state = notesState(); if (!state.activeNote) return true;
        std::string path;
        if (host.chooseSavePath("note.html", notesFolderPath, path)) {
            std::string error;
            if (isNotePath(path)) { host.ask("Choose an HTML export destination", "Export to an .html file to keep your Markdown notes intact.", {"OK"}); return true; }
            NotesState exported = state;
            exported.preview = renderNote(textOf(*activeDocument()));
            if (!atomicWrite(noteHtml(exported), path, -1, error)) host.ask("Could not export note", error, {"OK"});
        }
        return true;
    }
    default: return false;
    }
}
}
