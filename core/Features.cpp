// Commands that aren't part of the editing core: sessions, tabs, panels, macros, Run, plugins' tools.
#include "WorkspaceInternal.h"

#include "SciLexer.h"
#include "Scintilla.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <fnmatch.h>
#include <functional>
#include <climits>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <spawn.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern char **environ;

namespace fs = std::filesystem;

namespace omapad {

namespace {

std::string lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return value;
}

std::string extensionOf(const std::string &path) {
	std::string extension = fs::path(path).extension().string();
	if (!extension.empty() && extension[0] == '.') extension.erase(0, 1);
	return extension;
}

std::string replaceEvery(std::string text, const std::string &what, const std::string &with) {
	for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + with.size())) text.replace(at, what.size(), with);
	return text;
}

std::string readWholeFile(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	std::stringstream out;
	out << in.rdbuf();
	return out.str();
}

FormField formField(FormField::Kind kind, const std::string &key, const std::string &label, const std::string &value = std::string()) {
	FormField field;
	field.kind = kind;
	field.key = key;
	field.label = label;
	field.value = value;
	return field;
}

const FormField *fieldNamed(const std::vector<FormField> &fields, const char *key) {
	for (const FormField &field : fields)
		if (field.key == key) return &field;
	return nullptr;
}

std::string fieldValue(const std::vector<FormField> &fields, const char *key) {
	const FormField *field = fieldNamed(fields, key);
	return field ? field->value : std::string();
}

}  // namespace

std::string currentExecutablePath() {
#ifdef __APPLE__
	char buffer[PATH_MAX];
	uint32_t size = sizeof buffer;
	if (_NSGetExecutablePath(buffer, &size) == 0) return buffer;
	return std::string();
#else
	char buffer[PATH_MAX];
	const ssize_t length = readlink("/proc/self/exe", buffer, sizeof buffer - 1);
	return length > 0 ? std::string(buffer, static_cast<size_t>(length)) : std::string();
#endif
}

bool spawnDetached(const std::vector<std::string> &arguments, const std::string &directory, std::string &error) {
	if (arguments.empty()) return false;
	const pid_t child = fork();
	if (child < 0) {
		error = std::strerror(errno);
		return false;
	}
	if (child == 0) {
		// A second fork, so the program outlives us and never becomes a zombie of ours.
		if (fork() != 0) _exit(0);
		setsid();
		if (!directory.empty() && chdir(directory.c_str()) != 0) { /* Run where we are instead. */ }
		std::vector<char *> argv;
		for (const std::string &argument : arguments) argv.push_back(const_cast<char *>(argument.c_str()));
		argv.push_back(nullptr);
		execvp(argv[0], argv.data());
		_exit(127);
	}
	int status = 0;
	waitpid(child, &status, 0);
	return true;
}

bool commandExists(const std::string &name) {
	const char *path = getenv("PATH");
	if (!path) return false;
	std::stringstream parts(path);
	std::string part;
	while (std::getline(parts, part, ':')) {
		const std::string candidate = part + "/" + name;
		if (access(candidate.c_str(), X_OK) == 0) return true;
	}
	return false;
}

#pragma mark - Opening things elsewhere

void Workspace::openExternally(const std::string &target) {
#ifdef __APPLE__
	host.launch({"open", target}, std::string());
#else
	host.launch({"xdg-open", target}, std::string());
#endif
}

void Workspace::openTerminalAt(const std::string &directory) {
#ifdef __APPLE__
	host.launch({"open", "-a", "Terminal", directory}, std::string());
#else
	// Omarchy and most other desktops provide one of these.
	const char *configured = getenv("TERMINAL");
	std::vector<std::string> candidates;
	if (configured && *configured) candidates.push_back(configured);
	for (const char *name : {"xdg-terminal-exec", "alacritty", "kitty", "ghostty", "foot", "gnome-terminal", "konsole", "xterm"}) candidates.push_back(name);
	for (const std::string &candidate : candidates) {
		if (commandExists(candidate)) {
			host.launch({candidate}, directory);
			return;
		}
	}
	host.flash("No terminal program was found");
#endif
}

void Workspace::viewInBrowser(const std::string &browser) {
	const Document *document = activeDocument();
	if (!document || document->path.empty()) {
		host.flash("Save the file first");
		return;
	}
#ifdef __APPLE__
	if (browser.empty()) {
		host.launch({"open", document->path}, std::string());
		return;
	}
	static const std::map<std::string, std::string> apps = {{"firefox", "Firefox"}, {"chrome", "Google Chrome"}, {"edge", "Microsoft Edge"}, {"safari", "Safari"}};
	const auto found = apps.find(browser);
	if (found == apps.end()) return;
	if (!fs::exists("/Applications/" + found->second + ".app")) {
		host.flash(found->second + " isn't installed");
		return;
	}
	host.launch({"open", "-a", found->second, document->path}, std::string());
#else
	if (browser.empty()) {
		host.launch({"xdg-open", document->path}, std::string());
		return;
	}
	static const std::map<std::string, std::vector<std::string>> commands = {
		{"firefox", {"firefox", "firefox-esr"}},
		{"chrome", {"google-chrome-stable", "google-chrome", "chromium", "chromium-browser"}},
		{"edge", {"microsoft-edge-stable", "microsoft-edge"}},
	};
	const auto found = commands.find(browser);
	if (found == commands.end()) return;
	for (const std::string &command : found->second) {
		if (commandExists(command)) {
			host.launch({command, document->path}, std::string());
			return;
		}
	}
	host.flash("That browser isn't installed");
#endif
}

void Workspace::openNewInstance(const std::string &path) {
	const std::string executable = currentExecutablePath();
	if (executable.empty()) return;
	std::vector<std::string> arguments = {executable, "-multiInst", "-nosession"};
	if (!path.empty()) arguments.push_back(path);
	host.launch(arguments, std::string());
}

#pragma mark - Sessions

std::string Workspace::sessionFileText() {
	json out;
	json list = json::array();
	for (int pn = 0; pn < 2; pn++) {
		saveViewState(pn);
		const Pane &p = paneAt(pn);
		for (int i = 0; i < static_cast<int>(p.tabs.size()); i++) {
			const Tab &tab = p.tabs[i];
			if (tab.document->path.empty()) continue;
			json entry;
			entry["path"] = tab.document->path;
			entry["pane"] = pn;
			entry["caret"] = tab.caret;
			entry["anchor"] = tab.anchor;
			entry["firstLine"] = tab.firstLine;
			entry["language"] = tab.document->language->name;
			entry["encoding"] = tab.document->encoding->identifier;
			entry["pinned"] = tab.pinned;
			entry["colour"] = tab.colour;
			entry["active"] = i == p.active;
			list.push_back(entry);
		}
	}
	out["omapadSession"] = 1;
	out["files"] = list;
	out["folders"] = folderRoots;
	return out.dump(1, '\t', false, json::error_handler_t::replace);
}

bool Workspace::loadSessionFile(const std::string &path) {
	const json session = json::parse(readWholeFile(path), nullptr, false);
	std::vector<std::pair<int, std::string>> activeTabs;
	if (session.is_object() && session.contains("files") && session["files"].is_array()) {
		for (const json &entry : session["files"]) {
			if (!entry.is_object()) continue;
			const std::string file = jsonString(entry, "path");
			std::error_code error;
			if (file.empty() || !fs::exists(file, error)) continue;
			const int pane = jsonLong(entry, "pane") == 1 ? 1 : 0;
			const int previousFocus = focus;
			if (pane == 1 && !splitVisible()) {
				// The second view appears when its first tab does.
				openPath(file);
				moveTabToOtherView(focus, paneAt(focus).active);
			} else {
				focus = pane;
				openPath(file);
			}
			Tab *tab = activeTabPointer();
			if (tab) {
				tab->pinned = jsonBool(entry, "pinned");
				tab->colour = static_cast<int>(jsonLong(entry, "colour"));
				s(SCI_SETSEL, jsonLong(entry, "anchor"), jsonLong(entry, "caret"));
				s(SCI_SETFIRSTVISIBLELINE, jsonLong(entry, "firstLine"));
				const std::string language = jsonString(entry, "language");
				if (!language.empty() && tab->document->language->name != language) setActiveLanguage(language);
				if (jsonBool(entry, "active")) activeTabs.emplace_back(focus, file);
			}
			(void)previousFocus;
		}
		for (const std::string &folder : jsonStrings(session, "folders")) addFolder(folder);
		for (const auto &active : activeTabs) {
			int pane, index;
			findOpen(active.second, pane, index);
			if (pane >= 0) activate(pane, index);
		}
		return true;
	}
	// Notepad++'s own session files.
	pugi::xml_document document;
	if (!document.load_file(path.c_str())) return false;
	const pugi::xml_node root = document.child("NotepadPlus").child("Session");
	if (!root) return false;
	const auto loadView = [&](const char *name) {
		for (const pugi::xml_node file : root.child(name).children("File")) {
			const std::string filename = file.attribute("filename").as_string();
			std::error_code error;
			if (!filename.empty() && fs::exists(filename, error)) {
				openPath(filename);
				s(SCI_GOTOPOS, file.attribute("startPos").as_llong());
			}
		}
	};
	loadView("mainView");
	loadView("subView");
	return true;
}

#pragma mark - Editing commands

void Workspace::beginEndSelect(bool column) {
	const long position = s(SCI_GETCURRENTPOS);
	if (selectAnchor < 0) {
		selectAnchor = position;
		selectAnchorColumn = column;
		host.flash(column ? "Begin column selection: go to the end, then choose it again" : "Begin selection: go to the end, then choose it again");
		return;
	}
	if (selectAnchorColumn || column) {
		s(SCI_SETSELECTIONMODE, SC_SEL_RECTANGLE);
		s(SCI_SETRECTANGULARSELECTIONANCHOR, selectAnchor);
		s(SCI_SETRECTANGULARSELECTIONCARET, position);
	} else {
		s(SCI_SETSEL, selectAnchor, position);
	}
	selectAnchor = -1;
}

void Workspace::insertDate(int kind) {
	const std::string &custom = settings.dateTimeFormat;
	std::string format;
	if (kind == 2) {
		format = custom;
	} else {
		const std::string time = "HH:mm";
		const std::string date = kind == 0 ? "M/d/yyyy" : "dddd, MMMM d, yyyy";
		format = settings.dateTimeReverse ? date + " " + time : time + " " + date;
	}
	const std::string text = formatDateTime(format, time(nullptr));
	s(SCI_REPLACESEL, 0, reinterpret_cast<intptr_t>(text.c_str()));
}

void Workspace::copyAllNames(bool fullPaths) {
	std::string out;
	for (int pn = 0; pn < 2; pn++)
		for (const Tab &tab : paneAt(pn).tabs) {
			out += fullPaths && !tab.document->path.empty() ? tab.document->path : tab.document->title();
			out += '\n';
		}
	host.setClipboard(out);
}

void Workspace::blockUncomment() {
	const Document *document = activeDocument();
	if (!document) return;
	const std::string &open = document->language->commentStart;
	const std::string &close = document->language->commentEnd;
	if (open.empty() || close.empty()) {
		host.flash(document->language->displayName + " has no block comment syntax");
		return;
	}
	long start = s(SCI_GETSELECTIONSTART);
	long end = s(SCI_GETSELECTIONEND);
	const long length = s(SCI_GETLENGTH);
	// With no markers in the selection, the ones around the caret count.
	const std::string body = rangeOf(start, end);
	size_t at = body.find(open);
	if (at == std::string::npos || start == end) {
		const std::string before = rangeOf(0, start);
		const size_t openAt = before.rfind(open);
		const std::string after = rangeOf(start, length);
		const size_t closeAt = after.find(close);
		if (openAt == std::string::npos || closeAt == std::string::npos) {
			host.flash("The caret isn't in a block comment");
			return;
		}
		start = static_cast<long>(openAt);
		end = s(SCI_GETSELECTIONSTART) + static_cast<long>(closeAt + close.size());
	}
	std::string inner = rangeOf(start, end);
	const size_t first = inner.find(open);
	const size_t last = inner.rfind(close);
	if (first == std::string::npos || last == std::string::npos || last < first + open.size()) {
		host.flash("The selection isn't a block comment");
		return;
	}
	inner.erase(last, close.size());
	inner.erase(first, open.size());
	replaceRange(start, end, inner, true);
}

void Workspace::eolAndTrimToSpace() {
	const long start = s(SCI_GETSELECTIONSTART), end = s(SCI_GETSELECTIONEND);
	const bool whole = start == end;
	const long from = whole ? 0 : start, to = whole ? s(SCI_GETLENGTH) : end;
	std::stringstream lines(rangeOf(from, to));
	std::string line, out;
	while (std::getline(lines, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		const size_t first = line.find_first_not_of(" \t");
		if (first == std::string::npos) continue;
		const size_t last = line.find_last_not_of(" \t");
		if (!out.empty()) out += ' ';
		out += line.substr(first, last - first + 1);
	}
	replaceRange(from, to, out, !whole);
}

void Workspace::pasteClipboardFormat(const char *format, const char *name) {
	std::string data;
	if (!host.clipboardData(format, data) || data.empty()) {
		host.flash(std::string("The clipboard has no ") + name + " content");
		return;
	}
	s(SCI_BEGINUNDOACTION);
	s(SCI_DELETERANGE, s(SCI_GETSELECTIONSTART), s(SCI_GETSELECTIONEND) - s(SCI_GETSELECTIONSTART));
	s(SCI_ADDTEXT, data.size(), reinterpret_cast<intptr_t>(data.data()));
	s(SCI_ENDUNDOACTION);
}

void Workspace::copyBinary(bool cut) {
	const long start = s(SCI_GETSELECTIONSTART), end = s(SCI_GETSELECTIONEND);
	if (start == end) return;
	binaryClipboard = rangeOf(start, end);
	// Other programs get as much as text can carry. Our paste uses the exact bytes.
	host.setClipboard(binaryClipboard);
	if (cut) s(SCI_DELETERANGE, start, end - start);
}

void Workspace::pasteBinary() {
	if (binaryClipboard.empty()) {
		pasteClipboardFormat("text", "text");
		return;
	}
	s(SCI_BEGINUNDOACTION);
	s(SCI_DELETERANGE, s(SCI_GETSELECTIONSTART), s(SCI_GETSELECTIONEND) - s(SCI_GETSELECTIONSTART));
	s(SCI_ADDTEXT, binaryClipboard.size(), reinterpret_cast<intptr_t>(binaryClipboard.data()));
	s(SCI_ENDUNDOACTION);
}

std::string Workspace::selectionAsPath() const {
	std::string text = selectedText();
	if (text.empty()) text = selectionOrWord();
	const size_t first = text.find_first_not_of(" \t\r\n\"'<>");
	if (first == std::string::npos) return std::string();
	const size_t last = text.find_last_not_of(" \t\r\n\"'<>");
	text = text.substr(first, last - first + 1);
	if (!text.empty() && text[0] != '/' && text[0] != '~') {
		const Document *document = activeDocument();
		if (document && !document->path.empty()) text = (fs::path(directoryOf(document->path)) / text).string();
	}
	return expandPath(text);
}

void Workspace::redactSelection(bool dots) {
	const long start = s(SCI_GETSELECTIONSTART), end = s(SCI_GETSELECTIONEND);
	if (start == end) {
		host.flash("Select the text to redact first");
		return;
	}
	const std::string mark = dots ? "●" : "█";
	std::string out;
	for (const char32_t cp : utf8ToCodePoints(rangeOf(start, end)))
		if (cp == '\r' || cp == '\n') appendUtf8(out, cp);
		else out += mark;
	replaceRange(start, end, out, false);
}

void Workspace::searchOnInternet() {
	const std::string word = selectionOrWord();
	if (word.empty()) return;
	std::string engine = settings.searchEngine;
	if (engine.find("$(CURRENT_WORD)") == std::string::npos) engine += "$(CURRENT_WORD)";
	openExternally(replaceEvery(engine, "$(CURRENT_WORD)", percentEncode(word)));
}

void Workspace::changeSearchEngine() {
	std::vector<FormField> fields;
	FormField choice = formField(FormField::Kind::Choice, "engine", "Search engine");
	choice.choices = {{"https://duckduckgo.com/?q=$(CURRENT_WORD)", "DuckDuckGo"}, {"https://www.google.com/search?q=$(CURRENT_WORD)", "Google"},
	                  {"https://search.yahoo.com/search?q=$(CURRENT_WORD)", "Yahoo!"}, {"https://stackoverflow.com/search?q=$(CURRENT_WORD)", "Stack Overflow"},
	                  {"custom", "Custom, below"}};
	choice.value = "custom";
	for (const auto &option : choice.choices)
		if (option.first == settings.searchEngine) choice.value = option.first;
	fields.push_back(choice);
	fields.push_back(formField(FormField::Kind::Text, "custom", "Custom address, with $(CURRENT_WORD)", settings.searchEngine));
	if (!host.runForm("Change Search Engine", fields)) return;
	const std::string chosen = fieldValue(fields, "engine");
	settings.searchEngine = chosen == "custom" ? fieldValue(fields, "custom") : chosen;
	settings.prefs["searchEnginePref"] = settings.searchEngine;
	saveSettings();
}

void Workspace::setAllReadOnly(bool on) {
	for (const auto &document : documents) {
		BackgroundDocument background(host, *document);
		background.view.send(SCI_SETREADONLY, on);
	}
	refreshChrome();
}

void Workspace::toggleFileReadOnly() {
	const Document *document = activeDocument();
	if (!document || document->path.empty()) return;
	struct stat info;
	if (stat(document->path.c_str(), &info) != 0) return;
	const bool writable = (info.st_mode & S_IWUSR) != 0;
	const mode_t mask = umask(0);
	umask(mask);
	const mode_t mode = writable ? (info.st_mode & ~(S_IWUSR | S_IWGRP | S_IWOTH)) : (info.st_mode | (S_IWUSR | ((S_IWGRP | S_IWOTH) & ~mask)));
	if (chmod(document->path.c_str(), mode) != 0) host.ask("The file's attribute could not be changed.", std::strerror(errno), {"OK"});
	s(SCI_SETREADONLY, writable);
	refreshChrome();
}

void Workspace::goToDialog() {
	const long lines = s(SCI_GETLINECOUNT);
	const long length = s(SCI_GETLENGTH);
	const long position = s(SCI_GETCURRENTPOS);
	std::vector<FormField> fields;
	FormField mode = formField(FormField::Kind::Choice, "mode", "Go to", "line");
	mode.choices = {{"line", "Line"}, {"offset", "Offset"}};
	fields.push_back(mode);
	fields.push_back(formField(FormField::Kind::Heading, "", "You are at line " + std::to_string(s(SCI_LINEFROMPOSITION, position) + 1) + " of " + std::to_string(lines) + ", offset " + std::to_string(position) + " of " + std::to_string(length)));
	fields.push_back(formField(FormField::Kind::Text, "target", "Go to"));
	if (!host.runForm("Go To", fields)) return;
	const long target = std::strtol(fieldValue(fields, "target").c_str(), nullptr, 10);
	if (fieldValue(fields, "mode") == "offset") {
		const long clamped = std::max(0L, std::min(target, length));
		s(SCI_GOTOPOS, clamped);
		s(SCI_ENSUREVISIBLEENFORCEPOLICY, s(SCI_LINEFROMPOSITION, clamped));
	} else if (target >= 1) {
		const long line = std::min(target - 1, lines - 1);
		s(SCI_ENSUREVISIBLEENFORCEPOLICY, line);
		s(SCI_GOTOLINE, line);
	}
}

void Workspace::pasteToBookmarked() {
	std::string text;
	if (!host.clipboardData("text", text) || text.empty()) {
		host.flash("The clipboard is empty");
		return;
	}
	while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
	const long mask = 1 << kMarkerBookmark;
	s(SCI_BEGINUNDOACTION);
	for (long line = s(SCI_GETLINECOUNT) - 1; line >= 0; line--) {
		if (!(s(SCI_MARKERGET, line) & mask)) continue;
		const long start = s(SCI_POSITIONFROMLINE, line);
		s(SCI_SETTARGETRANGE, start, s(SCI_GETLINEENDPOSITION, line));
		s(SCI_REPLACETARGET, text.size(), reinterpret_cast<intptr_t>(text.data()));
	}
	s(SCI_ENDUNDOACTION);
}

bool Workspace::findCharacterInRange(long from, long to, bool backward, bool wrap) {
	const long length = s(SCI_GETLENGTH);
	const long caret = backward ? s(SCI_GETSELECTIONSTART) : s(SCI_GETSELECTIONEND);
	const auto search = [&](long start, long end) {
		for (long at = start; backward ? at > end : at < end;) {
			const long position = backward ? s(SCI_POSITIONBEFORE, at) : at;
			const long next = s(SCI_POSITIONAFTER, position);
			const std::u32string decoded = utf8ToCodePoints(rangeOf(position, next));
			const long cp = decoded.empty() ? -1 : static_cast<long>(decoded[0]);
			if (cp >= from && cp <= to) {
				s(SCI_SETSEL, position, next);
				s(SCI_SCROLLCARET);
				return true;
			}
			if (backward) {
				if (position <= end) break;
				at = position;
			} else {
				if (next <= at) break;
				at = next;
			}
		}
		return false;
	};
	if (backward ? search(caret, 0) : search(caret, length)) return true;
	return wrap && (backward ? search(length, caret) : search(0, caret));
}

void Workspace::findCharactersDialog() {
	std::vector<FormField> fields;
	FormField range = formField(FormField::Kind::Choice, "range", "Search for", charRange.mode);
	range.choices = {{"high", "Non-ASCII characters (128 and up)"}, {"low", "ASCII characters (0 to 127)"}, {"custom", "This range"}};
	fields.push_back(range);
	FormField low = formField(FormField::Kind::Int, "from", "From (decimal)", std::to_string(charRange.from));
	low.maximum = 0x10FFFF;
	fields.push_back(low);
	FormField high = formField(FormField::Kind::Int, "to", "To (decimal)", std::to_string(charRange.to));
	high.maximum = 0x10FFFF;
	fields.push_back(high);
	fields.push_back(formField(FormField::Kind::Bool, "backward", "Search up", charRange.backward ? "1" : "0"));
	fields.push_back(formField(FormField::Kind::Bool, "wrap", "Wrap around", charRange.wrap ? "1" : "0"));
	if (!host.runForm("Find Characters in Range", fields)) return;
	charRange.mode = fieldValue(fields, "range");
	charRange.from = std::strtol(fieldValue(fields, "from").c_str(), nullptr, 10);
	charRange.to = std::strtol(fieldValue(fields, "to").c_str(), nullptr, 10);
	charRange.backward = fieldValue(fields, "backward") == "1";
	charRange.wrap = fieldValue(fields, "wrap") == "1";
	const long from = charRange.mode == "high" ? 128 : charRange.mode == "low" ? 0 : charRange.from;
	const long to = charRange.mode == "high" ? 0x10FFFF : charRange.mode == "low" ? 127 : charRange.to;
	if (!findCharacterInRange(from, to, charRange.backward, charRange.wrap)) host.flash("No character in that range");
}

void Workspace::setPrintableSymbols() {
	for (int pn = 0; pn < 2; pn++) {
		SciView &v = view(pn);
		v.send(SCI_CLEARALLREPRESENTATIONS);
		const auto show = [&](const char *utf8, const char *abbreviation, const char *codepoint) {
			const std::string text = settings.npcCodepoint ? codepoint : abbreviation;
			v.send(SCI_SETREPRESENTATION, reinterpret_cast<uintptr_t>(utf8), reinterpret_cast<intptr_t>(text.c_str()));
			const std::string colour = settings.pref("npcColour");
			if (colour.size() == 6 && colour.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
				const unsigned long rgb = std::strtoul(colour.c_str(), nullptr, 16);
				const intptr_t bgra = static_cast<intptr_t>(0xFF000000UL | ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF));
				v.send(SCI_SETREPRESENTATIONAPPEARANCE, reinterpret_cast<uintptr_t>(utf8), SC_REPRESENTATION_COLOUR);
				v.send(SCI_SETREPRESENTATIONCOLOUR, reinterpret_cast<uintptr_t>(utf8), bgra);
			}
		};
		if (settings.showNonPrinting) {
			show("\xC2\xA0", "NBSP", "U+00A0");
			show("\xC2\xAD", "SHY", "U+00AD");
			show("\xE1\x9A\x80", "OSPM", "U+1680");
			show("\xE2\x80\x80", "NQSP", "U+2000");
			show("\xE2\x80\x81", "MQSP", "U+2001");
			show("\xE2\x80\x82", "ENSP", "U+2002");
			show("\xE2\x80\x83", "EMSP", "U+2003");
			show("\xE2\x80\x84", "3/MSP", "U+2004");
			show("\xE2\x80\x85", "4/MSP", "U+2005");
			show("\xE2\x80\x86", "6/MSP", "U+2006");
			show("\xE2\x80\x87", "FSP", "U+2007");
			show("\xE2\x80\x88", "PSP", "U+2008");
			show("\xE2\x80\x89", "THSP", "U+2009");
			show("\xE2\x80\x8A", "HSP", "U+200A");
			show("\xE2\x80\x8B", "ZWSP", "U+200B");
			show("\xE2\x80\x8C", "ZWNJ", "U+200C");
			show("\xE2\x80\x8D", "ZWJ", "U+200D");
			show("\xE2\x80\x8E", "LRM", "U+200E");
			show("\xE2\x80\x8F", "RLM", "U+200F");
			show("\xE2\x80\xAA", "LRE", "U+202A");
			show("\xE2\x80\xAB", "RLE", "U+202B");
			show("\xE2\x80\xAC", "PDF", "U+202C");
			show("\xE2\x80\xAD", "LRO", "U+202D");
			show("\xE2\x80\xAE", "RLO", "U+202E");
			show("\xE2\x80\xAF", "NNBSP", "U+202F");
			show("\xE2\x81\x9F", "MMSP", "U+205F");
			show("\xE2\x81\xA0", "WJ", "U+2060");
			show("\xE3\x80\x80", "IDSP", "U+3000");
			show("\xEF\xBB\xBF", "ZWNBSP", "U+FEFF");
		}
		if (settings.showControlChars) {
			show("\xC2\x85", "NEL", "U+0085");
			show("\xE2\x80\xA8", "LS", "U+2028");
			show("\xE2\x80\xA9", "PS", "U+2029");
			for (int c = 0x80; c <= 0x9F; c++) {
				if (c == 0x85) continue;
				char utf8[3] = {static_cast<char>(0xC2), static_cast<char>(c), 0};
				char label[8];
				snprintf(label, sizeof label, "U+%04X", c);
				show(utf8, label, label);
			}
		}
	}
}

void Workspace::hideSelectedLines() {
	long first = s(SCI_LINEFROMPOSITION, s(SCI_GETSELECTIONSTART));
	long last = s(SCI_LINEFROMPOSITION, s(SCI_GETSELECTIONEND));
	const long lines = s(SCI_GETLINECOUNT);
	if (first == 0 && last >= lines - 1) {
		host.flash("At least one line has to stay visible");
		return;
	}
	s(SCI_HIDELINES, first, last);
	// Arrows in the margin mark the gap. Clicking one shows the lines again.
	s(SCI_MARKERADD, first > 0 ? first - 1 : last + 1, first > 0 ? kMarkerHiddenBegin : kMarkerHiddenEnd);
	Tab *tab = activeTabPointer();
	if (tab) tab->hiddenLines.emplace_back(first, last);
}

bool Workspace::showHiddenLinesAt(long line) {
	Tab *tab = activeTabPointer();
	if (!tab) return false;
	const long markers = s(SCI_MARKERGET, line);
	if (!(markers & ((1 << kMarkerHiddenBegin) | (1 << kMarkerHiddenEnd)))) return false;
	s(SCI_MARKERDELETE, line, kMarkerHiddenBegin);
	s(SCI_MARKERDELETE, line, kMarkerHiddenEnd);
	for (auto it = tab->hiddenLines.begin(); it != tab->hiddenLines.end();) {
		if (it->first - 1 == line || it->second + 1 == line) {
			s(SCI_SHOWLINES, it->first, it->second);
			it = tab->hiddenLines.erase(it);
		} else {
			++it;
		}
	}
	return true;
}

void Workspace::setTextDirection(bool rightToLeft) {
	Document *document = activeDocument();
	if (!document) return;
	document->rightToLeft = rightToLeft;
	s(SCI_SETBIDIRECTIONAL, rightToLeft ? SC_BIDIRECTIONAL_R2L : SC_BIDIRECTIONAL_DISABLED);
	if (rightToLeft && s(SCI_GETBIDIRECTIONAL) != SC_BIDIRECTIONAL_R2L) host.flash("Right-to-left text isn't available in the editor on this system");
}

void Workspace::toggleMonitoring() {
	Document *document = activeDocument();
	if (!document || document->path.empty()) {
		host.flash("Save the file first");
		return;
	}
	document->monitoring = !document->monitoring;
	s(SCI_SETREADONLY, document->monitoring);
	if (document->monitoring) s(SCI_DOCUMENTEND);
	refreshChrome();
}

void Workspace::updateMonitoredDocuments() {
	Document *active = activeDocument();
	if (!active || !active->monitoring) return;
	int64_t current = 0;
	std::error_code error;
	const auto time = fs::last_write_time(active->path, error);
	if (error) return;
	current = static_cast<int64_t>(time.time_since_epoch().count());
	if (active->hasMtime && current == active->mtime) return;
	s(SCI_SETREADONLY, 0);
	reloadActive();
	s(SCI_SETREADONLY, 1);
	s(SCI_DOCUMENTEND);
}

#pragma mark - Tabs and the Window menu

void Workspace::setTabColour(int pane, int index, int colour) {
	Pane &p = paneAt(pane);
	if (index >= 0 && index < static_cast<int>(p.tabs.size())) p.tabs[index].colour = colour;
	host.tabsChanged();
}

void Workspace::setTabPinned(int pane, int index, bool pinned) {
	Pane &p = paneAt(pane);
	const int count = static_cast<int>(p.tabs.size());
	if (index < 0 || index >= count) return;
	p.tabs[index].pinned = pinned;
	// Pinned tabs stay together on the left.
	int firstUnpinned = 0;
	while (firstUnpinned < count && p.tabs[firstUnpinned].pinned && firstUnpinned != index) firstUnpinned++;
	if (pinned && firstUnpinned < index) moveTab(pane, index, firstUnpinned);
	host.tabsChanged();
}

void Workspace::sortTabs(int kind) {
	Pane &p = paneAt(focus);
	if (p.tabs.size() < 2 || p.active < 0) return;
	saveViewState(focus);
	const std::shared_ptr<Document> active = p.tabs[p.active].document;
	const auto sizeOf = [&](const Document &document) {
		BackgroundDocument background(host, document);
		return static_cast<long>(background.view.send(SCI_GETLENGTH));
	};
	const auto timeOf = [](const Document &document) {
		std::error_code error;
		const auto when = fs::last_write_time(document.path, error);
		return error ? static_cast<int64_t>(0) : static_cast<int64_t>(when.time_since_epoch().count());
	};
	const bool descending = kind % 2 == 1;
	const int field = kind / 2;
	std::stable_sort(p.tabs.begin(), p.tabs.end(), [&](const Tab &a, const Tab &b) {
		// Pinned tabs always stay first.
		if (a.pinned != b.pinned) return a.pinned;
		const Document &x = descending ? *b.document : *a.document;
		const Document &y = descending ? *a.document : *b.document;
		switch (field) {
		case 0: return lower(x.title()) < lower(y.title());
		case 1: return lower(x.path.empty() ? x.title() : x.path) < lower(y.path.empty() ? y.title() : y.path);
		case 2: return lower(extensionOf(x.title())) < lower(extensionOf(y.title()));
		case 3: return sizeOf(x) < sizeOf(y);
		default: return timeOf(x) < timeOf(y);
		}
	});
	for (int i = 0; i < static_cast<int>(p.tabs.size()); i++)
		if (p.tabs[i].document == active) p.active = i;
	host.tabsChanged();
}

void Workspace::noteTabUsed() {
	const Document *document = activeDocument();
	if (!document) return;
	if (switcherPosition > 0) return;  // Still cycling: the order settles once it stops.
	mru.erase(std::remove(mru.begin(), mru.end(), document), mru.end());
	mru.insert(mru.begin(), document);
}

void Workspace::activateDocument(const Document *document) {
	for (int pn = 0; pn < 2; pn++) {
		const auto &list = paneAt(pn).tabs;
		for (int i = 0; i < static_cast<int>(list.size()); i++) {
			if (list[i].document.get() == document) {
				focus = pn;
				activate(pn, i);
				host.focusPane(pn);
				return;
			}
		}
	}
}

void Workspace::switchDocument(bool backward) {
	// Forget closed documents.
	std::vector<const Document *> order;
	for (const Document *candidate : mru) {
		for (const auto &document : documents) {
			if (document.get() == candidate) {
				order.push_back(candidate);
				break;
			}
		}
	}
	for (const auto &document : documents)
		if (std::find(order.begin(), order.end(), document.get()) == order.end()) order.push_back(document.get());
	mru = order;
	if (order.size() < 2) return;
	const long count = static_cast<long>(order.size());
	const bool cycling = tickCount - switcherTick <= 2 && switcherPosition > 0;
	if (!cycling) switcherPosition = 0;
	switcherPosition = settings.switcherMru ? (switcherPosition + (backward ? count - 1 : 1)) % count : 1;
	if (switcherPosition == 0) switcherPosition = backward ? count - 1 : 1;
	switcherTick = tickCount;
	activateDocument(order[static_cast<size_t>(switcherPosition)]);
}

void Workspace::showWindowsDialog() {
	for (;;) {
		ListDialog dialog;
		dialog.title = "Windows";
		dialog.columns = {"Name", "Path", "Type", "Size", "Modified time"};
		dialog.buttons = {"Activate", "Save", "Close Window", "Sort by Name", "Done", "Copy Names", "Copy Paths"};
		dialog.multiSelect = true;
		std::vector<std::pair<int, int>> where;
		for (int pn = 0; pn < 2; pn++) {
			const auto &list = paneAt(pn).tabs;
			for (int i = 0; i < static_cast<int>(list.size()); i++) {
				const Document &document = *list[i].document;
				BackgroundDocument background(host, document);
				std::string modified;
				std::error_code timeError;
				if (!document.path.empty()) {
					struct stat info;
					if (stat(document.path.c_str(), &info) == 0) modified = formatDateTime("yyyy-MM-dd HH:mm:ss", info.st_mtime);
				}
				dialog.rows.push_back({(document.dirty() ? "* " : "") + document.title(), document.path.empty() ? std::string() : directoryOf(document.path),
				                       document.language->displayName, std::to_string(background.view.send(SCI_GETLENGTH)), modified});
				where.emplace_back(pn, i);
			}
		}
		std::vector<int> selected;
		const int button = host.runList(dialog, selected);
		if (button < 0 || button == 4) return;
		if (button == 3) {
			sortTabs(0);
			continue;
		}
		if (selected.empty()) continue;
		if (button >= 5) {
			std::string out;
			for (const int row : selected) {
				const Document &document = *paneAt(where[row].first).tabs[where[row].second].document;
				out += button == 6 && !document.path.empty() ? document.path : document.title();
				out += "\n";
			}
			host.setClipboard(out);
			continue;
		}
		if (button == 0) {
			clickTab(where[selected[0]].first, where[selected[0]].second);
			return;
		}
		if (button == 1) {
			for (const int row : selected) {
				const std::shared_ptr<Document> document = paneAt(where[row].first).tabs[where[row].second].document;
				if (document->path.empty()) saveAsDialog(*document);
				else saveDocument(*document, document->path);
			}
			continue;
		}
		// From the end, so the earlier positions stay right.
		std::sort(selected.begin(), selected.end(), std::greater<int>());
		for (const int row : selected) closeTab(where[row].first, where[row].second);
	}
}

#pragma mark - Macros

std::string Workspace::macroFile() const { return (fs::path(settingsDir) / "macros.json").string(); }

void Workspace::loadMacros() {
	savedMacros.clear();
	const json all = json::parse(readWholeFile(macroFile()), nullptr, false);
	if (!all.is_array()) return;
	for (const json &entry : all) {
		if (!entry.is_object()) continue;
		SavedMacro saved;
		saved.name = jsonString(entry, "name");
		if (saved.name.empty() || !entry.contains("steps") || !entry["steps"].is_array()) continue;
		for (const json &step : entry["steps"]) {
			if (!step.is_object()) continue;
			saved.steps.push_back({static_cast<unsigned>(jsonLong(step, "message")), static_cast<uintptr_t>(jsonLong(step, "w")), static_cast<intptr_t>(jsonLong(step, "l")), jsonString(step, "text")});
		}
		savedMacros.push_back(std::move(saved));
	}
}

void Workspace::saveMacros() const {
	json all = json::array();
	for (const SavedMacro &saved : savedMacros) {
		json steps = json::array();
		for (const MacroStep &step : saved.steps) {
			json out;
			out["message"] = step.message;
			out["w"] = static_cast<long>(step.w);
			out["l"] = static_cast<long>(step.l);
			out["text"] = step.text;
			steps.push_back(out);
		}
		json entry;
		entry["name"] = saved.name;
		entry["steps"] = steps;
		all.push_back(entry);
	}
	std::string error;
	atomicWrite(all.dump(1, '\t', false, json::error_handler_t::replace), macroFile(), 0644, error);
}

void Workspace::saveCurrentMacro() {
	if (macro.empty()) {
		host.flash("There's no recorded macro to save");
		return;
	}
	std::string name;
	if (!host.promptText("Save Macro", "Name:", "", name) || name.empty()) return;
	savedMacros.erase(std::remove_if(savedMacros.begin(), savedMacros.end(), [&](const SavedMacro &m) { return m.name == name; }), savedMacros.end());
	savedMacros.push_back({name, macro});
	saveMacros();
	host.menusChanged();
}

void Workspace::playSteps(const std::vector<MacroStep> &steps) {
	s(SCI_BEGINUNDOACTION);
	for (const MacroStep &step : steps) s(step.message, step.w, step.text.empty() ? step.l : reinterpret_cast<intptr_t>(step.text.c_str()));
	s(SCI_ENDUNDOACTION);
}

void Workspace::runMacroMultiple() {
	std::vector<FormField> fields;
	FormField which = formField(FormField::Kind::Choice, "macro", "Macro", "");
	which.choices.emplace_back("", "Current recorded macro");
	for (const SavedMacro &saved : savedMacros) which.choices.emplace_back(saved.name, saved.name);
	fields.push_back(which);
	FormField mode = formField(FormField::Kind::Choice, "mode", "Run", "times");
	mode.choices = {{"times", "This many times"}, {"end", "Until the end of the file"}};
	fields.push_back(mode);
	FormField times = formField(FormField::Kind::Int, "times", "Times", "1");
	times.minimum = 1;
	times.maximum = 100000;
	fields.push_back(times);
	if (!host.runForm("Run a Macro Multiple Times", fields)) return;
	const std::string name = fieldValue(fields, "macro");
	const std::vector<MacroStep> *steps = &macro;
	for (const SavedMacro &saved : savedMacros)
		if (saved.name == name) steps = &saved.steps;
	if (steps->empty()) {
		host.flash("There's no macro to run");
		return;
	}
	if (fieldValue(fields, "mode") == "end") {
		// Stops when it runs out of file or stops moving, so a macro can't loop for ever.
		long previous = -1;
		while (s(SCI_LINEFROMPOSITION, s(SCI_GETCURRENTPOS)) < s(SCI_GETLINECOUNT) - 1) {
			const long now = s(SCI_GETCURRENTPOS);
			if (now == previous) break;
			previous = now;
			playSteps(*steps);
		}
	} else {
		const long count = std::strtol(fieldValue(fields, "times").c_str(), nullptr, 10);
		for (long i = 0; i < count; i++) playSteps(*steps);
	}
}

void Workspace::manageMacros() {
	for (;;) {
		ListDialog dialog;
		dialog.title = "Modify or Delete Macros";
		dialog.columns = {"Name", "Steps"};
		dialog.buttons = {"Run", "Rename", "Delete", "Done"};
		for (const SavedMacro &saved : savedMacros) dialog.rows.push_back({saved.name, std::to_string(saved.steps.size())});
		std::vector<int> selected;
		const int button = host.runList(dialog, selected);
		if (button < 0 || button == 3) return;
		if (selected.empty()) continue;
		SavedMacro &target = savedMacros[static_cast<size_t>(selected[0])];
		if (button == 0) {
			playSteps(target.steps);
			return;
		}
		if (button == 1) {
			std::string name;
			if (host.promptText("Rename Macro", "Name:", target.name, name) && !name.empty()) target.name = name;
		} else {
			savedMacros.erase(savedMacros.begin() + selected[0]);
		}
		saveMacros();
		host.menusChanged();
	}
}

#pragma mark - Run

std::string Workspace::expandRunVariables(const std::string &command) const {
	const Document *document = activeDocument();
	std::string path = document ? document->path : std::string();
	std::string out = command;
	const fs::path p(path);
	out = replaceEvery(out, "$(FULL_CURRENT_PATH)", path);
	out = replaceEvery(out, "$(CURRENT_DIRECTORY)", path.empty() ? std::string() : directoryOf(path));
	out = replaceEvery(out, "$(FILE_NAME)", document ? document->title() : std::string());
	out = replaceEvery(out, "$(NAME_PART)", fs::path(document ? document->title() : std::string()).stem().string());
	out = replaceEvery(out, "$(EXT_PART)", p.extension().string());
	out = replaceEvery(out, "$(CURRENT_WORD)", selectionOrWord());
	const long position = s(SCI_GETCURRENTPOS);
	out = replaceEvery(out, "$(CURRENT_LINE)", std::to_string(s(SCI_LINEFROMPOSITION, position) + 1));
	out = replaceEvery(out, "$(CURRENT_COLUMN)", std::to_string(s(SCI_GETCOLUMN, position) + 1));
	const std::string executable = currentExecutablePath();
	out = replaceEvery(out, "$(NPP_DIRECTORY)", directoryOf(executable));
	out = replaceEvery(out, "$(NPP_FULL_FILE_PATH)", executable);
	out = replaceEvery(out, "$(SETTINGS_DIRECTORY)", settingsDir);
	// $(SYS.NAME) is an environment variable.
	for (size_t at = out.find("$(SYS."); at != std::string::npos; at = out.find("$(SYS.", at + 1)) {
		const size_t end = out.find(')', at);
		if (end == std::string::npos) break;
		const char *value = getenv(out.substr(at + 6, end - at - 6).c_str());
		out.replace(at, end - at + 1, value ? value : "");
	}
	return out;
}

void Workspace::runCommandLine(const std::string &command) {
	const Document *document = activeDocument();
	const std::string directory = document && !document->path.empty() ? directoryOf(document->path) : std::string();
	host.launch({"/bin/sh", "-c", expandRunVariables(command)}, directory);
}

std::string Workspace::runCommandFile() const { return (fs::path(settingsDir) / "runCommands.json").string(); }

void Workspace::loadRunCommands() {
	runCommands.clear();
	const json all = json::parse(readWholeFile(runCommandFile()), nullptr, false);
	if (!all.is_array()) return;
	for (const json &entry : all)
		if (entry.is_object() && !jsonString(entry, "name").empty()) runCommands.push_back({jsonString(entry, "name"), jsonString(entry, "command")});
}

void Workspace::saveRunCommands() const {
	json all = json::array();
	for (const auto &command : runCommands) {
		json entry;
		entry["name"] = command.first;
		entry["command"] = command.second;
		all.push_back(entry);
	}
	std::string error;
	atomicWrite(all.dump(1, '\t', false, json::error_handler_t::replace), runCommandFile(), 0644, error);
}

bool Workspace::deleteShortcutCommand(const std::string &id) {
	const std::string name = baseName(id);
	if (id.rfind("Macro/", 0) == 0) {
		const auto before = savedMacros.size();
		savedMacros.erase(std::remove_if(savedMacros.begin(), savedMacros.end(), [&](const SavedMacro &macro) { return macro.name == name; }), savedMacros.end());
		if (savedMacros.size() == before) return false;
		saveMacros();
	} else if (id.rfind("Run/", 0) == 0) {
		const auto before = runCommands.size();
		runCommands.erase(std::remove_if(runCommands.begin(), runCommands.end(), [&](const auto &command) { return command.first == name; }), runCommands.end());
		if (runCommands.size() == before) return false;
		saveRunCommands();
	} else {
		return false;
	}
	settings.shortcuts.erase(id);
	saveSettings();
	host.menusChanged();
	return true;
}

void Workspace::runDialog() {
	// Earlier commands come back through a list, and "browse" fills the field from a file picker.
	std::vector<std::string> history;
	{
		std::istringstream lines(settings.pref("runHistory"));
		std::string line;
		while (std::getline(lines, line))
			if (!line.empty()) history.push_back(line);
	}
	std::vector<FormField> fields;
	fields.push_back(formField(FormField::Kind::Text, "command", "The program to run", lastRunCommand));
	if (!history.empty()) {
		FormField earlier = formField(FormField::Kind::Choice, "history", "Or an earlier one", "");
		earlier.choices.emplace_back("", "");
		for (const std::string &command : history) earlier.choices.emplace_back(command, command);
		fields.push_back(earlier);
	}
	fields.push_back(formField(FormField::Kind::Bool, "browse", "Browse for the program first", "0"));
	fields.push_back(formField(FormField::Kind::Heading, "", "Variables: $(FULL_CURRENT_PATH) $(CURRENT_DIRECTORY) $(FILE_NAME) $(NAME_PART) $(EXT_PART) $(CURRENT_WORD) $(CURRENT_LINE) $(CURRENT_COLUMN) $(SYS.NAME)"));
	fields.push_back(formField(FormField::Kind::Text, "save", "Save as, with this name (optional)"));
	if (!host.runForm("Run", fields)) return;
	lastRunCommand = fieldValue(fields, "command");
	if (!fieldValue(fields, "history").empty()) lastRunCommand = fieldValue(fields, "history");
	if (fieldValue(fields, "browse") == "1") {
		std::vector<std::string> paths;
		if (!host.chooseFilesToOpen(dialogDirectory(), paths) || paths.empty()) return;
		lastRunCommand = "\"" + paths[0] + "\"";
		runDialog();
		return;
	}
	if (lastRunCommand.empty()) return;
	history.erase(std::remove(history.begin(), history.end(), lastRunCommand), history.end());
	history.insert(history.begin(), lastRunCommand);
	if (history.size() > 10) history.resize(10);
	{
		std::string joined;
		for (const std::string &command : history) joined += command + "\n";
		settings.prefs["runHistory"] = joined;
		saveSettings();
	}
	const std::string name = fieldValue(fields, "save");
	if (!name.empty()) {
		runCommands.erase(std::remove_if(runCommands.begin(), runCommands.end(), [&](const auto &c) { return c.first == name; }), runCommands.end());
		runCommands.emplace_back(name, lastRunCommand);
		saveRunCommands();
		host.menusChanged();
	}
	runCommandLine(lastRunCommand);
}

void Workspace::manageRunCommands() {
	for (;;) {
		ListDialog dialog;
		dialog.title = "Modify or Delete Commands";
		dialog.columns = {"Name", "Command"};
		dialog.buttons = {"Run", "Edit", "Delete", "Done"};
		for (const auto &command : runCommands) dialog.rows.push_back({command.first, command.second});
		std::vector<int> selected;
		const int button = host.runList(dialog, selected);
		if (button < 0 || button == 3) return;
		if (selected.empty()) continue;
		auto &target = runCommands[static_cast<size_t>(selected[0])];
		if (button == 0) {
			runCommandLine(target.second);
			return;
		}
		if (button == 1) {
			std::vector<FormField> fields;
			fields.push_back(formField(FormField::Kind::Text, "name", "Name", target.first));
			fields.push_back(formField(FormField::Kind::Text, "command", "Command", target.second));
			if (host.runForm("Edit Command", fields)) {
				target.first = fieldValue(fields, "name");
				target.second = fieldValue(fields, "command");
			}
		} else {
			runCommands.erase(runCommands.begin() + selected[0]);
		}
		saveRunCommands();
		host.menusChanged();
	}
}

void Workspace::validateShortcuts() {
	std::map<std::string, std::vector<std::string>> used;
	for (const ShortcutEntry &entry : shortcutEntries())
		if (!entry.shortcut.empty() && entry.id.rfind("Scintilla/", 0) != 0) used[entry.shortcut].push_back(entry.id);
	std::string report;
	for (const auto &key : used) {
		if (key.second.size() < 2) continue;
		report += key.first + ":";
		for (const std::string &id : key.second) report += "\n    " + id;
		report += "\n";
	}
	host.ask(report.empty() ? "No shortcut is used twice." : "Some shortcuts are used more than once.", report, {"OK"});
}

#pragma mark - Hash tools

void Workspace::hashDialog(HashKind kind) {
	std::vector<FormField> fields;
	fields.push_back(formField(FormField::Kind::LongText, "text", "Text to hash"));
	fields.push_back(formField(FormField::Kind::Bool, "perLine", "Treat each line as a separate string", "0"));
	if (!host.runForm("Generate Hash", fields)) return;
	const std::string text = fieldValue(fields, "text");
	std::string out;
	if (fieldValue(fields, "perLine") == "1") {
		std::stringstream lines(text);
		std::string line;
		while (std::getline(lines, line)) out += hashHex(line.data(), line.size(), kind) + "  " + line + "\n";
	} else {
		out = hashHex(text.data(), text.size(), kind);
	}
	host.setClipboard(out);
	host.ask("Copied to the clipboard", out, {"OK"});
}

#pragma mark - Help

std::string Workspace::debugInfo() const {
	std::string out = "omapad " OMAPAD_VERSION "\n";
#ifdef __APPLE__
	out += "Platform: macOS, AppKit\n";
#else
	out += "Platform: Linux, GTK 3\n";
#endif
	out += "Executable: " + currentExecutablePath() + "\n";
	out += "Settings: " + settingsDir + "\n";
	out += "State: " + stateDir + "\n";
	out += "Theme: " + (effectiveTheme().empty() ? std::string("Default") : effectiveTheme()) + "\n";
	out += "Font: " + settings.fontName + " " + std::to_string(settings.fontSize) + "\n";
	out += "Open documents: " + std::to_string(documents.size()) + "\n";
	return out;
}

void Workspace::checkForUpdates() {
	if (!commandExists("curl")) {
		openExternally("https://github.com/andysolomon/omapad/releases");
		return;
	}
	std::string command = "curl -fsSL -m 10 ";
	if (!settings.updateProxy.empty()) {
		std::string quoted;
		for (const char c : settings.updateProxy) {
			if (c == '\'') quoted += "'\\''";
			else quoted += c;
		}
		command += "-x '" + quoted + "' ";
	}
	command += "https://api.github.com/repos/andysolomon/omapad/releases/latest 2>/dev/null";
	std::string reply;
	if (FILE *pipe = popen(command.c_str(), "r")) {
		char buffer[4096];
		size_t got;
		while ((got = fread(buffer, 1, sizeof buffer, pipe)) > 0) reply.append(buffer, got);
		pclose(pipe);
	}
	const json release = json::parse(reply, nullptr, false);
	const std::string tag = release.is_object() ? jsonString(release, "tag_name") : std::string();
	if (tag.empty()) {
		host.ask("No release is available to compare with.", "You're running omapad " OMAPAD_VERSION ". This needs a published GitHub release and a network connection.", {"OK"});
	} else if (tag == "v" OMAPAD_VERSION || tag == OMAPAD_VERSION) {
		host.ask("omapad is up to date.", "You're running " OMAPAD_VERSION ".", {"OK"});
	} else if (host.ask("omapad " + tag + " is available.", "You're running " OMAPAD_VERSION ".", {"Later", "Open the Releases Page"}) == 1) {
		openExternally("https://github.com/andysolomon/omapad/releases");
	}
}

namespace {
struct QuoteEntry { const char *name; int speed; const char *text; };
const QuoteEntry kQuotes[] = {
#include "Quotes.inc"
};
const std::pair<const char *, const char *> kLocales[] = {
#include "Locales.inc"
};
CommandLine gLaunch;
}  // namespace

void setLaunchOptions(const CommandLine &line) { gLaunch = line; }
const CommandLine &launchOptions() { return gLaunch; }

std::string programDirectory() {
	std::string path = currentExecutablePath();
	// In a bundle, "next to the program" means next to omapad.app.
	const size_t bundle = path.find(".app/Contents/MacOS/");
	if (bundle != std::string::npos) path = path.substr(0, bundle + 4);
	return directoryOf(path);
}

void resolveConfigDirectories(std::string &settingsDirectory, std::string &stateDirectory) {
	std::error_code error;
	if (!gLaunch.settingsDirectory.empty()) {
		settingsDirectory = stateDirectory = gLaunch.settingsDirectory;
	} else if (fs::exists(fs::path(programDirectory()) / "doLocalConf.xml", error) && access(programDirectory().c_str(), W_OK) == 0) {
		// A portable copy keeps everything beside itself.
		settingsDirectory = stateDirectory = programDirectory();
	}
}

const char *commandLineHelp() {
	return "omapad [--help] [-multiInst] [-noPlugin] [-lLanguage] [-udl=\"My UDL Name\"]\n"
	       "[-LlangCode] [-nLineNumber] [-cColumnNumber] [-pPosition] [-xLeftPos] [-yTopPos]\n"
	       "[-monitor] [-monitoringMode] [-nosession] [-notabbar] [-systemtray] [-loadingTime]\n"
	       "[-alwaysOnTop] [-ro] [-fullReadOnly] [-fullReadOnlySavingForbidden] [-openSession] [-r]\n"
	       "[-qn=\"Easter egg name\" | -qt=\"a text to display.\" | -qf=\"my quote.txt\"]\n"
	       "[-qSpeed1|2|3] [-quickPrint] [-settingsDir=\"your settings dir\"]\n"
	       "[-openFoldersAsWorkspace] [-titleAdd=\"additional title bar text\"] [filePath]\n\n"
	       "--help: This help message\n"
	       "-multiInst: Launch another omapad instance\n"
	       "-noPlugin: Launch without loading any plugin\n"
	       "-l: Open file or ghost type with syntax highlighting of choice\n"
	       "-udl=\"My UDL Name\": Open file by applying a user-defined language\n"
	       "-L: Apply indicated localization, langCode is a browser language code\n"
	       "-n, -c, -p: Scroll to indicated line, column or position\n"
	       "-x, -y: Move the window to this left side or top position\n"
	       "-monitor: Open files given with file monitoring on\n"
	       "-monitoringMode: Monitor all files opened\n"
	       "-nosession: Launch without the previous session\n"
	       "-notabbar: Launch without the tab bar\n"
	       "-ro: Make the file read-only\n"
	       "-fullReadOnly: Open all files read-only, which can be turned off\n"
	       "-fullReadOnlySavingForbidden: Open all files read-only, and saving is disabled\n"
	       "-systemtray: Launch in the system tray (Mac)\n"
	       "-loadingTime: Display the loading time\n"
	       "-alwaysOnTop: Keep the window on top\n"
	       "-openSession: Open a session. filePath must be a session file\n"
	       "-r: Open files recursively, when filePath has a wildcard\n"
	       "-qn: Ghost type an easter egg by name\n"
	       "-qt: Ghost type the given text\n"
	       "-qf: Ghost type a file's content\n"
	       "-qSpeed: Ghost typing speed from 1 to 3 for slow, fast and fastest\n"
	       "-quickPrint: Print the file given and quit\n"
	       "-settingsDir: Override the settings folder\n"
	       "-openFoldersAsWorkspace: Open folders as Folder as Workspace\n"
	       "-titleAdd: Add text to the title bar\n"
	       "filePath: file or folder to open\n";
}

#pragma mark - Command line

CommandLine parseCommandLine(const std::vector<std::string> &arguments) {
	CommandLine out;
	for (const std::string &argument : arguments) {
		const auto valueAfter = [&](const char *prefix) { return argument.substr(std::strlen(prefix)); };
		const auto starts = [&](const char *prefix) { return argument.rfind(prefix, 0) == 0; };
		if (argument == "--help") out.help = true;
		else if (argument == "-multiInst") out.multiInstance = true;
		else if (argument == "-noPlugin") out.noPlugin = true;
		else if (argument == "-monitoringMode") out.monitoringMode = true;
		else if (argument == "-fullReadOnlySavingForbidden") { out.fullReadOnly = true; out.savingForbidden = true; }
		else if (argument == "-systemtray") out.systemTray = true;
		else if (argument == "-loadingTime") out.loadingTime = true;
		else if (argument == "-r") out.recursive = true;
		else if (starts("-settingsDir=")) out.settingsDirectory = valueAfter("-settingsDir=");
		else if (starts("-titleAdd=")) out.titleAdd = valueAfter("-titleAdd=");
		else if (starts("-pluginMessage=")) out.pluginMessage = valueAfter("-pluginMessage=");
		else if (starts("-qn=")) out.ghostName = valueAfter("-qn=");
		else if (starts("-qt=")) out.ghostText = valueAfter("-qt=");
		else if (starts("-qf=")) out.ghostFile = valueAfter("-qf=");
		else if (starts("-qSpeed") && argument.size() > 7) out.ghostSpeed = std::max(0, std::min(2, argument[7] - '1'));
		else if (argument == "-notepadStyleCmdline") {}
		else if (starts("-L") && argument.size() > 2) out.localeCode = valueAfter("-L");
		else if (starts("-x") && argument.size() > 2 && std::isdigit(static_cast<unsigned char>(argument[2]))) out.windowX = std::strtol(argument.c_str() + 2, nullptr, 10);
		else if (starts("-y") && argument.size() > 2 && std::isdigit(static_cast<unsigned char>(argument[2]))) out.windowY = std::strtol(argument.c_str() + 2, nullptr, 10);
		else if (argument == "-multiInst") out.multiInstance = true;
		else if (argument == "-nosession") out.noSession = true;
		else if (argument == "-notabbar") out.noTabBar = true;
		else if (argument == "-ro") out.readOnly = true;
		else if (argument == "-fullReadOnly") out.fullReadOnly = true;
		else if (argument == "-monitor") out.monitor = true;
		else if (argument == "-alwaysOnTop") out.alwaysOnTop = true;
		else if (argument == "-openSession") out.openSession = true;
		else if (argument == "-openFoldersAsWorkspace") out.foldersAsWorkspace = true;
		else if (argument == "-quickPrint") out.quickPrint = true;
		else if (starts("-udl=")) out.language = "udl:" + valueAfter("-udl=");
		else if (starts("-n") && argument.size() > 2 && std::isdigit(static_cast<unsigned char>(argument[2]))) out.line = std::strtol(argument.c_str() + 2, nullptr, 10);
		else if (starts("-c") && argument.size() > 2 && std::isdigit(static_cast<unsigned char>(argument[2]))) out.column = std::strtol(argument.c_str() + 2, nullptr, 10);
		else if (starts("-p") && argument.size() > 2 && std::isdigit(static_cast<unsigned char>(argument[2]))) out.position = std::strtol(argument.c_str() + 2, nullptr, 10);
		else if (starts("-l") && argument.size() > 2) out.language = valueAfter("-l");
		else if (!argument.empty() && argument[0] == '-' && argument.size() > 1) out.ignored.push_back(argument);
		else out.files.push_back(argument);
	}
	return out;
}

void Workspace::prepareCommandLine(const CommandLine &line) {
	std::error_code error;
	const bool likeNotepad = fs::exists(fs::path(programDirectory()) / "asNotepad.xml", error);
	if (line.noSession || likeNotepad) sessionDisabled = true;
	if (likeNotepad) tabsHiddenByCommandLine = true;
	if (line.monitoringMode) allFilesMonitored = true;
	if (line.fullReadOnly) allFilesReadOnly = true;
	if (line.savingForbidden) savingForbidden = true;
	titleSuffix = line.titleAdd;
	if (!line.localeCode.empty()) {
		std::string code = lower(line.localeCode);
		std::replace(code.begin(), code.end(), '_', '-');
		for (const auto &locale : kLocales) {
			if (code == locale.first) {
				settings.prefs["uiLanguage"] = locale.second;
				loadMenuLanguage();
				host.menusChanged();
			}
		}
	}
}

void Workspace::applyCommandLine(const CommandLine &line) {
	prepareCommandLine(line);
	const auto startedAt = std::chrono::steady_clock::now();
	if (line.alwaysOnTop) {
		alwaysOnTop = true;
		host.setAlwaysOnTop(true);
	}
	if (line.windowX >= 0 || line.windowY >= 0) host.setWindowPosition(line.windowX, line.windowY);
	if (line.systemTray) host.setTray(true);
	if (!line.pluginMessage.empty()) notifyPlugins("commandLine", line.pluginMessage);
	if (line.noTabBar) {
		tabsHiddenByCommandLine = true;
		applyChromeState();
	}
	for (const std::string &path : line.files) {
		std::error_code error;
		const std::string full = expandPath(path);
		if (line.openSession) {
			loadSessionFile(full);
			continue;
		}
		if (path.find_first_of("*?") != std::string::npos) {
			openWildcard(full, line.recursive);
			continue;
		}
		if (fs::is_directory(full, error)) {
			if (line.foldersAsWorkspace || !settings.folderDropOpensFiles) {
				addFolder(full);
				setFolderPanelVisible(true);
			} else {
				for (const auto &entry : fs::directory_iterator(full, error))
					if (entry.is_regular_file(error)) openPath(entry.path().string());
			}
			continue;
		}
		offerCreatingMissing = true;
		const bool opened = openPath(full);
		offerCreatingMissing = false;
		if (!opened) continue;
		if (!line.language.empty()) setActiveLanguage(line.language);
		if (line.readOnly || line.fullReadOnly) s(SCI_SETREADONLY, 1);
		if (line.fullReadOnly) {
			struct stat info;
			if (stat(full.c_str(), &info) == 0 && (info.st_mode & S_IWUSR)) toggleFileReadOnly();
		}
		if (line.monitor) toggleMonitoring();
	}
	if (!line.files.empty() && activeDocument()) {
		if (line.position >= 0) {
			s(SCI_GOTOPOS, std::min<long>(line.position, s(SCI_GETLENGTH)));
		} else if (line.line > 0) {
			const long target = std::min<long>(line.line - 1, s(SCI_GETLINECOUNT) - 1);
			s(SCI_ENSUREVISIBLEENFORCEPOLICY, target);
			s(SCI_GOTOLINE, target);
			if (line.column > 0) s(SCI_GOTOPOS, s(SCI_FINDCOLUMN, target, line.column - 1));
		}
		if (line.quickPrint) {
			PrintOutput output;
			output.showDialog = false;
			printActive(false, output);
			host.quit();
		}
	}
	if (!line.ghostName.empty() || !line.ghostText.empty() || !line.ghostFile.empty()) {
		std::string text = line.ghostText;
		int speed = line.ghostSpeed >= 0 ? line.ghostSpeed : 1;
		if (!line.ghostFile.empty()) text = readWholeFile(expandPath(line.ghostFile));
		if (!line.ghostName.empty()) {
			const size_t count = sizeof kQuotes / sizeof kQuotes[0];
			if (lower(line.ghostName) == "random") {
				const QuoteEntry &quote = kQuotes[static_cast<size_t>(time(nullptr)) % count];
				text = quote.text;
			}
			for (const QuoteEntry &quote : kQuotes) {
				if (lower(quote.name) == lower(line.ghostName)) {
					text = quote.text;
					if (line.ghostSpeed < 0) speed = quote.speed;
				}
			}
		}
		if (!text.empty()) {
			createUntitledDocument();
			if (!line.language.empty()) setActiveLanguage(line.language);
			ghostType(text, speed);
		}
	}
	if (line.loadingTime) {
		const auto taken = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt).count();
		host.ask("Loading time", std::to_string(taken) + " ms", {"OK"});
	}
}

#pragma mark - Ghost typing, helpers

void Workspace::ghostTypeNext(std::shared_ptr<std::u32string> text, size_t at, int speed) {
	if (at >= text->size() || !activeDocument()) return;
	std::string one;
	appendUtf8(one, (*text)[at]);
	s(SCI_ADDTEXT, one.size(), reinterpret_cast<intptr_t>(one.data()));
	s(SCI_SCROLLCARET);
	static const int delays[] = {70, 25, 4};
	const int delay = delays[std::max(0, std::min(2, speed))];
	std::shared_ptr<bool> guard = alive;
	host.post([this, guard, text, at, speed, delay]() {
		if (!*guard) return;
		usleep(static_cast<useconds_t>(delay * 1000));
		ghostTypeNext(text, at + 1, speed);
	});
}

void Workspace::ghostType(const std::string &text, int speed) {
	ghostTypeNext(std::make_shared<std::u32string>(utf8ToCodePoints(text)), 0, speed);
}

bool Workspace::aboutEasterEgg() {
	const std::string selected = selectedText();
	if (selected.empty()) return false;
	std::error_code error;
	if (fs::exists(fs::path(programDirectory()) / "noEasterEggs.xml", error)) return false;
	const std::string name = lower(selected);
	if (name == "get them all!!!") {
		std::string all;
		for (const QuoteEntry &quote : kQuotes) all += std::string(quote.name) + ":\n" + quote.text + "\n\n";
		createUntitledDocument();
		ghostType(all, 2);
		return true;
	}
	const size_t count = sizeof kQuotes / sizeof kQuotes[0];
	const QuoteEntry *chosen = name == "random" ? &kQuotes[static_cast<size_t>(time(nullptr)) % count] : nullptr;
	for (const QuoteEntry &quote : kQuotes)
		if (lower(quote.name) == name) chosen = &quote;
	if (!chosen) return false;
	createUntitledDocument();
	ghostType(chosen->text, chosen->speed);
	return true;
}

void Workspace::openWildcard(const std::string &pattern, bool recursive) {
	const fs::path full(pattern);
	const std::string directory = full.parent_path().string(), wildcard = full.filename().string();
	std::vector<std::string> paths;
	std::error_code error;
	const auto consider = [&](const fs::directory_entry &entry) {
		std::error_code entryError;
		if (entry.is_regular_file(entryError) && fnmatch(wildcard.c_str(), entry.path().filename().c_str(), 0) == 0) paths.push_back(entry.path().string());
	};
	if (recursive) {
		for (const auto &entry : fs::recursive_directory_iterator(directory, fs::directory_options::skip_permission_denied, error)) consider(entry);
	} else {
		for (const auto &entry : fs::directory_iterator(directory, fs::directory_options::skip_permission_denied, error)) consider(entry);
	}
	std::sort(paths.begin(), paths.end());
	openFiles(paths);
}

std::string Workspace::dialogDirectory() const {
	const std::string mode = settings.pref("defaultDirMode");
	if (mode == "custom" && !settings.pref("defaultDir").empty()) return expandPath(settings.pref("defaultDir"));
	if (mode == "remember" && !lastDialogDirectory.empty()) return lastDialogDirectory;
	const Document *document = activeDocument();
	return document && !document->path.empty() ? directoryOf(document->path) : std::string();
}

void Workspace::notifyPlugins(const char *event, const std::string &path) {
	for (const LoadedPlugin &plugin : plugins)
		if (plugin.notify) plugin.notify(event, path.c_str());
}

bool Workspace::saveWithAdminRights(const std::string &bytes, const std::string &path, std::string &error) {
	// The text goes to a private file first, and only a copy runs with raised rights.
	std::string temporary = (fs::temp_directory_path() / "omapad-admin-XXXXXX").string();
	const int fd = mkstemp(temporary.data());
	if (fd < 0) {
		error = std::strerror(errno);
		return false;
	}
	const ssize_t written = write(fd, bytes.data(), bytes.size());
	close(fd);
	if (written != static_cast<ssize_t>(bytes.size())) {
		unlink(temporary.c_str());
		error = "The temporary copy could not be written.";
		return false;
	}
	const auto quoted = [](const std::string &value) {
		std::string out = "'";
		for (const char c : value) {
			if (c == '\'') out += "'\\''";
			else out += c;
		}
		return out + "'";
	};
	int status = -1;
	const std::string copy = "/bin/cp " + quoted(temporary) + " " + quoted(path);
#ifdef __APPLE__
	std::string script;
	for (const char c : copy) {
		if (c == '\\' || c == '"') script += '\\';
		script += c;
	}
	const std::string command = "/usr/bin/osascript -e " + quoted("do shell script \"" + script + "\" with administrator privileges");
#else
	const std::string command = "pkexec sh -c " + quoted(copy);
#endif
	status = system(command.c_str());
	unlink(temporary.c_str());
	if (status != 0) {
		error = "The copy with administrator rights didn't complete.";
		return false;
	}
	error.clear();
	return true;
}

void Workspace::findInResultsDialog() {
	std::vector<FormField> fields;
	fields.push_back(formField(FormField::Kind::Text, "what", "Find what"));
	fields.push_back(formField(FormField::Kind::Bool, "onlyFound", "Search only in found lines", "1"));
	fields.push_back(formField(FormField::Kind::Bool, "word", "Match whole word only", "0"));
	fields.push_back(formField(FormField::Kind::Bool, "case", "Match case", "0"));
	FormField mode = formField(FormField::Kind::Choice, "mode", "Search mode", "0");
	mode.choices = {{"0", "Normal"}, {"1", "Extended (\\n, \\r, \\t, \\0, \\x...)"}, {"2", "Regular expression"}};
	fields.push_back(mode);
	fields.push_back(formField(FormField::Kind::Bool, "dot", ". matches newline", "0"));
	if (!host.runForm("Find in Search Results", fields)) return;
	FindOptions options;
	options.what = fieldValue(fields, "what");
	options.matchCase = fieldValue(fields, "case") == "1";
	options.wholeWord = fieldValue(fields, "word") == "1";
	options.mode = static_cast<SearchMode>(std::atoi(fieldValue(fields, "mode").c_str()));
	options.dotMatchesNewline = fieldValue(fields, "dot") == "1";
	searchInResults(options, fieldValue(fields, "onlyFound") == "1");
}

void Workspace::fileAssociationDialog() {
	std::vector<FormField> fields;
	fields.push_back(formField(FormField::Kind::Heading, "", "omapad becomes the default program for these file types."));
	fields.push_back(formField(FormField::Kind::LongText, "extensions", "Extensions (like: txt md py)"));
	if (!host.runForm("File Association", fields)) return;
	std::vector<std::string> extensions;
	std::istringstream words(fieldValue(fields, "extensions"));
	std::string word;
	while (words >> word) {
		if (word[0] == '.') word.erase(0, 1);
		bool safe = !word.empty();
		for (const unsigned char c : word) safe = safe && (std::isalnum(c) || c == '+' || c == '-' || c == '_');
		if (safe) extensions.push_back(word);
	}
	if (extensions.empty()) return;
	std::string error;
	if (host.registerFileTypes(extensions, error)) host.flash("omapad now opens those file types");
	else host.ask("The file types could not be registered.", error, {"OK"});
}

#pragma mark - Dispatch

bool Workspace::runFeature(Cmd cmd, int arg, const std::string &argument) {
	Document *document = activeDocument();
	const std::string documentPath = document ? document->path : std::string();
	const std::string documentDirectory = documentPath.empty() ? std::string() : directoryOf(documentPath);
	if (runPanelCommand(cmd, arg, argument)) return true;
	switch (cmd) {
	case Cmd::OpenTerminal: if (!documentDirectory.empty()) openTerminalAt(documentDirectory); return true;
	case Cmd::RestoreClosed: {
		if (recentlyClosed.empty()) return true;
		const std::string path = recentlyClosed.back();
		recentlyClosed.pop_back();
		openPath(path);
		return true;
	}
	case Cmd::FocusPanel:
		if (arg < kPanelCount && !panelVisible(arg)) togglePanel(arg);
		if (arg == kPanelFunctions && !functionPanel) run(Cmd::ToggleFunctionList);
		if (arg == kPanelFolders && !folderPanel) run(Cmd::ToggleFolderPanel);
		host.focusPanel(arg);
		return true;
	case Cmd::FileAssociation: fileAssociationDialog(); return true;
	case Cmd::FolderOpen: {
		std::error_code folderError;
		if (fs::is_directory(argument, folderError)) setFolderExpanded(argument, true);
		else openPath(argument);
		return true;
	}
	case Cmd::FolderRunBySystem: openExternally(argument); return true;
	case Cmd::FolderCopyName: host.setClipboard(baseName(argument)); return true;
	case Cmd::FolderTerminal: openTerminalAt(argument); return true;
	case Cmd::FolderExpandAll: {
		std::function<void(const std::string &, int)> expand = [&](const std::string &folder, int depth) {
			setFolderExpanded(folder, true);
			if (depth > 6) return;
			for (const FolderEntry &entry : listFolder(folder))
				if (entry.isDirectory) expand(entry.path, depth + 1);
		};
		for (const std::string &root : folderRoots) expand(root, 0);
		host.foldersChanged();
		return true;
	}
	case Cmd::FolderCollapseAll:
		expandedFolders.clear();
		host.foldersChanged();
		return true;
	case Cmd::FolderLocate: {
		if (documentPath.empty()) return true;
		for (const std::string &root : folderRoots) {
			if (documentPath.rfind(root + "/", 0) != 0) continue;
			setFolderExpanded(root, true);
			fs::path walk(root);
			for (const auto &part : fs::path(documentPath.substr(root.size() + 1)).parent_path()) {
				walk /= part;
				setFolderExpanded(walk.string(), true);
			}
			host.foldersChanged();
			host.selectFolderPath(documentPath);
			return true;
		}
		host.flash("That file isn't in the workspace");
		return true;
	}
	case Cmd::FolderRemoveAll: {
		const std::vector<std::string> roots = folderRoots;
		for (const std::string &root : roots) removeFolder(root);
		return true;
	}
	case Cmd::OpenFolderOfFile:
		if (!documentDirectory.empty()) {
			addFolder(documentDirectory);
			setFolderPanelVisible(true);
		}
		return true;
	case Cmd::OpenDefaultViewer: if (!documentPath.empty()) openExternally(documentPath); return true;
	case Cmd::CloseUnpinned: {
		for (int pn = 1; pn >= 0; pn--) {
			for (int i = static_cast<int>(paneAt(pn).tabs.size()) - 1; i >= 0; i--) {
				if (i >= static_cast<int>(paneAt(pn).tabs.size())) continue;
				if (!paneAt(pn).tabs[i].pinned && !closeTab(pn, i)) return true;
			}
		}
		return true;
	}
	case Cmd::TabPin: {
		const int pane = arg / 10000, index = arg % 10000;
		const Pane &p = paneAt(pane);
		if (index < static_cast<int>(p.tabs.size())) setTabPinned(pane, index, !p.tabs[index].pinned);
		return true;
	}
	case Cmd::PinActive: {
		const Tab *tab = activeTabPointer();
		if (tab) setTabPinned(focus, paneAt(focus).active, !tab->pinned);
		return true;
	}
	case Cmd::LoadSession: {
		std::vector<std::string> paths;
		if (host.chooseFilesToOpen(dialogDirectory(), paths))
			for (const std::string &path : paths)
				if (settings.pref("instanceMode") == "session") host.launch({currentExecutablePath(), "-multiInst", "-nosession", "-openSession", path}, std::string());
				else
				if (!loadSessionFile(path)) host.ask("“" + baseName(path) + "” isn't a session file.", "", {"OK"});
		return true;
	}
	case Cmd::SaveSession: {
		std::string path, error;
		if (host.chooseSavePath("session" + settings.sessionExtension, std::string(), path) && !atomicWrite(sessionFileText(), path, 0644, error))
			host.ask("The session could not be saved.", error, {"OK"});
		return true;
	}
	case Cmd::BeginEndSelect: beginEndSelect(arg == 1); return true;
	case Cmd::CopyAllNames: copyAllNames(arg == 1); return true;
	case Cmd::BlockUncomment: blockUncomment(); return true;
	case Cmd::TrimEolToSpace: eolAndTrimToSpace(); return true;
	case Cmd::PasteHtml: pasteClipboardFormat("text/html", "HTML"); return true;
	case Cmd::PasteRtf: pasteClipboardFormat("text/rtf", "RTF"); return true;
	case Cmd::CopyBinary: copyBinary(false); return true;
	case Cmd::CutBinary: copyBinary(true); return true;
	case Cmd::PasteBinary: pasteBinary(); return true;
	case Cmd::OpenSelection: {
		const std::string path = selectionAsPath();
		std::error_code error;
		if (path.empty() || !fs::exists(path, error)) host.flash("That file doesn't exist");
		else openPath(path);
		return true;
	}
	case Cmd::OpenSelectionFolder: {
		const std::string path = selectionAsPath();
		std::error_code error;
		if (path.empty() || !fs::exists(path, error)) host.flash("That file doesn't exist");
		else host.reveal(path);
		return true;
	}
	case Cmd::RedactSelection: redactSelection(arg == 1); return true;
	case Cmd::SearchInternet: searchOnInternet(); return true;
	case Cmd::ChangeSearchEngine: changeSearchEngine(); return true;
	case Cmd::ColumnModeInfo:
		host.ask("Column Mode", "Hold Alt on Linux or Option on a Mac while you drag to select a column. Alt+Shift (Option+Shift) with the arrow keys does the same from the keyboard.", {"OK"});
		return true;
	case Cmd::ReadOnlyAll: setAllReadOnly(true); return true;
	case Cmd::ReadOnlyClearAll: setAllReadOnly(false); return true;
	case Cmd::FileReadOnly: toggleFileReadOnly(); return true;
	case Cmd::VolatileFind: {
		const std::string word = selectionOrWord();
		if (word.empty()) return true;
		FindOptions options;
		options.what = word;
		options.backward = arg == 1;
		options.matchCase = true;
		options.wrap = true;
		if (findNext(options) == SearchResult::NotFound) host.flash("Can't find “" + word + "”");
		return true;
	}
	case Cmd::GoToDialog: goToDialog(); return true;
	case Cmd::ShowMark: host.showFind(FindTab::Mark, selectionOrWord(), std::string()); return true;
	case Cmd::CutBookmarked: {
		run(Cmd::CopyBookmarked);
		removeLines(false);
		return true;
	}
	case Cmd::PasteBookmarked: pasteToBookmarked(); return true;
	case Cmd::FindCharsInRange: findCharactersDialog(); return true;
	case Cmd::ViewInBrowser: viewInBrowser(argument); return true;
	case Cmd::ToggleNonPrinting:
		settings.showNonPrinting = !settings.showNonPrinting;
		setPrintableSymbols();
		saveSettings();
		return true;
	case Cmd::ToggleControlChars:
		settings.showControlChars = !settings.showControlChars;
		setPrintableSymbols();
		saveSettings();
		return true;
	case Cmd::ToggleZoomSync:
		settings.syncZoom = !settings.syncZoom;
		saveSettings();
		return true;
	case Cmd::MoveToNewInstance: {
		if (!document || documentPath.empty()) {
			host.flash("Save the file first");
			return true;
		}
		if (document->dirty() && !saveDocument(*document, documentPath)) return true;
		openNewInstance(documentPath);
		closeTab(focus, paneAt(focus).active);
		return true;
	}
	case Cmd::OpenInNewInstance: if (!documentPath.empty()) openNewInstance(documentPath); return true;
	case Cmd::FirstTab: activate(focus, 0); host.focusPane(focus); return true;
	case Cmd::LastTab: activate(focus, static_cast<int>(paneAt(focus).tabs.size()) - 1); host.focusPane(focus); return true;
	case Cmd::MoveTabStart: moveTab(focus, paneAt(focus).active, 0); return true;
	case Cmd::MoveTabEnd: moveTab(focus, paneAt(focus).active, static_cast<int>(paneAt(focus).tabs.size()) - 1); return true;
	case Cmd::TabColour: setTabColour(focus, paneAt(focus).active, arg); return true;
	case Cmd::HideLines: hideSelectedLines(); return true;
	case Cmd::TextDirection: setTextDirection(arg == 1); return true;
	case Cmd::ToggleMonitoring: toggleMonitoring(); return true;
	case Cmd::WindowSort: sortTabs(arg); return true;
	case Cmd::ShowWindows: showWindowsDialog(); return true;
	case Cmd::RecentWindow: {
		noteTabUsed();
		for (size_t i = 1; i < mru.size(); i++) {
			for (const auto &candidate : documents) {
				if (candidate.get() == mru[i]) {
					activateDocument(mru[i]);
					return true;
				}
			}
		}
		return true;
	}
	case Cmd::DocumentSwitch:
		if (settings.prefOn("switcherEnabled")) switchDocument(arg == 1);
		else run(arg == 1 ? Cmd::PreviousTab : Cmd::NextTab);
		return true;
	case Cmd::MacroSave: saveCurrentMacro(); return true;
	case Cmd::MacroRunSaved:
		for (const SavedMacro &saved : savedMacros)
			if (saved.name == argument) playSteps(saved.steps);
		return true;
	case Cmd::MacroRunMultiple: runMacroMultiple(); return true;
	case Cmd::MacroManage: manageMacros(); return true;
	case Cmd::RunDialog: runDialog(); return true;
	case Cmd::RunSaved: runCommandLine(argument); return true;
	case Cmd::RunManage: manageRunCommands(); return true;
	case Cmd::ValidateShortcuts: validateShortcuts(); return true;
	case Cmd::HashDialog: hashDialog(static_cast<HashKind>(arg)); return true;
	case Cmd::HelpOpen: openExternally(argument); return true;
	case Cmd::HelpArguments: host.ask("Command Line Arguments", commandLineHelp(), {"OK"}); return true;
	case Cmd::DebugInfo: {
		const std::string info = debugInfo();
		if (host.ask("Debug Info", info, {"Close", "Copy"}) == 1) host.setClipboard(info);
		return true;
	}
	case Cmd::CheckUpdates: checkForUpdates(); return true;
	case Cmd::SetUpdateProxy: {
		std::string proxy;
		if (host.promptText("Set Updater Proxy", "Proxy address:", settings.updateProxy, proxy)) {
			settings.updateProxy = proxy;
			saveSettings();
		}
		return true;
	}
	default:
		return false;
	}
}

}  // namespace omapad
