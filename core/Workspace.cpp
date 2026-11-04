#include "Workspace.h"

#include "Scintilla.h"
#include "json.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fnmatch.h>
#include <fstream>
#include <regex>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <copyfile.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace omapad {

namespace {

const int kMarginNumber = 0;
const int kMarginBookmark = 1;
const int kMarginFold = 2;
const int kMarkerBookmark = 24;
const int kIndicatorSmart = 30;
const int kIndicatorMark = 31;

// Beyond this a text editor stops being the right tool, and loading could exhaust memory.
const unsigned long long kMaxOpenBytes = 2ULL * 1024 * 1024 * 1024;
const unsigned long long kMaxSearchBytes = 256ULL * 1024 * 1024;
const long kMaxResultLines = 50000;
const size_t kMaxRegexLine = 1000000;

const int kPaneArgFactor = 10000;

bool fileMTime(const std::string &path, int64_t &out) {
	std::error_code error;
	const auto time = fs::last_write_time(path, error);
	if (error) return false;
	out = static_cast<int64_t>(time.time_since_epoch().count());
	return true;
}

std::string baseName(const std::string &path) { return fs::path(path).filename().string(); }
std::string directoryOf(const std::string &path) { return fs::path(path).parent_path().string(); }

std::string expandPath(const std::string &raw) {
	std::string path = raw;
	if (path.rfind("~/", 0) == 0 || path == "~") {
		const char *home = getenv("HOME");
		if (home) path = std::string(home) + path.substr(1);
	}
	std::error_code error;
	fs::path absolute = fs::absolute(path, error);
	if (error) return path;
	return absolute.lexically_normal().string();
}

// Writes via a temporary file and rename, so a crash or full disk can't leave a half written
// file where the original used to be. `mode` -1 keeps the existing file's permissions.
bool atomicWrite(const std::string &bytes, const std::string &rawPath, int mode, std::string &errorOut) {
	std::error_code fsError;
	// Follows symlinks, so a symlink stays a symlink and its target gets the new contents.
	const std::string path = fs::weakly_canonical(rawPath, fsError).string();
	const std::string target = fsError ? rawPath : path;
	std::string pattern = directoryOf(target) + "/." + baseName(target) + ".omapad-XXXXXX";
	const int fd = mkstemp(pattern.data());
	if (fd < 0) {
		// A folder we can't create files in may still hold a file we can overwrite.
		const int out = open(target.c_str(), O_WRONLY | O_TRUNC);
		if (out < 0) {
			errorOut = std::strerror(errno);
			return false;
		}
		size_t done = 0;
		bool ok = true;
		while (done < bytes.size()) {
			const ssize_t written = write(out, bytes.data() + done, bytes.size() - done);
			if (written < 0) {
				if (errno == EINTR) continue;
				errorOut = std::strerror(errno);
				ok = false;
				break;
			}
			done += static_cast<size_t>(written);
		}
		close(out);
		return ok;
	}
	bool ok = true;
	int savedErrno = 0;
	size_t done = 0;
	while (done < bytes.size() && ok) {
		const ssize_t written = write(fd, bytes.data() + done, bytes.size() - done);
		if (written < 0) {
			if (errno == EINTR) continue;
			savedErrno = errno;
			ok = false;
		} else {
			done += static_cast<size_t>(written);
		}
	}
	if (ok && fsync(fd) != 0) {
		savedErrno = errno;
		ok = false;
	}
	if (ok) {
		struct stat existing;
		if (mode >= 0) {
			fchmod(fd, static_cast<mode_t>(mode));
		} else if (stat(target.c_str(), &existing) == 0) {
#ifdef __APPLE__
			copyfile(target.c_str(), pattern.c_str(), nullptr, COPYFILE_METADATA);
#else
			fchmod(fd, existing.st_mode & 07777);
			if (fchown(fd, existing.st_uid, existing.st_gid) != 0) { /* Only root can always do this. */ }
#endif
		} else {
			const mode_t mask = umask(0);
			umask(mask);
			fchmod(fd, 0666 & ~mask);
		}
	}
	close(fd);
	if (ok && rename(pattern.c_str(), target.c_str()) != 0) {
		savedErrno = errno;
		ok = false;
	}
	if (!ok) {
		unlink(pattern.c_str());
		errorOut = std::strerror(savedErrno);
	}
	return ok;
}

std::string randomId() {
	static const char *hex = "0123456789abcdef";
	std::ifstream random("/dev/urandom", std::ios::binary);
	std::string id;
	for (int i = 0; i < 16; i++) {
		char c = 0;
		random.get(c);
		id += hex[(c >> 4) & 0xF];
		id += hex[c & 0xF];
	}
	return id;
}

std::string jsonString(const json &j, const char *key, const std::string &fallback = std::string()) {
	const auto found = j.find(key);
	return (found != j.end() && found->is_string()) ? found->get<std::string>() : fallback;
}

long jsonLong(const json &j, const char *key, long fallback = 0) {
	const auto found = j.find(key);
	return (found != j.end() && found->is_number_integer()) ? found->get<long>() : fallback;
}

bool jsonBool(const json &j, const char *key, bool fallback = false) {
	const auto found = j.find(key);
	return (found != j.end() && found->is_boolean()) ? found->get<bool>() : fallback;
}

std::vector<std::string> jsonStrings(const json &j, const char *key) {
	std::vector<std::string> out;
	const auto found = j.find(key);
	if (found != j.end() && found->is_array())
		for (const json &value : *found)
			if (value.is_string()) out.push_back(value.get<std::string>());
	return out;
}

bool validName(const std::string &name, std::string &error) {
	if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos || name.find('\0') != std::string::npos) {
		error = "That isn't a valid name.";
		return false;
	}
	return true;
}

// Cuts at a character boundary, so a long result line never ends in half a character.
std::string truncateUtf8(const std::string &text, size_t limit) {
	if (text.size() <= limit) return text;
	size_t end = limit;
	while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) end--;
	return text.substr(0, end) + "…";
}

size_t utf8Size(char32_t cp) {
	if (cp < 0x80) return 1;
	if (cp < 0x800) return 2;
	if (cp < 0x10000) return 3;
	return 4;
}

}  // namespace

#pragma mark - Model

struct Document {
	intptr_t sciDoc = 0;
	std::string path, untitledName;
	const Encoding *encoding = nullptr;
	Eol eol = Eol::LF;
	const Language *language = nullptr;
	bool languageChosen = false;
	bool modified = false;    // Differs from Scintilla's save point.
	bool forceDirty = false;  // Unsaved for another reason, like an encoding change.
	bool hasMtime = false;
	int64_t mtime = 0;
	bool missing = false;
	std::string backupId;
	unsigned long editGeneration = 0, backupGeneration = 0;
	bool dirty() const { return modified || forceDirty; }
	std::string title() const { return path.empty() ? untitledName : baseName(path); }
};

struct Tab {
	std::shared_ptr<Document> document;
	long anchor = 0, caret = 0, firstLine = 0, xOffset = 0;
	std::vector<long> folds;
};

struct Pane {
	std::vector<Tab> tabs;
	int active = -1;
	int lineDigits = 0;
	bool smartActive = false;
};

struct ResultEntry {
	std::string path;
	std::weak_ptr<Document> document;
	long line = -1, offset = 0, length = 0;
};

// Points the hidden view at a document for as long as this lives. This reads and changes
// documents without disturbing what either visible pane is showing.
struct BackgroundDocument {
	SciView &view;
	BackgroundDocument(Host &host, const Document &document) : view(host.backgroundView()) {
		view.send(SCI_SETDOCPOINTER, 0, document.sciDoc);
	}
	~BackgroundDocument() { view.send(SCI_SETDOCPOINTER, 0, 0); }
};

Workspace::Workspace(Host &h, const std::string &resources, const std::string &settingsDirectory, const std::string &stateDirectory)
	: host(h), catalog(resources), settingsDir(settingsDirectory), stateDir(stateDirectory), alive(std::make_shared<bool>(true)) {
	panes[0] = std::make_unique<Pane>();
	panes[1] = std::make_unique<Pane>();
	std::error_code error;
	fs::create_directories(settingsDir, error);
	fs::create_directories(fs::path(stateDir) / "backup", error);
	chmod(stateDir.c_str(), 0700);
	chmod((fs::path(stateDir) / "backup").c_str(), 0700);
	loadSettings();
}

Workspace::~Workspace() {
	*alive = false;
	stopFindInFiles();
	joinSearchThread();
}

SciView &Workspace::view() const { return host.paneView(focus); }
SciView &Workspace::view(int pane) const { return host.paneView(pane); }
intptr_t Workspace::s(unsigned message, uintptr_t w, intptr_t l) const { return view().send(message, w, l); }
Pane &Workspace::paneAt(int pane) const { return *panes[pane]; }

Tab *Workspace::activeTabPointer() const {
	Pane &pane = paneAt(focus);
	return (pane.active >= 0 && pane.active < static_cast<int>(pane.tabs.size())) ? &pane.tabs[pane.active] : nullptr;
}

Document *Workspace::activeDocument() const {
	Tab *tab = activeTabPointer();
	return tab ? tab->document.get() : nullptr;
}

void Workspace::start() {
	if (started) return;
	started = true;
	reloadOmarchyTheme();
	configureView(0);
	configureView(1);
	SciView &results = host.resultsView();
	results.send(SCI_SETCODEPAGE, SC_CP_UTF8);
	results.send(SCI_SETREADONLY, 1);

	// The first view starts with a document of its own, which becomes the first tab.
	auto document = std::make_shared<Document>();
	document->sciDoc = view(0).send(SCI_GETDOCPOINTER);
	view(0).send(SCI_ADDREFDOCUMENT, 0, document->sciDoc);
	document->encoding = encodingWithIdentifier("utf8");
	document->language = catalog.plainText();
	document->untitledName = nextUntitledName();
	document->backupId = randomId();
	documents.push_back(document);
	Tab tab;
	tab.document = document;
	paneAt(0).tabs.push_back(tab);
	paneAt(0).active = 0;
	view(0).send(SCI_SETCODEPAGE, SC_CP_UTF8);
	view(0).send(SCI_SETEOLMODE, SC_EOL_LF);
	applyThemeEverywhere();
	refreshChrome();
}

#pragma mark - Settings

void Workspace::loadSettings() {
	std::ifstream in(fs::path(settingsDir) / "settings.json", std::ios::binary);
	if (!in) return;
	std::stringstream text;
	text << in.rdbuf();
	const json j = json::parse(text.str(), nullptr, false);
	if (j.is_discarded() || !j.is_object()) return;
	settings.theme = jsonString(j, "theme", settings.theme);
	settings.fontName = jsonString(j, "fontName", settings.fontName);
	const auto size = j.find("fontSize");
	if (size != j.end() && size->is_number() && size->get<double>() >= 4 && size->get<double>() <= 96) settings.fontSize = size->get<double>();
	settings.wordWrap = jsonBool(j, "wordWrap", settings.wordWrap);
	settings.showSpace = jsonBool(j, "showSpace", settings.showSpace);
	settings.showEol = jsonBool(j, "showEol", settings.showEol);
	settings.showGuides = jsonBool(j, "showGuides", settings.showGuides);
	settings.showWrapSymbol = jsonBool(j, "showWrapSymbol", settings.showWrapSymbol);
	const long width = jsonLong(j, "tabWidth", settings.tabWidth);
	if (width >= 1 && width <= 32) settings.tabWidth = static_cast<int>(width);
	settings.tabsToSpaces = jsonBool(j, "tabsToSpaces", settings.tabsToSpaces);
	settings.showHiddenInFolders = jsonBool(j, "showHiddenInFolders", settings.showHiddenInFolders);
	settings.recent = jsonStrings(j, "recent");
}

void Workspace::saveSettings() const {
	json j;
	j["theme"] = settings.theme;
	j["fontName"] = settings.fontName;
	j["fontSize"] = settings.fontSize;
	j["wordWrap"] = settings.wordWrap;
	j["showSpace"] = settings.showSpace;
	j["showEol"] = settings.showEol;
	j["showGuides"] = settings.showGuides;
	j["showWrapSymbol"] = settings.showWrapSymbol;
	j["tabWidth"] = settings.tabWidth;
	j["tabsToSpaces"] = settings.tabsToSpaces;
	j["showHiddenInFolders"] = settings.showHiddenInFolders;
	j["recent"] = settings.recent;
	std::string error;
	atomicWrite(j.dump(1, '\t', false, json::error_handler_t::replace), (fs::path(settingsDir) / "settings.json").string(), 0600, error);
}

#pragma mark - Views and themes

void Workspace::configureView(int pane) {
	SciView &v = view(pane);
	v.send(SCI_SETCODEPAGE, SC_CP_UTF8);
	v.send(SCI_SETMARGINTYPEN, kMarginNumber, SC_MARGIN_NUMBER);
	v.send(SCI_SETMARGINTYPEN, kMarginBookmark, SC_MARGIN_SYMBOL);
	v.send(SCI_SETMARGINMASKN, kMarginBookmark, 1 << kMarkerBookmark);
	v.send(SCI_SETMARGINWIDTHN, kMarginBookmark, 14);
	v.send(SCI_SETMARGINSENSITIVEN, kMarginBookmark, 1);
	v.send(SCI_SETMARGINTYPEN, kMarginFold, SC_MARGIN_SYMBOL);
	v.send(SCI_SETMARGINMASKN, kMarginFold, SC_MASK_FOLDERS);
	v.send(SCI_SETMARGINWIDTHN, kMarginFold, 14);
	v.send(SCI_SETMARGINSENSITIVEN, kMarginFold, 1);
	v.send(SCI_MARKERDEFINE, kMarkerBookmark, SC_MARK_BOOKMARK);
	v.send(SCI_MARKERSETBACK, kMarkerBookmark, 0xD07020);
	v.send(SCI_MARKERSETFORE, kMarkerBookmark, 0xD07020);
	const int markers[][2] = {
		{SC_MARKNUM_FOLDER, SC_MARK_BOXPLUS}, {SC_MARKNUM_FOLDEROPEN, SC_MARK_BOXMINUS},
		{SC_MARKNUM_FOLDERSUB, SC_MARK_VLINE}, {SC_MARKNUM_FOLDERTAIL, SC_MARK_LCORNER},
		{SC_MARKNUM_FOLDEREND, SC_MARK_BOXPLUSCONNECTED}, {SC_MARKNUM_FOLDEROPENMID, SC_MARK_BOXMINUSCONNECTED},
		{SC_MARKNUM_FOLDERMIDTAIL, SC_MARK_TCORNER},
	};
	for (const auto &m : markers) v.send(SCI_MARKERDEFINE, m[0], m[1]);
	v.send(SCI_SETFOLDFLAGS, SC_FOLDFLAG_LINEAFTER_CONTRACTED);
	v.send(SCI_SETAUTOMATICFOLD, SC_AUTOMATICFOLD_SHOW | SC_AUTOMATICFOLD_CLICK | SC_AUTOMATICFOLD_CHANGE);
	v.send(SCI_SETCARETLINEVISIBLEALWAYS, 1);
	v.send(SCI_SETMULTIPLESELECTION, 1);
	v.send(SCI_SETADDITIONALSELECTIONTYPING, 1);
	v.send(SCI_SETMULTIPASTE, SC_MULTIPASTE_EACH);
	v.send(SCI_SETVIRTUALSPACEOPTIONS, SCVS_RECTANGULARSELECTION);
	v.send(SCI_SETSCROLLWIDTH, 1);
	v.send(SCI_SETSCROLLWIDTHTRACKING, 1);
	v.send(SCI_SETMODEVENTMASK, SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT);
	for (int indicator : {kIndicatorSmart, kIndicatorMark}) {
		v.send(SCI_INDICSETSTYLE, indicator, INDIC_ROUNDBOX);
		v.send(SCI_INDICSETALPHA, indicator, 110);
		v.send(SCI_INDICSETOUTLINEALPHA, indicator, 200);
		v.send(SCI_INDICSETUNDER, indicator, 1);
	}
	applyViewPreferences(pane);
}

void Workspace::applyViewPreferences(int pane) {
	SciView &v = view(pane);
	v.send(SCI_SETVIEWWS, settings.showSpace ? SCWS_VISIBLEALWAYS : SCWS_INVISIBLE);
	v.send(SCI_SETVIEWEOL, settings.showEol);
	v.send(SCI_SETINDENTATIONGUIDES, settings.showGuides ? SC_IV_LOOKBOTH : SC_IV_NONE);
	v.send(SCI_SETWRAPVISUALFLAGS, settings.showWrapSymbol ? SC_WRAPVISUALFLAG_END : SC_WRAPVISUALFLAG_NONE);
	v.send(SCI_SETWRAPMODE, settings.wordWrap ? SC_WRAP_WORD : SC_WRAP_NONE);
}

std::string Workspace::effectiveTheme() const {
	if (settings.theme == "Default") return std::string();
	if (settings.theme == "Omarchy" && catalog.hasOmarchyTheme()) return "Omarchy";
	if (settings.theme != "Auto" && settings.theme != "Omarchy") return settings.theme;
	if (catalog.hasOmarchyTheme()) return "Omarchy";
	return host.systemIsDark() ? "DarkModeDefault" : std::string();
}

bool Workspace::themeIsDark() const { return catalog.themeIsDark(effectiveTheme()); }

ChromeColors Workspace::chromeColors() const {
	ChromeColors c;
	if (effectiveTheme() != "Omarchy" || !palette.valid()) return c;
	c.valid = true;
	c.background = palette.get("background");
	c.foreground = palette.get("foreground");
	c.dim = palette.get("dark_foreground", "foreground");
	c.accent = palette.get("accent", "blue");
	c.selection = palette.get("selection", "lighter_background");
	c.darkBackground = palette.get("dark_background", "background");
	c.lighterBackground = palette.get("lighter_background", "selection");
	return c;
}

std::string Workspace::omarchyThemePath() const {
	const char *overridePath = getenv("OMAPAD_OMARCHY_THEME");
	if (overridePath) return overridePath;
	const char *home = getenv("HOME");
	if (!home) return std::string();
	const std::string current = std::string(home) + "/.local/state/omarchy/current/theme/colors.toml";
	std::error_code error;
	if (fs::exists(current, error)) return current;
	return std::string(home) + "/.config/omarchy/current/theme/colors.toml";  // Before Quattro.
}

bool Workspace::reloadOmarchyTheme() {
	const std::string path = omarchyThemePath();
	std::string text;
	if (!path.empty()) {
		std::ifstream in(path, std::ios::binary);
		if (in) {
			std::stringstream buffer;
			buffer << in.rdbuf();
			text = buffer.str();
		}
	}
	// A theme switch rewrites this file, and could be caught half written. Keep the current
	// colours until a complete palette shows up.
	Palette parsed = parsePalette(text);
	if (!text.empty() && !parsed.valid()) return false;
	if (text == lastPaletteText) return false;
	lastPaletteText = text;
	palette = parsed;
	catalog.setOmarchyPalette(parsed);
	if (started) applyThemeEverywhere();
	return true;
}

void Workspace::systemAppearanceChanged() {
	if (settings.theme == "Auto" && !catalog.hasOmarchyTheme()) applyThemeEverywhere();
}

void Workspace::applyLanguageTo(int pane) {
	Pane &p = paneAt(pane);
	if (p.active < 0 || p.active >= static_cast<int>(p.tabs.size())) return;
	const Document &document = *p.tabs[p.active].document;
	catalog.apply(*document.language, view(pane), effectiveTheme(), settings.fontName, settings.fontSize);
	const int tabWidth = document.language->tabSize > 0 ? document.language->tabSize : settings.tabWidth;
	const bool spaces = document.language->tabSize > 0 ? document.language->tabsToSpaces : settings.tabsToSpaces;
	view(pane).send(SCI_SETTABWIDTH, tabWidth);
	view(pane).send(SCI_SETUSETABS, !spaces);
	p.lineDigits = 0;
	updateLineNumberMargin(pane);
}

void Workspace::applyThemeEverywhere() {
	applyLanguageTo(0);
	applyLanguageTo(1);
	// Panes with no tabs still need the colours, so they aren't white when first shown.
	if (paneAt(1).active < 0) catalog.apply(*catalog.plainText(), view(1), effectiveTheme(), settings.fontName, settings.fontSize);
	SciView &results = host.resultsView();
	catalog.apply(*catalog.plainText(), results, effectiveTheme(), settings.fontName, settings.fontSize > 1 ? settings.fontSize - 1 : settings.fontSize);
	results.send(SCI_SETMARGINWIDTHN, kMarginNumber, 0);
	results.send(SCI_SETMARGINWIDTHN, kMarginBookmark, 0);
	host.themeChanged();
}

void Workspace::updateLineNumberMargin(int pane) {
	SciView &v = view(pane);
	long lines = v.send(SCI_GETLINECOUNT);
	int digits = 1;
	while (lines >= 10) {
		lines /= 10;
		digits++;
	}
	if (digits < 3) digits = 3;
	Pane &p = paneAt(pane);
	if (digits == p.lineDigits) return;
	p.lineDigits = digits;
	const std::string sample(static_cast<size_t>(digits) + 1, '9');
	const long width = v.send(SCI_TEXTWIDTH, STYLE_LINENUMBER, reinterpret_cast<intptr_t>(sample.c_str()));
	v.send(SCI_SETMARGINWIDTHN, kMarginNumber, width);
}

#pragma mark - Tabs

std::string Workspace::nextUntitledName() const {
	long number = 1;
	for (const auto &document : documents) {
		if (document->path.empty() && document->untitledName.rfind("new ", 0) == 0)
			number = std::max(number, std::strtol(document->untitledName.c_str() + 4, nullptr, 10) + 1);
	}
	return "new " + std::to_string(number);
}

std::shared_ptr<Document> Workspace::addDocument() {
	auto document = std::make_shared<Document>();
	document->sciDoc = view(0).send(SCI_CREATEDOCUMENT, 0, SC_DOCUMENTOPTION_DEFAULT);
	document->encoding = encodingWithIdentifier("utf8");
	document->language = catalog.plainText();
	document->untitledName = nextUntitledName();
	document->backupId = randomId();
	{
		BackgroundDocument background(host, *document);
		background.view.send(SCI_SETCODEPAGE, SC_CP_UTF8);
		background.view.send(SCI_SETEOLMODE, SC_EOL_LF);
	}
	documents.push_back(document);
	return document;
}

void Workspace::addTab(int pane, std::shared_ptr<Document> document) {
	Tab tab;
	tab.document = std::move(document);
	paneAt(pane).tabs.push_back(tab);
}

void Workspace::saveViewState(int pane) {
	Pane &p = paneAt(pane);
	if (p.active < 0 || p.active >= static_cast<int>(p.tabs.size())) return;
	Tab &tab = p.tabs[p.active];
	SciView &v = view(pane);
	tab.anchor = v.send(SCI_GETANCHOR);
	tab.caret = v.send(SCI_GETCURRENTPOS);
	tab.firstLine = v.send(SCI_DOCLINEFROMVISIBLE, v.send(SCI_GETFIRSTVISIBLELINE));
	tab.xOffset = v.send(SCI_GETXOFFSET);
	tab.folds.clear();
	long line = 0;
	while ((line = v.send(SCI_CONTRACTEDFOLDNEXT, line)) >= 0) {
		tab.folds.push_back(line);
		line++;
	}
}

void Workspace::activate(int pane, int index) {
	Pane &p = paneAt(pane);
	if (index < 0 || index >= static_cast<int>(p.tabs.size())) return;
	if (index != p.active) saveViewState(pane);
	Tab &tab = p.tabs[index];
	SciView &v = view(pane);
	v.send(SCI_SETDOCPOINTER, 0, tab.document->sciDoc);
	p.active = index;
	v.send(SCI_SETCODEPAGE, SC_CP_UTF8);
	v.send(SCI_SETEOLMODE, static_cast<int>(tab.document->eol));
	applyLanguageTo(pane);
	for (long line : tab.folds) v.send(SCI_FOLDLINE, line, SC_FOLDACTION_CONTRACT);
	v.send(SCI_SETSEL, tab.anchor, tab.caret);
	v.send(SCI_SETFIRSTVISIBLELINE, v.send(SCI_VISIBLEFROMDOCLINE, tab.firstLine));
	v.send(SCI_SETXOFFSET, tab.xOffset);
	p.smartActive = false;
	refreshChrome();
}

void Workspace::createUntitledDocument() {
	addTab(focus, addDocument());
	activate(focus, static_cast<int>(paneAt(focus).tabs.size()) - 1);
	host.focusPane(focus);
}

void Workspace::refreshChrome() {
	host.tabsChanged();
	updateStatus();
}

void Workspace::updateStatus() {
	const Document *document = activeDocument();
	if (!document) return;
	const long position = s(SCI_GETCURRENTPOS);
	const long line = s(SCI_LINEFROMPOSITION, position);
	const long column = s(SCI_GETCOLUMN, position);
	const long start = s(SCI_GETSELECTIONSTART);
	const long end = s(SCI_GETSELECTIONEND);
	const long selected = end > start ? s(SCI_COUNTCHARACTERS, start, end) : 0;
	std::string left = document->language->displayName + "    length: " + std::to_string(s(SCI_GETLENGTH)) + "    lines: " + std::to_string(s(SCI_GETLINECOUNT));
	std::string right = "Ln " + std::to_string(line + 1) + ", Col " + std::to_string(column + 1) + ", Sel " + std::to_string(selected) + "    " +
		eolName(document->eol) + "    " + document->encoding->name + "    " + (s(SCI_GETOVERTYPE) ? "OVR" : "INS");
	host.statusChanged(left, right);
}

bool Workspace::splitVisible() const { return !paneAt(1).tabs.empty(); }

std::vector<TabInfo> Workspace::tabs(int pane) const {
	std::vector<TabInfo> out;
	for (const Tab &tab : paneAt(pane).tabs) {
		TabInfo info;
		info.title = tab.document->title();
		info.toolTip = tab.document->path.empty() ? tab.document->title() : tab.document->path;
		info.dirty = tab.document->dirty();
		out.push_back(info);
	}
	return out;
}

int Workspace::activeTab(int pane) const { return paneAt(pane).active; }

void Workspace::paneFocused(int pane) {
	if (pane < 0 || pane > 1 || paneAt(pane).tabs.empty() || pane == focus) return;
	focus = pane;
	refreshChrome();
}

void Workspace::clickTab(int pane, int index) {
	focus = pane;
	activate(pane, index);
	host.focusPane(pane);
}

void Workspace::moveTab(int pane, int from, int to) {
	Pane &p = paneAt(pane);
	const int count = static_cast<int>(p.tabs.size());
	if (from < 0 || to < 0 || from >= count || to >= count || from == to) return;
	const std::shared_ptr<Document> activeDoc = p.tabs[p.active].document;
	Tab moved = std::move(p.tabs[from]);
	p.tabs.erase(p.tabs.begin() + from);
	p.tabs.insert(p.tabs.begin() + to, std::move(moved));
	for (int i = 0; i < count; i++)
		if (p.tabs[i].document == activeDoc) p.active = i;
	host.tabsChanged();
}

bool Workspace::documentOpenElsewhere(const Document *document, int pane, int index) const {
	for (int pn = 0; pn < 2; pn++) {
		const auto &list = paneAt(pn).tabs;
		for (int i = 0; i < static_cast<int>(list.size()); i++)
			if (list[i].document.get() == document && !(pn == pane && i == index)) return true;
	}
	return false;
}

void Workspace::detachPane(int pane) {
	view(pane).send(SCI_SETDOCPOINTER, 0, 0);
	paneAt(pane).active = -1;
}

void Workspace::releaseIfUnused(const std::shared_ptr<Document> &document) {
	if (documentOpenElsewhere(document.get(), -1, -1)) return;
	removeBackup(*document);
	view(0).send(SCI_RELEASEDOCUMENT, 0, document->sciDoc);
	documents.erase(std::remove(documents.begin(), documents.end(), document), documents.end());
}

void Workspace::afterSplitChange() {
	host.splitChanged(splitVisible(), stacked);
	if (!splitVisible() && focus == 1) {
		focus = 0;
		host.focusPane(0);
	}
}

bool Workspace::closeTab(int pane, int index) {
	Pane &p = paneAt(pane);
	if (index < 0 || index >= static_cast<int>(p.tabs.size())) return false;
	const std::shared_ptr<Document> document = p.tabs[index].document;
	if (document->dirty() && !documentOpenElsewhere(document.get(), pane, index)) {
		focus = pane;
		activate(pane, index);
		const int choice = host.ask("Save changes to “" + document->title() + "”?",
		                            "Your changes will be lost if you don't save them.",
		                            {"Save", "Cancel", "Don't Save"});
		if (choice == 1) return false;
		if (choice == 0 && !saveActive()) return false;
	}
	// The first view always has a tab, so its last one is replaced by a new empty document.
	if (pane == 0 && p.tabs.size() == 1) addTab(0, addDocument());
	if (p.tabs.size() == 1) {
		detachPane(pane);
		p.tabs.clear();
		releaseIfUnused(document);
		afterSplitChange();
		refreshChrome();
		return true;
	}
	if (index == p.active) {
		const int neighbour = index + 1 < static_cast<int>(p.tabs.size()) ? index + 1 : index - 1;
		activate(pane, neighbour);
	}
	p.tabs.erase(p.tabs.begin() + index);
	if (p.active > index) p.active--;
	releaseIfUnused(document);
	refreshChrome();
	return true;
}

bool Workspace::closeActive() { return closeTab(focus, paneAt(focus).active); }

void Workspace::closeTabs(std::vector<std::pair<int, std::shared_ptr<Document>>> targets) {
	for (const auto &target : targets) {
		const auto &list = paneAt(target.first).tabs;
		for (int i = 0; i < static_cast<int>(list.size()); i++) {
			if (list[i].document == target.second) {
				if (!closeTab(target.first, i)) return;
				break;
			}
		}
	}
}

bool Workspace::moveTabToOtherView(int pane, int index) {
	Pane &source = paneAt(pane);
	if (index < 0 || index >= static_cast<int>(source.tabs.size())) return false;
	// The first view always keeps a tab, so its only one can't leave.
	if (pane == 0 && source.tabs.size() == 1) {
		host.flash("Open another tab first, so this view isn't left empty");
		return false;
	}
	if (!cloneTabToOtherView(pane, index)) return false;
	const std::shared_ptr<Document> document = source.tabs[index].document;
	// The document is open in the other view now, so closing this tab asks nothing.
	if (source.tabs.size() == 1) {
		detachPane(pane);
		source.tabs.clear();
		afterSplitChange();
	} else {
		if (index == source.active) activate(pane, index + 1 < static_cast<int>(source.tabs.size()) ? index + 1 : index - 1);
		source.tabs.erase(source.tabs.begin() + index);
		if (source.active > index) source.active--;
	}
	refreshChrome();
	return true;
}

bool Workspace::cloneTabToOtherView(int pane, int index) {
	Pane &source = paneAt(pane);
	if (index < 0 || index >= static_cast<int>(source.tabs.size())) return false;
	if (index == source.active) saveViewState(pane);
	const Tab tab = source.tabs[index];
	const int other = 1 - pane;
	Pane &target = paneAt(other);
	int existing = -1;
	for (int i = 0; i < static_cast<int>(target.tabs.size()); i++)
		if (target.tabs[i].document == tab.document) existing = i;
	if (existing < 0) {
		target.tabs.push_back(tab);
		existing = static_cast<int>(target.tabs.size()) - 1;
		afterSplitChange();
	}
	focus = other;
	activate(other, existing);
	host.focusPane(other);
	return true;
}

void Workspace::findOpen(const std::string &path, int &pane, int &index) const {
	pane = index = -1;
	// Prefer the focused view when a file is open in both.
	for (int pn : {focus, 1 - focus}) {
		const auto &list = paneAt(pn).tabs;
		for (int i = 0; i < static_cast<int>(list.size()); i++) {
			if (list[i].document->path == path) {
				pane = pn;
				index = i;
				return;
			}
		}
	}
}

int Workspace::documentCount() const { return static_cast<int>(documents.size()); }
std::string Workspace::activeTitle() const { const Document *d = activeDocument(); return d ? d->title() : std::string(); }
std::string Workspace::activePath() const { const Document *d = activeDocument(); return d ? d->path : std::string(); }
std::string Workspace::activeEncodingIdentifier() const { const Document *d = activeDocument(); return d ? d->encoding->identifier : ""; }
std::string Workspace::activeLanguageName() const { const Document *d = activeDocument(); return d ? d->language->name : std::string(); }
bool Workspace::activeIsDirty() const { const Document *d = activeDocument(); return d && d->dirty(); }
std::string Workspace::activeEolName() const { const Document *d = activeDocument(); return d ? eolName(d->eol) : ""; }
std::string Workspace::windowTitle() const { return activeTitle(); }
std::string Workspace::windowPath() const { return activePath(); }
bool Workspace::windowDirty() const { return activeIsDirty(); }

std::string Workspace::text() const {
	const long length = s(SCI_GETLENGTH);
	const char *pointer = reinterpret_cast<const char *>(s(SCI_GETCHARACTERPOINTER));
	return (pointer && length > 0) ? std::string(pointer, static_cast<size_t>(length)) : std::string();
}

void Workspace::setText(const std::string &value) { s(SCI_SETTEXT, 0, reinterpret_cast<intptr_t>(value.c_str())); }
void Workspace::setSelection(long start, long end) { s(SCI_SETSEL, start, end); }
std::string Workspace::selectedText() const { return rangeOf(s(SCI_GETSELECTIONSTART), s(SCI_GETSELECTIONEND)); }

std::string Workspace::rangeOf(long start, long end) const {
	if (end <= start) return std::string();
	const char *pointer = reinterpret_cast<const char *>(s(SCI_GETRANGEPOINTER, start, end - start));
	return pointer ? std::string(pointer, static_cast<size_t>(end - start)) : std::string();
}

std::string Workspace::textOf(const Document &document) const {
	BackgroundDocument background(host, document);
	const long length = background.view.send(SCI_GETLENGTH);
	const char *pointer = reinterpret_cast<const char *>(background.view.send(SCI_GETCHARACTERPOINTER));
	return (pointer && length > 0) ? std::string(pointer, static_cast<size_t>(length)) : std::string();
}

// Replaces a document's whole contents as a fresh, unmodified load.
void Workspace::loadInto(Document &document, const std::string &utf8) {
	document.eol = detectEol(utf8);
	{
		BackgroundDocument background(host, document);
		SciView &v = background.view;
		v.send(SCI_SETUNDOCOLLECTION, 0);
		v.send(SCI_CLEARALL);
		if (!utf8.empty()) v.send(SCI_ADDTEXT, utf8.size(), reinterpret_cast<intptr_t>(utf8.data()));
		v.send(SCI_SETUNDOCOLLECTION, 1);
		v.send(SCI_EMPTYUNDOBUFFER);
		v.send(SCI_SETSAVEPOINT);
		v.send(SCI_SETEOLMODE, static_cast<int>(document.eol));
	}
	document.modified = false;
	document.forceDirty = false;
}

bool Workspace::readFile(const std::string &path, std::string &out, bool report) {
	std::error_code error;
	const auto status = fs::status(path, error);
	if (!error && fs::is_directory(status)) {
		if (report) host.ask("That's a folder, not a file.", path, {"OK"});
		return false;
	}
	const auto size = fs::file_size(path, error);
	if (!error && size > kMaxOpenBytes) {
		if (report) host.ask("This file is too large to open.", path + " is over the 2 GB limit.", {"OK"});
		return false;
	}
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		if (report) host.ask("The file could not be opened.", path + "\n\n" + std::strerror(errno), {"OK"});
		return false;
	}
	std::stringstream buffer;
	buffer << in.rdbuf();
	if (in.bad()) {
		if (report) host.ask("The file could not be read.", path, {"OK"});
		return false;
	}
	out = buffer.str();
	return true;
}

const Language *Workspace::detectLanguage(const std::string &path, const std::string &utf8) const {
	const Language *language = catalog.languageForPath(path);
	if (language != catalog.plainText()) return language;
	const size_t end = utf8.find_first_of("\r\n");
	const std::string first = utf8.substr(0, end == std::string::npos ? std::min<size_t>(utf8.size(), 256) : end);
	const Language *fromLine = catalog.languageForFirstLine(first);
	return fromLine ? fromLine : language;
}

bool Workspace::openPath(const std::string &rawPath) {
	const std::string path = expandPath(rawPath);
	int openPane, openIndex;
	findOpen(path, openPane, openIndex);
	if (openPane >= 0) {
		focus = openPane;
		activate(openPane, openIndex);
		host.focusPane(openPane);
		return true;
	}
	std::string bytes;
	if (!readFile(path, bytes, true)) return false;
	Decoded decoded = decodeAuto(bytes);

	// A single empty, untouched tab is just a placeholder, so the file takes its place.
	Document *current = activeDocument();
	const bool reuse = current && current->path.empty() && !current->dirty() && s(SCI_GETLENGTH) == 0 &&
		!documentOpenElsewhere(current, focus, paneAt(focus).active);
	std::shared_ptr<Document> document;
	if (reuse) {
		document = activeTabPointer()->document;
	} else {
		document = addDocument();
		addTab(focus, document);
	}
	document->path = path;
	document->untitledName.clear();
	document->encoding = decoded.encoding;
	document->language = detectLanguage(path, decoded.utf8);
	document->hasMtime = fileMTime(path, document->mtime);
	document->missing = false;
	loadInto(*document, decoded.utf8);
	Tab &tab = reuse ? *activeTabPointer() : paneAt(focus).tabs.back();
	tab.anchor = tab.caret = tab.firstLine = tab.xOffset = 0;
	tab.folds.clear();
	activate(focus, reuse ? paneAt(focus).active : static_cast<int>(paneAt(focus).tabs.size()) - 1);
	addRecent(path);
	host.focusPane(focus);
	return true;
}

void Workspace::openFiles(const std::vector<std::string> &paths) {
	for (const std::string &path : paths) openPath(path);
}

bool Workspace::writeFileAtomically(const std::string &bytes, const std::string &path, std::string &error) {
	return atomicWrite(bytes, path, -1, error);
}

bool Workspace::saveDocument(Document &document, const std::string &path) {
	const std::string content = textOf(document);
	std::string bytes;
	if (!encode(content, document.encoding, bytes)) {
		host.ask("This text can't be saved in the current encoding.",
		         std::string("Some characters don't exist in ") + document.encoding->name +
		         ". Nothing was written. Convert the document to UTF-8 from the Encoding menu, then save again.", {"OK"});
		return false;
	}
	std::string error;
	if (!writeFileAtomically(bytes, path, error)) {
		host.ask("The file could not be saved.", path + "\n\n" + error + "\n\nYour text is still open here. Try Save As to choose another location.", {"OK"});
		return false;
	}
	const bool pathChanged = document.path != path;
	document.path = path;
	document.untitledName.clear();
	document.hasMtime = fileMTime(path, document.mtime);
	document.missing = false;
	document.forceDirty = false;
	{
		BackgroundDocument background(host, document);
		background.view.send(SCI_SETSAVEPOINT);
	}
	document.modified = false;
	if (pathChanged && !document.languageChosen) document.language = detectLanguage(path, content);
	if (pathChanged) {
		for (int pn = 0; pn < 2; pn++) {
			const Pane &p = paneAt(pn);
			if (p.active >= 0 && p.tabs[p.active].document.get() == &document) applyLanguageTo(pn);
		}
	}
	removeBackup(document);
	addRecent(path);
	refreshChrome();
	return true;
}

bool Workspace::saveActiveToPath(const std::string &path) {
	Document *document = activeDocument();
	return document && saveDocument(*document, expandPath(path));
}

bool Workspace::saveActive() {
	Document *document = activeDocument();
	if (!document) return false;
	return document->path.empty() ? saveAsDialog(*document) : saveDocument(*document, document->path);
}

bool Workspace::saveAsDialog(Document &document) {
	std::string path;
	const std::string start = document.path.empty() ? std::string() : directoryOf(document.path);
	if (!host.chooseSavePath(document.title(), start, path)) return false;
	return saveDocument(document, path);
}

void Workspace::checkExternalChanges() {
	if (checkingExternal) return;
	checkingExternal = true;
	const auto snapshot = documents;
	for (const auto &document : snapshot) {
		if (document->path.empty()) continue;
		int64_t current = 0;
		const auto show = [&]() {
			int pane, index;
			findOpen(document->path, pane, index);
			if (pane >= 0) {
				focus = pane;
				activate(pane, index);
			}
		};
		if (!fileMTime(document->path, current)) {
			if (!document->missing) {
				document->missing = true;
				document->forceDirty = true;
				show();
				host.ask("“" + document->title() + "” no longer exists on disk.",
				         "The text stays open here and is marked as unsaved. Saving writes it back.", {"OK"});
			}
			continue;
		}
		if (document->hasMtime && current == document->mtime) continue;
		document->missing = false;
		show();
		const std::string detail = document->dirty()
			? "Reloading discards your unsaved changes here. This can't be undone."
			: "Reload it to see the new contents?";
		const int choice = host.ask("“" + document->title() + "” was changed by another program.", detail, {"Keep Current Text", "Reload"});
		document->mtime = current;
		document->hasMtime = true;
		if (choice == 1) reloadActive();
	}
	checkingExternal = false;
	refreshChrome();
}

bool Workspace::reloadActive() {
	Document *document = activeDocument();
	if (!document || document->path.empty()) return false;
	std::string bytes;
	if (!readFile(document->path, bytes, true)) return false;
	Decoded decoded = decodeAuto(bytes);
	const long caret = s(SCI_GETCURRENTPOS);
	const long firstLine = s(SCI_GETFIRSTVISIBLELINE);
	document->encoding = decoded.encoding;
	document->hasMtime = fileMTime(document->path, document->mtime);
	loadInto(*document, decoded.utf8);
	s(SCI_SETEOLMODE, static_cast<int>(document->eol));
	s(SCI_GOTOPOS, std::min<long>(caret, s(SCI_GETLENGTH)));
	s(SCI_SETFIRSTVISIBLELINE, firstLine);
	removeBackup(*document);
	refreshChrome();
	return true;
}

void Workspace::addRecent(const std::string &path) {
	auto &recent = settings.recent;
	recent.erase(std::remove(recent.begin(), recent.end(), path), recent.end());
	recent.insert(recent.begin(), path);
	if (recent.size() > 15) recent.resize(15);
	saveSettings();
}

#pragma mark - Session and backups

std::string Workspace::backupPath(const Document &document) const {
	return (fs::path(stateDir) / "backup" / (document.backupId + ".txt")).string();
}

void Workspace::removeBackup(Document &document) {
	std::error_code error;
	fs::remove(backupPath(document), error);
	document.backupGeneration = 0;
}

bool Workspace::writeSession() {
	saveViewState(0);
	saveViewState(1);
	bool ok = true;
	std::error_code fsError;
	json documentList = json::array();
	std::vector<std::string> keptBackups;
	std::vector<Document *> written;
	for (const auto &document : documents) {
		const bool empty = document->path.empty() && !document->dirty() && textOf(*document).empty();
		if (empty) continue;
		json entry;
		if (!document->path.empty()) entry["path"] = document->path;
		if (!document->untitledName.empty()) entry["untitledName"] = document->untitledName;
		entry["encoding"] = document->encoding->identifier;
		entry["eol"] = static_cast<int>(document->eol);
		entry["language"] = document->language->name;
		entry["languageChosen"] = document->languageChosen;
		if (document->hasMtime) entry["mtime"] = document->mtime;
		if (document->dirty()) {
			const std::string backup = backupPath(*document);
			bool haveBackup = fs::exists(backup, fsError);
			if (!haveBackup || document->backupGeneration != document->editGeneration + 1) {
				std::string error;
				if (atomicWrite(textOf(*document), backup, 0600, error)) {
					document->backupGeneration = document->editGeneration + 1;
					haveBackup = true;
				} else {
					ok = false;
				}
			}
			if (haveBackup) {
				entry["backup"] = document->backupId;
				keptBackups.push_back(document->backupId + ".txt");
			}
		}
		documentList.push_back(entry);
		written.push_back(document.get());
	}
	json paneList = json::array();
	for (int pn = 0; pn < 2; pn++) {
		json tabList = json::array();
		int activeOut = 0;
		const Pane &p = paneAt(pn);
		for (int i = 0; i < static_cast<int>(p.tabs.size()); i++) {
			const Tab &tab = p.tabs[i];
			const auto found = std::find(written.begin(), written.end(), tab.document.get());
			if (found == written.end()) continue;
			if (i == p.active) activeOut = static_cast<int>(tabList.size());
			json entry;
			entry["document"] = found - written.begin();
			entry["anchor"] = tab.anchor;
			entry["caret"] = tab.caret;
			entry["firstLine"] = tab.firstLine;
			tabList.push_back(entry);
		}
		json out;
		out["tabs"] = tabList;
		out["active"] = activeOut;
		paneList.push_back(out);
	}
	json session;
	session["version"] = 2;
	session["documents"] = documentList;
	session["panes"] = paneList;
	session["focus"] = focus;
	session["stacked"] = stacked;
	session["folders"] = folderRoots;
	session["expanded"] = expandedFolders;
	session["folderPanel"] = folderPanel;
	const std::string text = session.dump(-1, ' ', false, json::error_handler_t::replace);
	if (text != lastSession) {
		std::string error;
		if (atomicWrite(text, (fs::path(stateDir) / "session.json").string(), 0600, error)) lastSession = text;
		else ok = false;
	}
	// Only drop stale backups once the session that no longer needs them is safely written.
	if (ok) {
		for (const auto &entry : fs::directory_iterator(fs::path(stateDir) / "backup", fsError)) {
			const std::string name = entry.path().filename().string();
			if (std::find(keptBackups.begin(), keptBackups.end(), name) == keptBackups.end()) fs::remove(entry.path(), fsError);
		}
	}
	return ok;
}

bool Workspace::restoreSession() {
	std::ifstream in(fs::path(stateDir) / "session.json", std::ios::binary);
	if (!in) return false;
	std::stringstream buffer;
	buffer << in.rdbuf();
	const json session = json::parse(buffer.str(), nullptr, false);
	if (session.is_discarded() || !session.is_object()) return false;

	// Folders first, so they're back even when there are no documents to restore.
	for (const std::string &folder : jsonStrings(session, "folders")) {
		std::error_code error;
		if (fs::is_directory(folder, error)) folderRoots.push_back(folder);
	}
	expandedFolders = jsonStrings(session, "expanded");
	folderPanel = jsonBool(session, "folderPanel", !folderRoots.empty()) && !folderRoots.empty();
	stacked = jsonBool(session, "stacked");
	host.setFolderPanelVisible(folderPanel);
	host.foldersChanged();

	const auto documentList = session.find("documents");
	const auto paneList = session.find("panes");
	if (documentList == session.end() || !documentList->is_array() || paneList == session.end() || !paneList->is_array()) return !folderRoots.empty();

	std::vector<std::shared_ptr<Document>> restored;
	std::error_code fsError;
	Document *first = activeDocument();
	bool canReuse = first && first->path.empty() && !first->dirty() && s(SCI_GETLENGTH) == 0 && documents.size() == 1;
	for (const json &entry : *documentList) {
		restored.push_back(nullptr);
		if (!entry.is_object()) continue;
		const std::string path = jsonString(entry, "path");
		std::string backupId = jsonString(entry, "backup");
		// The session file is data, so a backup id can only ever name a file in our folder.
		if (backupId.find('/') != std::string::npos || backupId.find('.') != std::string::npos) backupId.clear();
		std::string backupText;
		bool haveBackup = false;
		if (!backupId.empty()) {
			std::ifstream backup(fs::path(stateDir) / "backup" / (backupId + ".txt"), std::ios::binary);
			if (backup) {
				std::stringstream contents;
				contents << backup.rdbuf();
				backupText = contents.str();
				haveBackup = isValidUtf8(backupText.data(), backupText.size());
			}
		}
		std::string fileBytes;
		const bool haveFile = !haveBackup && !path.empty() && readFile(path, fileBytes, false);
		if (!haveBackup && !haveFile) continue;

		std::shared_ptr<Document> document;
		if (canReuse) {
			document = documents[0];
			canReuse = false;
		} else {
			document = addDocument();
		}
		document->path = path;
		document->untitledName = path.empty() ? jsonString(entry, "untitledName", nextUntitledName()) : std::string();
		if (haveBackup) {
			const Encoding *encoding = encodingWithIdentifier(jsonString(entry, "encoding", "utf8"));
			document->encoding = encoding ? encoding : encodingWithIdentifier("utf8");
			loadInto(*document, backupText);
			document->forceDirty = true;
			document->backupId = backupId;
			document->backupGeneration = document->editGeneration + 1;
			const long eol = jsonLong(entry, "eol", 2);
			if (eol >= 0 && eol <= 2) document->eol = static_cast<Eol>(eol);
		} else {
			Decoded decoded = decodeAuto(fileBytes);
			document->encoding = decoded.encoding;
			loadInto(*document, decoded.utf8);
		}
		if (!path.empty()) {
			// Keep the recorded time, so a file edited while we were closed gets flagged.
			const auto recorded = entry.find("mtime");
			if (recorded != entry.end() && recorded->is_number_integer()) {
				document->mtime = recorded->get<int64_t>();
				document->hasMtime = true;
			} else {
				document->hasMtime = fileMTime(path, document->mtime);
			}
		}
		document->languageChosen = jsonBool(entry, "languageChosen");
		document->language = catalog.languageNamed(jsonString(entry, "language", "normal"));
		restored.back() = document;
	}

	std::vector<Tab> paneTabs[2];
	int paneActive[2] = {0, 0};
	for (int pn = 0; pn < 2 && pn < static_cast<int>(paneList->size()); pn++) {
		const json &entry = (*paneList)[pn];
		if (!entry.is_object()) continue;
		const auto tabList = entry.find("tabs");
		if (tabList == entry.end() || !tabList->is_array()) continue;
		const long wantedActive = jsonLong(entry, "active");
		for (int i = 0; i < static_cast<int>(tabList->size()); i++) {
			const json &tabEntry = (*tabList)[i];
			if (!tabEntry.is_object()) continue;
			const long index = jsonLong(tabEntry, "document", -1);
			if (index < 0 || index >= static_cast<long>(restored.size()) || !restored[index]) continue;
			if (i == wantedActive) paneActive[pn] = static_cast<int>(paneTabs[pn].size());
			Tab tab;
			tab.document = restored[index];
			const long length = [&] { BackgroundDocument background(host, *tab.document); return static_cast<long>(background.view.send(SCI_GETLENGTH)); }();
			tab.anchor = std::min(std::max(0L, jsonLong(tabEntry, "anchor")), length);
			tab.caret = std::min(std::max(0L, jsonLong(tabEntry, "caret")), length);
			tab.firstLine = std::max(0L, jsonLong(tabEntry, "firstLine"));
			paneTabs[pn].push_back(tab);
		}
	}
	// A document that lost its tab to a damaged session still gets one.
	for (const auto &document : restored) {
		if (!document) continue;
		bool hasTab = false;
		for (const auto &list : paneTabs)
			for (const Tab &tab : list) hasTab = hasTab || tab.document == document;
		if (!hasTab) {
			Tab tab;
			tab.document = document;
			paneTabs[0].push_back(tab);
		}
	}
	bool any = false;
	for (const auto &list : paneTabs) any = any || !list.empty();
	if (!any) return !folderRoots.empty();
	// The second view can't exist without the first.
	if (paneTabs[0].empty()) {
		paneTabs[0] = std::move(paneTabs[1]);
		paneTabs[1].clear();
		paneActive[0] = paneActive[1];
	}
	for (int pn = 0; pn < 2; pn++) {
		Pane &p = paneAt(pn);
		const bool adopted = pn == 0;
		std::shared_ptr<Document> previous = (adopted && !p.tabs.empty()) ? p.tabs[0].document : nullptr;
		p.tabs = paneTabs[pn];
		p.active = -1;
		if (!p.tabs.empty()) activate(pn, std::min(paneActive[pn], static_cast<int>(p.tabs.size()) - 1));
		if (previous) releaseIfUnused(previous);
	}
	const long wantedFocus = jsonLong(session, "focus");
	focus = (wantedFocus == 1 && !paneAt(1).tabs.empty()) ? 1 : 0;
	host.splitChanged(splitVisible(), stacked);
	host.focusPane(focus);
	refreshChrome();
	return true;
}

bool Workspace::tick() { return writeSession(); }

bool Workspace::prepareToQuit() {
	if (writeSession()) return true;
	return host.ask("Your unsaved work couldn't be backed up.", "Quitting now could lose changes that haven't been saved.",
	                {"Don't Quit", "Quit Anyway"}) == 1;
}

void Workspace::applicationActivated() {
	reloadOmarchyTheme();
	checkExternalChanges();
	host.foldersChanged();
}

#pragma mark - Notifications

void Workspace::editorNotification(int pane, const SCNotification &n) {
	Pane &p = paneAt(pane);
	Document *document = (p.active >= 0 && p.active < static_cast<int>(p.tabs.size())) ? p.tabs[p.active].document.get() : nullptr;
	if (!document) return;
	switch (n.nmhdr.code) {
	case SCN_SAVEPOINTLEFT:
		document->modified = true;
		refreshChrome();
		break;
	case SCN_SAVEPOINTREACHED:
		document->modified = false;
		refreshChrome();
		break;
	case SCN_MODIFIED:
		if (n.modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)) {
			document->editGeneration++;
			if (n.linesAdded != 0) {
				updateLineNumberMargin(pane);
				// The same document may be showing in the other view too.
				const Pane &other = paneAt(1 - pane);
				if (other.active >= 0 && other.tabs[other.active].document.get() == document) updateLineNumberMargin(1 - pane);
			}
		}
		break;
	case SCN_UPDATEUI:
		if (pane == focus && (n.updated & (SC_UPDATE_SELECTION | SC_UPDATE_CONTENT))) updateStatus();
		if (n.updated & (SC_UPDATE_SELECTION | SC_UPDATE_CONTENT)) updateBraceHighlight(pane);
		if (n.updated & SC_UPDATE_SELECTION) updateSmartHighlight(pane);
		if (n.updated & (SC_UPDATE_V_SCROLL | SC_UPDATE_H_SCROLL)) syncScroll(pane);
		break;
	case SCN_CHARADDED:
		if (pane == focus) autoIndent(pane, n.ch);
		break;
	case SCN_MARGINCLICK:
		if (n.margin == kMarginBookmark) {
			focus = pane;
			toggleBookmark(view(pane).send(SCI_LINEFROMPOSITION, n.position));
		}
		break;
	case SCN_MACRORECORD:
		if (pane == focus) macroRecord(n.message, n.wParam, n.lParam);
		break;
	case SCN_FOCUSIN:
		paneFocused(pane);
		break;
	default:
		break;
	}
}

void Workspace::resultsNotification(const SCNotification &n) {
	if (n.nmhdr.code == SCN_DOUBLECLICK) jumpToResultLine(n.line);
}

void Workspace::autoIndent(int pane, int ch) {
	SciView &v = view(pane);
	const int eol = static_cast<int>(v.send(SCI_GETEOLMODE));
	const bool newline = eol == SC_EOL_CR ? ch == '\r' : ch == '\n';
	if (!newline || v.send(SCI_GETSELECTIONS) != 1) return;
	const long line = v.send(SCI_LINEFROMPOSITION, v.send(SCI_GETCURRENTPOS));
	if (line < 1) return;
	// Copy the previous line's whitespace exactly, so spaces never turn into tabs or back.
	const std::string indent = rangeOf(v.send(SCI_POSITIONFROMLINE, line - 1), v.send(SCI_GETLINEINDENTPOSITION, line - 1));
	if (indent.empty()) return;
	// Playback gets its indent from the replayed newline, so recording this would double it.
	if (recording) v.send(SCI_STOPRECORD);
	v.send(SCI_REPLACESEL, 0, reinterpret_cast<intptr_t>(indent.c_str()));
	if (recording) v.send(SCI_STARTRECORD);
}

void Workspace::updateBraceHighlight(int pane) {
	SciView &v = view(pane);
	const auto isBrace = [](int c) { return c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}'; };
	const long position = v.send(SCI_GETCURRENTPOS);
	long brace = -1;
	if (position > 0 && isBrace(static_cast<int>(v.send(SCI_GETCHARAT, position - 1)))) brace = position - 1;
	else if (isBrace(static_cast<int>(v.send(SCI_GETCHARAT, position)))) brace = position;
	if (brace < 0) {
		v.send(SCI_BRACEHIGHLIGHT, INVALID_POSITION, INVALID_POSITION);
		return;
	}
	const long match = v.send(SCI_BRACEMATCH, brace);
	if (match < 0) v.send(SCI_BRACEBADLIGHT, brace);
	else v.send(SCI_BRACEHIGHLIGHT, brace, match);
}

// Highlights other visible copies of a selected whole word, like Notepad++'s smart highlighting.
void Workspace::updateSmartHighlight(int pane) {
	SciView &v = view(pane);
	Pane &p = paneAt(pane);
	const long length = v.send(SCI_GETLENGTH);
	v.send(SCI_SETINDICATORCURRENT, kIndicatorSmart);
	if (p.smartActive) {
		v.send(SCI_INDICATORCLEARRANGE, 0, length);
		p.smartActive = false;
	}
	const long start = v.send(SCI_GETSELECTIONSTART);
	const long end = v.send(SCI_GETSELECTIONEND);
	if (end <= start || end - start > 256 || v.send(SCI_GETSELECTIONS) != 1) return;
	if (v.send(SCI_WORDSTARTPOSITION, start, 1) != start || v.send(SCI_WORDENDPOSITION, start, 1) != end) return;
	const char *pointer = reinterpret_cast<const char *>(v.send(SCI_GETRANGEPOINTER, start, end - start));
	if (!pointer) return;
	const std::string word(pointer, static_cast<size_t>(end - start));
	const long firstVisible = v.send(SCI_DOCLINEFROMVISIBLE, v.send(SCI_GETFIRSTVISIBLELINE));
	const long lastLine = firstVisible + v.send(SCI_LINESONSCREEN) + 1;
	long position = v.send(SCI_POSITIONFROMLINE, firstVisible);
	const long rangeEnd = lastLine < v.send(SCI_GETLINECOUNT) ? v.send(SCI_POSITIONFROMLINE, lastLine) : length;
	const long savedStart = v.send(SCI_GETTARGETSTART);
	const long savedEnd = v.send(SCI_GETTARGETEND);
	v.send(SCI_SETSEARCHFLAGS, SCFIND_WHOLEWORD | SCFIND_MATCHCASE);
	while (position < rangeEnd) {
		v.send(SCI_SETTARGETRANGE, position, rangeEnd);
		const long found = v.send(SCI_SEARCHINTARGET, word.size(), reinterpret_cast<intptr_t>(word.data()));
		if (found < 0) break;
		const long foundEnd = v.send(SCI_GETTARGETEND);
		v.send(SCI_INDICATORFILLRANGE, found, foundEnd - found);
		p.smartActive = true;
		position = foundEnd;
	}
	v.send(SCI_SETTARGETRANGE, savedStart, savedEnd);
}

void Workspace::syncScroll(int pane) {
	if (syncing || pane != focus || !splitVisible() || (!syncVertical && !syncHorizontal)) return;
	syncing = true;
	SciView &source = view(pane);
	SciView &target = view(1 - pane);
	// The offsets are the second view minus the first, kept from when syncing was turned on.
	const long sign = pane == 0 ? 1 : -1;
	if (syncVertical) target.send(SCI_SETFIRSTVISIBLELINE, std::max(0L, source.send(SCI_GETFIRSTVISIBLELINE) + sign * syncLine));
	if (syncHorizontal) target.send(SCI_SETXOFFSET, std::max(0L, source.send(SCI_GETXOFFSET) + sign * syncColumn));
	syncing = false;
}

#pragma mark - Search

std::string Workspace::patternFrom(const FindOptions &options) const {
	return options.mode == SearchMode::Extended ? unescapeExtended(options.what) : options.what;
}

int Workspace::flagsFrom(const FindOptions &options) const {
	int flags = 0;
	if (options.matchCase) flags |= SCFIND_MATCHCASE;
	if (options.mode == SearchMode::Regex) flags |= SCFIND_REGEXP | SCFIND_CXX11REGEX;
	else if (options.wholeWord) flags |= SCFIND_WHOLEWORD;
	return flags;
}

long Workspace::searchIn(SciView &target, const std::string &pattern, long from, long to) const {
	target.send(SCI_SETTARGETRANGE, from, to);
	const long found = target.send(SCI_SEARCHINTARGET, pattern.size(), reinterpret_cast<intptr_t>(pattern.data()));
	// A bad pattern returns the same -1 as no match, and only the status tells them apart.
	if (found < 0 && target.send(SCI_GETSTATUS) == SC_STATUS_WARN_REGEX) {
		target.send(SCI_SETSTATUS, SC_STATUS_OK);
		return -2;
	}
	return found;
}

long Workspace::replaceTarget(SciView &target, const FindOptions &options) const {
	if (options.mode == SearchMode::Regex) {
		const std::string with = normalizeRegexReplacement(options.with);
		return target.send(SCI_REPLACETARGETRE, with.size(), reinterpret_cast<intptr_t>(with.data()));
	}
	const std::string with = options.mode == SearchMode::Extended ? unescapeExtended(options.with) : options.with;
	return target.send(SCI_REPLACETARGET, with.size(), reinterpret_cast<intptr_t>(with.data()));
}

SearchResult Workspace::findNext(const FindOptions &options) {
	const std::string pattern = patternFrom(options);
	if (pattern.empty()) return SearchResult::NotFound;
	SciView &v = view();
	v.send(SCI_SETSEARCHFLAGS, flagsFrom(options));
	const long length = v.send(SCI_GETLENGTH);
	const long selectionStart = v.send(SCI_GETSELECTIONSTART);
	const long selectionEnd = v.send(SCI_GETSELECTIONEND);
	long from = options.backward ? selectionStart : selectionEnd;
	// An empty match, such as ^, would find itself forever. Step past it, and at either
	// end of the document there's nothing left to step to, so go straight to wrapping.
	bool exhausted = false;
	if (selectionStart == selectionEnd && options.mode == SearchMode::Regex) {
		const long probe = searchIn(v, pattern, from, options.backward ? 0 : length);
		if (probe == -2) return SearchResult::InvalidPattern;
		if (probe == from && v.send(SCI_GETTARGETEND) == from) {
			if (options.backward) {
				if (from > 0) from = v.send(SCI_POSITIONBEFORE, from);
				else exhausted = true;
			} else {
				if (from < length) from = v.send(SCI_POSITIONAFTER, from);
				else exhausted = true;
			}
		}
	}
	long found = exhausted ? -1 : searchIn(v, pattern, from, options.backward ? 0 : length);
	if (found == -2) return SearchResult::InvalidPattern;
	if (found < 0 && options.wrap) {
		found = options.backward ? searchIn(v, pattern, length, from) : searchIn(v, pattern, 0, from);
		if (found == -2) return SearchResult::InvalidPattern;
		if (found >= 0) host.flash("Search wrapped around");
	}
	if (found < 0) return SearchResult::NotFound;
	const long start = v.send(SCI_GETTARGETSTART);
	const long end = v.send(SCI_GETTARGETEND);
	v.send(SCI_ENSUREVISIBLEENFORCEPOLICY, v.send(SCI_LINEFROMPOSITION, start));
	v.send(SCI_SETSEL, start, end);
	v.send(SCI_SCROLLCARET);
	return SearchResult::Found;
}

void Workspace::searchRange(const FindOptions &options, SciView &target, long &start, long &end) const {
	start = 0;
	end = target.send(SCI_GETLENGTH);
	if (options.inSelection) {
		const long a = target.send(SCI_GETSELECTIONSTART);
		const long b = target.send(SCI_GETSELECTIONEND);
		if (b > a) {
			start = a;
			end = b;
		}
	}
}

long Workspace::countMatches(const FindOptions &options) {
	const std::string pattern = patternFrom(options);
	if (pattern.empty()) return 0;
	SciView &v = view();
	v.send(SCI_SETSEARCHFLAGS, flagsFrom(options));
	long position, end;
	searchRange(options, v, position, end);
	long count = 0;
	while (position <= end) {
		const long found = searchIn(v, pattern, position, end);
		if (found == -2) return -1;
		if (found < 0) break;
		const long matchEnd = v.send(SCI_GETTARGETEND);
		count++;
		if (matchEnd == found) {
			if (found >= end) break;
			position = v.send(SCI_POSITIONAFTER, found);
		} else {
			position = matchEnd;
		}
	}
	return count;
}

long Workspace::replaceAllIn(SciView &v, const FindOptions &options) const {
	const std::string pattern = patternFrom(options);
	if (pattern.empty()) return 0;
	v.send(SCI_SETSEARCHFLAGS, flagsFrom(options));
	long position, end;
	searchRange(options, v, position, end);
	const long rangeStart = position;
	long count = 0;
	v.send(SCI_BEGINUNDOACTION);
	while (position <= end) {
		const long found = searchIn(v, pattern, position, end);
		if (found == -2) {
			v.send(SCI_ENDUNDOACTION);
			return -1;
		}
		if (found < 0) break;
		const long matchEnd = v.send(SCI_GETTARGETEND);
		const long replacedLength = replaceTarget(v, options);
		count++;
		const long afterReplacement = found + replacedLength;
		end += replacedLength - (matchEnd - found);
		if (matchEnd == found) {
			if (afterReplacement >= end) break;
			position = v.send(SCI_POSITIONAFTER, afterReplacement);
		} else {
			position = afterReplacement;
		}
	}
	v.send(SCI_ENDUNDOACTION);
	if (options.inSelection && count > 0) v.send(SCI_SETSEL, rangeStart, end);
	return count;
}

long Workspace::replaceAll(const FindOptions &options) { return replaceAllIn(view(), options); }

SearchResult Workspace::replaceOne(const FindOptions &options) {
	const std::string pattern = patternFrom(options);
	if (pattern.empty()) return SearchResult::NotFound;
	SciView &v = view();
	v.send(SCI_SETSEARCHFLAGS, flagsFrom(options));
	const long start = v.send(SCI_GETSELECTIONSTART);
	const long end = v.send(SCI_GETSELECTIONEND);
	// Replace only if the selection is itself a match, otherwise this just finds the next one.
	if (end > start) {
		const long found = searchIn(v, pattern, start, end);
		if (found == -2) return SearchResult::InvalidPattern;
		if (found == start && v.send(SCI_GETTARGETEND) == end) {
			const long replaced = replaceTarget(v, options);
			v.send(SCI_SETSEL, start + replaced, start + replaced);
		}
	}
	return findNext(options);
}

long Workspace::replaceAllOpenDocuments(const FindOptions &source) {
	FindOptions options = source;
	options.inSelection = false;
	long total = 0;
	for (const auto &document : documents) {
		BackgroundDocument background(host, *document);
		const long count = replaceAllIn(background.view, options);
		if (count < 0) return -1;
		if (count > 0) {
			document->modified = background.view.send(SCI_GETMODIFY) != 0;
			document->editGeneration++;
		}
		total += count;
	}
	refreshChrome();
	return total;
}

long Workspace::markAll(const FindOptions &options) {
	const std::string pattern = patternFrom(options);
	if (pattern.empty()) return 0;
	SciView &v = view();
	v.send(SCI_SETSEARCHFLAGS, flagsFrom(options));
	v.send(SCI_SETINDICATORCURRENT, kIndicatorMark);
	long position, end;
	searchRange(options, v, position, end);
	long count = 0;
	while (position <= end) {
		const long found = searchIn(v, pattern, position, end);
		if (found == -2) return -1;
		if (found < 0) break;
		const long matchEnd = v.send(SCI_GETTARGETEND);
		if (matchEnd > found) {
			v.send(SCI_INDICATORFILLRANGE, found, matchEnd - found);
			count++;
			position = matchEnd;
		} else {
			if (found >= end) break;
			position = v.send(SCI_POSITIONAFTER, found);
		}
	}
	return count;
}

void Workspace::clearMarks() {
	s(SCI_SETINDICATORCURRENT, kIndicatorMark);
	s(SCI_INDICATORCLEARRANGE, 0, s(SCI_GETLENGTH));
}

void Workspace::showResults(bool visible) {
	resultsVisible = visible;
	host.setResultsVisible(visible);
}

void Workspace::setResults(const std::string &value, std::vector<ResultEntry> entries, bool append) {
	SciView &results = host.resultsView();
	results.send(SCI_SETREADONLY, 0);
	if (!append) {
		results.send(SCI_CLEARALL);
		resultEntries.clear();
	}
	results.send(SCI_APPENDTEXT, value.size(), reinterpret_cast<intptr_t>(value.data()));
	results.send(SCI_SETREADONLY, 1);
	resultEntries.insert(resultEntries.end(), entries.begin(), entries.end());
}

std::string Workspace::resultsText() const {
	SciView &results = host.resultsView();
	const long length = results.send(SCI_GETLENGTH);
	const char *pointer = reinterpret_cast<const char *>(results.send(SCI_GETCHARACTERPOINTER));
	return (pointer && length > 0) ? std::string(pointer, static_cast<size_t>(length)) : std::string();
}

bool Workspace::jumpToResultLine(long line) {
	if (line < 0 || line >= static_cast<long>(resultEntries.size())) return false;
	const ResultEntry entry = resultEntries[line];
	if (entry.line < 0) return false;
	if (!entry.path.empty()) {
		if (!openPath(entry.path)) return false;
	} else {
		const auto document = entry.document.lock();
		if (!document) return false;
		bool shown = false;
		for (int pn : {focus, 1 - focus}) {
			const auto &list = paneAt(pn).tabs;
			for (int i = 0; i < static_cast<int>(list.size()) && !shown; i++) {
				if (list[i].document == document) {
					focus = pn;
					activate(pn, i);
					shown = true;
				}
			}
		}
		if (!shown) return false;
	}
	if (entry.line >= s(SCI_GETLINECOUNT)) return false;
	const long start = s(SCI_POSITIONFROMLINE, entry.line) + entry.offset;
	const long end = start + entry.length;
	const long limit = s(SCI_GETLENGTH);
	s(SCI_ENSUREVISIBLEENFORCEPOLICY, entry.line);
	s(SCI_SETSEL, std::min(start, limit), std::min(end, limit));
	s(SCI_SCROLLCARET);
	host.focusPane(focus);
	return true;
}

long Workspace::findAllInDocument(const FindOptions &options) {
	const std::string pattern = patternFrom(options);
	if (pattern.empty()) return 0;
	Tab *tab = activeTabPointer();
	if (!tab) return 0;
	const std::shared_ptr<Document> document = tab->document;
	s(SCI_SETSEARCHFLAGS, flagsFrom(options));
	long position, end;
	searchRange(options, view(), position, end);
	std::string out = "Search \"" + options.what + "\" in " + document->title() + "\n";
	std::vector<ResultEntry> entries(1);
	long count = 0;
	while (position <= end && count < kMaxResultLines) {
		const long found = searchIn(view(), pattern, position, end);
		if (found == -2) return -1;
		if (found < 0) break;
		const long matchEnd = s(SCI_GETTARGETEND);
		const long line = s(SCI_LINEFROMPOSITION, found);
		const long lineStart = s(SCI_POSITIONFROMLINE, line);
		out += "  Line " + std::to_string(line + 1) + ": " + truncateUtf8(rangeOf(lineStart, s(SCI_GETLINEENDPOSITION, line)), 400) + "\n";
		ResultEntry entry;
		entry.document = document;
		entry.line = line;
		entry.offset = found - lineStart;
		entry.length = matchEnd - found;
		entries.push_back(entry);
		count++;
		if (matchEnd == found) {
			if (found >= end) break;
			position = s(SCI_POSITIONAFTER, found);
		} else {
			position = matchEnd;
		}
	}
	setResults(out, std::move(entries), false);
	showResults(true);
	return count;
}

#pragma mark - Find in Files

namespace {

struct SearchJob {
	Host *host = nullptr;
	Workspace *workspace = nullptr;
	std::shared_ptr<std::atomic_bool> cancel;
	std::shared_ptr<bool> alive;
	long generation = 0;
	const long *currentGeneration = nullptr;
	FindOptions options;
	std::vector<std::string> filters;
	std::unique_ptr<std::wregex> regex;
	std::u32string literal;
	std::string literalBytes;
	std::function<void(const std::string &, std::vector<ResultEntry>)> emit;
	std::function<void(long, long, bool)> finish;
};

void searchLine(const SearchJob &job, const std::string &lineBytes, long lineNumber, const std::string &path,
                std::string &block, std::vector<ResultEntry> &entries, long &fileMatches) {
	const std::u32string line = utf8ToCodePoints(lineBytes);
	std::vector<std::pair<size_t, size_t>> matches;  // Code point start and length.
	if (job.regex) {
		if (line.size() > kMaxRegexLine) return;
		const std::wstring wide(line.begin(), line.end());
		try {
			for (std::wsregex_iterator it(wide.begin(), wide.end(), *job.regex), last; it != last; ++it)
				if (it->length() > 0) matches.emplace_back(static_cast<size_t>(it->position()), static_cast<size_t>(it->length()));
		} catch (const std::regex_error &) {
			return;
		}
	} else {
		const std::u32string &needle = job.literal;
		const bool matchCase = job.options.matchCase;
		if (needle.empty() || line.size() < needle.size()) return;
		size_t i = 0;
		while (i + needle.size() <= line.size()) {
			bool equal = true;
			for (size_t k = 0; k < needle.size(); k++) {
				const char32_t c = matchCase ? line[i + k] : foldCodePoint(line[i + k]);
				if (c != needle[k]) {
					equal = false;
					break;
				}
			}
			if (equal && job.options.wholeWord) {
				const bool left = i > 0 && isWordCodePoint(line[i - 1]);
				const bool right = i + needle.size() < line.size() && isWordCodePoint(line[i + needle.size()]);
				equal = !left && !right;
			}
			if (equal) {
				matches.emplace_back(i, needle.size());
				i += needle.size();
			} else {
				i++;
			}
		}
	}
	if (matches.empty()) return;
	size_t cursorCode = 0, cursorByte = 0;
	const std::string shown = truncateUtf8(lineBytes, 400);
	for (const auto &match : matches) {
		while (cursorCode < match.first) cursorByte += utf8Size(line[cursorCode++]);
		size_t lengthBytes = 0;
		for (size_t k = 0; k < match.second; k++) lengthBytes += utf8Size(line[match.first + k]);
		if (fileMatches == 0) {
			block += path + "\n";
			entries.emplace_back();
		}
		block += "  Line " + std::to_string(lineNumber + 1) + ": " + shown + "\n";
		ResultEntry entry;
		entry.path = path;
		entry.line = lineNumber;
		entry.offset = static_cast<long>(cursorByte);
		entry.length = static_cast<long>(lengthBytes);
		entries.push_back(entry);
		fileMatches++;
	}
}

void *searchThreadMain(void *argument) {
	std::unique_ptr<SearchJob> job(static_cast<SearchJob *>(argument));
	long matches = 0, filesWithMatches = 0;
	bool hitLimit = false;
	const FindOptions &o = job->options;

	std::error_code error;
	fs::recursive_directory_iterator walker(o.directory, fs::directory_options::skip_permission_denied, error);
	const fs::recursive_directory_iterator walkerEnd;
	while (!error && walker != walkerEnd) {
		if (job->cancel->load()) break;
		if (matches >= kMaxResultLines) {
			hitLimit = true;
			break;
		}
		const fs::directory_entry entry = *walker;
		const std::string name = entry.path().filename().string();
		const bool hidden = !name.empty() && name[0] == '.';
		std::error_code entryError;
		const auto status = entry.symlink_status(entryError);
		if (fs::is_directory(status)) {
			if ((hidden && !o.includeHidden) || !o.recursive) walker.disable_recursion_pending();
			walker.increment(error);
			continue;
		}
		const bool wanted = fs::is_regular_file(status) && (!hidden || o.includeHidden) && entry.file_size(entryError) <= kMaxSearchBytes && !entryError;
		bool accepted = wanted;
		if (wanted && !job->filters.empty()) {
			accepted = false;
			for (const std::string &filter : job->filters) {
#ifdef FNM_CASEFOLD
				const int flags = FNM_CASEFOLD;
#else
				const int flags = 0;
#endif
				if (fnmatch(filter.c_str(), name.c_str(), flags) == 0) {
					accepted = true;
					break;
				}
			}
		}
		const std::string path = entry.path().string();
		walker.increment(error);
		if (!accepted) continue;

		std::ifstream in(path, std::ios::binary);
		if (!in) continue;
		std::stringstream buffer;
		buffer << in.rdbuf();
		const std::string bytes = buffer.str();
		if (bytes.empty()) continue;
		const unsigned char *u = reinterpret_cast<const unsigned char *>(bytes.data());
		const bool hasBom = bytes.size() >= 2 && ((u[0] == 0xFF && u[1] == 0xFE) || (u[0] == 0xFE && u[1] == 0xFF));
		if (!hasBom && std::memchr(bytes.data(), 0, std::min<size_t>(bytes.size(), 8192))) continue;  // Binary.
		const Decoded decoded = decodeAuto(bytes);
		// A quick whole-file check avoids decoding every line of files that can't match.
		if (!job->regex && o.matchCase && decoded.utf8.find(job->literalBytes) == std::string::npos) continue;

		std::string block;
		std::vector<ResultEntry> entries;
		long fileMatches = 0;
		long lineNumber = 0;
		const std::string &contents = decoded.utf8;
		size_t start = 0;
		while (start <= contents.size() && !job->cancel->load()) {
			size_t end = contents.find_first_of("\r\n", start);
			const bool last = end == std::string::npos;
			if (last) end = contents.size();
			if (!(last && start == contents.size())) searchLine(*job, contents.substr(start, end - start), lineNumber, path, block, entries, fileMatches);
			if (last) break;
			start = end + ((contents[end] == '\r' && end + 1 < contents.size() && contents[end + 1] == '\n') ? 2 : 1);
			lineNumber++;
		}
		if (fileMatches > 0) {
			matches += fileMatches;
			filesWithMatches++;
			job->emit(block, std::move(entries));
		}
	}
	job->finish(matches, filesWithMatches, hitLimit);
	return nullptr;
}

}  // namespace

void Workspace::joinSearchThread() {
	if (searchThreadRunning) {
		pthread_join(searchThread, nullptr);
		searchThreadRunning = false;
	}
}

void Workspace::stopFindInFiles() {
	if (searchCancel) searchCancel->store(true);
}

void Workspace::findInFiles(const FindOptions &options, std::function<void(long, long)> completion) {
	stopFindInFiles();
	joinSearchThread();
	auto job = std::make_unique<SearchJob>();
	job->options = options;
	if (options.mode == SearchMode::Regex) {
		const std::u32string pattern = utf8ToCodePoints(options.wholeWord ? "\\b(?:" + options.what + ")\\b" : options.what);
		try {
			auto flags = std::regex::ECMAScript;
			if (!options.matchCase) flags |= std::regex::icase;
			job->regex = std::make_unique<std::wregex>(std::wstring(pattern.begin(), pattern.end()), flags);
		} catch (const std::regex_error &) {
			completion(-1, 0);
			return;
		}
	} else {
		job->literalBytes = options.mode == SearchMode::Extended ? unescapeExtended(options.what) : options.what;
		job->literal = utf8ToCodePoints(job->literalBytes);
		if (!options.matchCase)
			for (char32_t &c : job->literal) c = foldCodePoint(c);
	}
	std::istringstream filterWords(options.filters);
	std::string filter;
	while (filterWords >> filter) job->filters.push_back(filter);

	searchCancel = std::make_shared<std::atomic_bool>(false);
	job->cancel = searchCancel;
	const long generation = ++searchGeneration;
	setResults("Search \"" + options.what + "\" in " + options.directory + "\n", std::vector<ResultEntry>(1), false);

	Host *hostPointer = &host;
	std::shared_ptr<bool> guard = alive;
	job->emit = [this, hostPointer, guard, generation](const std::string &block, std::vector<ResultEntry> entries) {
		auto moved = std::make_shared<std::vector<ResultEntry>>(std::move(entries));
		hostPointer->post([this, guard, generation, block, moved]() {
			if (*guard && generation == searchGeneration) setResults(block, std::move(*moved), true);
		});
	};
	job->finish = [this, hostPointer, guard, generation, completion](long matches, long files, bool hitLimit) {
		hostPointer->post([this, guard, generation, completion, matches, files, hitLimit]() {
			if (!*guard || generation != searchGeneration) return;
			if (hitLimit) setResults("Stopped at the result limit. Narrow the search to see the rest.\n", std::vector<ResultEntry>(1), true);
			completion(matches, files);
		});
	};

	pthread_attr_t attributes;
	pthread_attr_init(&attributes);
	pthread_attr_setstacksize(&attributes, 512 * 1024 * 1024);  // Only reserved, not used up front.
	SearchJob *raw = job.release();
	if (pthread_create(&searchThread, &attributes, searchThreadMain, raw) != 0) {
		pthread_attr_setstacksize(&attributes, 64 * 1024 * 1024);
		if (pthread_create(&searchThread, &attributes, searchThreadMain, raw) != 0) {
			delete raw;
			pthread_attr_destroy(&attributes);
			completion(0, 0);
			return;
		}
	}
	pthread_attr_destroy(&attributes);
	searchThreadRunning = true;
}

#pragma mark - The Find panel's buttons

namespace {
std::string matchCount(const char *prefix, long count) {
	return std::string(prefix) + std::to_string(count) + (count == 1 ? " match" : " matches");
}
}  // namespace

void Workspace::findPanelFindNext() {
	switch (findNext(host.currentFindOptions())) {
	case SearchResult::Found: host.setFindStatus("", false); break;
	case SearchResult::NotFound: host.setFindStatus("Can't find that text.", true); break;
	case SearchResult::InvalidPattern: host.setFindStatus("That regular expression isn't valid.", true); break;
	}
}

void Workspace::findPanelCount() {
	const long count = countMatches(host.currentFindOptions());
	if (count < 0) host.setFindStatus("That regular expression isn't valid.", true);
	else host.setFindStatus(matchCount("", count), false);
}

void Workspace::findPanelFindAll() {
	const long count = findAllInDocument(host.currentFindOptions());
	if (count < 0) host.setFindStatus("That regular expression isn't valid.", true);
	else host.setFindStatus(matchCount("", count) + ". Double-click a result to go to it.", false);
}

void Workspace::findPanelReplace() {
	switch (replaceOne(host.currentFindOptions())) {
	case SearchResult::Found: host.setFindStatus("", false); break;
	case SearchResult::NotFound: host.setFindStatus("No more matches.", false); break;
	case SearchResult::InvalidPattern: host.setFindStatus("That regular expression isn't valid.", true); break;
	}
}

void Workspace::findPanelReplaceAll() {
	const long count = replaceAll(host.currentFindOptions());
	if (count < 0) host.setFindStatus("That regular expression isn't valid.", true);
	else host.setFindStatus(matchCount("Replaced ", count), false);
}

void Workspace::findPanelReplaceAllOpen() {
	const int choice = host.ask("Replace in all open documents?",
	                            "This changes up to " + std::to_string(documents.size()) + " open documents. You can undo it in each tab.",
	                            {"Replace All", "Cancel"});
	if (choice != 0) return;
	const long count = replaceAllOpenDocuments(host.currentFindOptions());
	if (count < 0) host.setFindStatus("That regular expression isn't valid.", true);
	else host.setFindStatus(matchCount("Replaced ", count), false);
}

void Workspace::findPanelMarkAll() {
	const long count = markAll(host.currentFindOptions());
	if (count < 0) host.setFindStatus("That regular expression isn't valid.", true);
	else host.setFindStatus(matchCount("Marked ", count), false);
}

void Workspace::findPanelClearMarks() {
	clearMarks();
	host.setFindStatus("", false);
}

void Workspace::findPanelFindInFiles() {
	FindOptions options = host.currentFindOptions();
	if (options.what.empty()) return;
	host.setFindInFilesRunning(true);
	host.setFindStatus("Searching…", false);
	showResults(true);
	Host *hostPointer = &host;
	findInFiles(options, [hostPointer](long matches, long files) {
		hostPointer->setFindInFilesRunning(false);
		if (matches < 0) {
			hostPointer->setFindStatus("That regular expression isn't valid.", true);
			return;
		}
		hostPointer->setFindStatus(std::to_string(matches) + (matches == 1 ? " match in " : " matches in ") + std::to_string(files) +
		                           (files == 1 ? " file" : " files") + ". Double-click a result to go to it.", false);
	});
}

#pragma mark - Encoding and language

bool Workspace::convertActiveToEncoding(const std::string &identifier) {
	const Encoding *target = encodingWithIdentifier(identifier);
	Document *document = activeDocument();
	if (!target || !document) return false;
	if (target == document->encoding) return true;
	std::string bytes;
	if (!encode(text(), target, bytes)) {
		host.ask("This text can't be converted to that encoding.",
		         std::string("Some characters don't exist in ") + target->name + ", so the document stays in " + document->encoding->name + ".", {"OK"});
		return false;
	}
	document->encoding = target;
	document->forceDirty = true;
	refreshChrome();
	return true;
}

bool Workspace::reinterpretActiveAsEncoding(const std::string &identifier) {
	const Encoding *target = encodingWithIdentifier(identifier);
	Document *document = activeDocument();
	if (!target || !document) return false;
	if (document->path.empty()) {
		host.ask("Only a saved file can be reopened in another encoding.", "To change how this text is saved, use Convert To instead.", {"OK"});
		return false;
	}
	if (document->dirty()) {
		const int choice = host.ask("Discard unsaved changes?", "Reading the file in another encoding reloads it from disk.", {"Cancel", "Discard and Reload"});
		if (choice != 1) return false;
	}
	std::string bytes, utf8;
	if (!readFile(document->path, bytes, true)) return false;
	if (!decodeAs(bytes, target, utf8)) {
		host.ask("The file isn't valid in that encoding.", std::string("Its bytes can't be read as ") + target->name + ". Nothing changed.", {"OK"});
		return false;
	}
	document->encoding = target;
	document->hasMtime = fileMTime(document->path, document->mtime);
	loadInto(*document, utf8);
	s(SCI_SETEOLMODE, static_cast<int>(document->eol));
	removeBackup(*document);
	refreshChrome();
	return true;
}

void Workspace::setActiveLanguage(const std::string &name) {
	Document *document = activeDocument();
	if (!document) return;
	document->language = catalog.languageNamed(name);
	document->languageChosen = true;
	applyLanguageTo(0);
	applyLanguageTo(1);
	refreshChrome();
}

#pragma mark - Folder as Workspace

bool Workspace::addFolder(const std::string &rawPath) {
	std::string path = expandPath(rawPath);
	while (path.size() > 1 && path.back() == '/') path.pop_back();
	std::error_code error;
	if (!fs::is_directory(path, error)) {
		host.ask("That folder can't be opened.", path, {"OK"});
		return false;
	}
	if (std::find(folderRoots.begin(), folderRoots.end(), path) == folderRoots.end()) folderRoots.push_back(path);
	setFolderExpanded(path, true);
	setFolderPanelVisible(true);
	host.foldersChanged();
	return true;
}

void Workspace::removeFolder(const std::string &path) {
	folderRoots.erase(std::remove(folderRoots.begin(), folderRoots.end(), path), folderRoots.end());
	if (folderRoots.empty()) setFolderPanelVisible(false);
	host.foldersChanged();
}

void Workspace::setFolderPanelVisible(bool visible) {
	folderPanel = visible;
	host.setFolderPanelVisible(visible);
}

bool Workspace::isFolderExpanded(const std::string &path) const {
	return std::find(expandedFolders.begin(), expandedFolders.end(), path) != expandedFolders.end();
}

void Workspace::setFolderExpanded(const std::string &path, bool expanded) {
	const bool present = isFolderExpanded(path);
	if (expanded && !present) expandedFolders.push_back(path);
	if (!expanded && present) expandedFolders.erase(std::remove(expandedFolders.begin(), expandedFolders.end(), path), expandedFolders.end());
}

std::vector<FolderEntry> Workspace::listFolder(const std::string &path) const {
	std::vector<FolderEntry> out;
	std::error_code error;
	for (fs::directory_iterator it(path, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error)) {
		FolderEntry entry;
		entry.name = it->path().filename().string();
		if (!settings.showHiddenInFolders && !entry.name.empty() && entry.name[0] == '.') continue;
		entry.path = it->path().string();
		std::error_code statusError;
		entry.isSymlink = it->is_symlink(statusError);
		// A link to a folder lists as a file, so a link loop can't expand forever.
		entry.isDirectory = !entry.isSymlink && it->is_directory(statusError);
		out.push_back(entry);
	}
	const auto folded = [](const std::string &s) {
		std::string lowered = s;
		std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return lowered;
	};
	std::sort(out.begin(), out.end(), [&](const FolderEntry &a, const FolderEntry &b) {
		if (a.isDirectory != b.isDirectory) return a.isDirectory;
		const std::string x = folded(a.name), y = folded(b.name);
		return x != y ? x < y : a.name < b.name;
	});
	return out;
}

bool Workspace::createFileIn(const std::string &directory, const std::string &name, std::string &error) {
	if (!validName(name, error)) return false;
	const std::string path = (fs::path(directory) / name).string();
	const int fd = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0666);
	if (fd < 0) {
		error = errno == EEXIST ? "Something with that name already exists." : std::strerror(errno);
		return false;
	}
	close(fd);
	host.foldersChanged();
	return openPath(path);
}

bool Workspace::createFolderIn(const std::string &directory, const std::string &name, std::string &error) {
	if (!validName(name, error)) return false;
	const std::string path = (fs::path(directory) / name).string();
	if (mkdir(path.c_str(), 0777) != 0) {
		error = errno == EEXIST ? "Something with that name already exists." : std::strerror(errno);
		return false;
	}
	setFolderExpanded(directory, true);
	host.foldersChanged();
	return true;
}

bool Workspace::renamePath(const std::string &path, const std::string &newName, std::string &error) {
	if (!validName(newName, error)) return false;
	const std::string target = (fs::path(path).parent_path() / newName).string();
	if (target == path) return true;
	std::error_code fsError;
	if (fs::symlink_status(target, fsError).type() != fs::file_type::not_found) {
		error = "Something with that name already exists.";
		return false;
	}
	if (rename(path.c_str(), target.c_str()) != 0) {
		error = std::strerror(errno);
		return false;
	}
	// Open documents follow the file, whether it or a folder above it was renamed.
	const std::string prefix = path + "/";
	const auto moved = [&](const std::string &value) -> std::string {
		if (value == path) return target;
		if (value.rfind(prefix, 0) == 0) return target + "/" + value.substr(prefix.size());
		return value;
	};
	for (const auto &document : documents) {
		const std::string updated = moved(document->path);
		if (updated == document->path) continue;
		document->path = updated;
		if (!document->languageChosen) document->language = detectLanguage(updated, textOf(*document));
	}
	for (std::string &root : folderRoots) root = moved(root);
	for (std::string &folder : expandedFolders) folder = moved(folder);
	applyLanguageTo(0);
	applyLanguageTo(1);
	host.foldersChanged();
	refreshChrome();
	return true;
}

void Workspace::runFolderCommand(Cmd cmd, const std::string &path) {
	std::string error;
	std::string name;
	switch (cmd) {
	case Cmd::FolderNewFile:
		if (host.promptText("New File", "Name:", "", name) && !createFileIn(path, name, error)) host.ask("The file could not be created.", error, {"OK"});
		break;
	case Cmd::FolderNewFolder:
		if (host.promptText("New Folder", "Name:", "", name) && !createFolderIn(path, name, error)) host.ask("The folder could not be created.", error, {"OK"});
		break;
	case Cmd::FolderRename:
		if (host.promptText("Rename", "New name:", baseName(path), name) && !renamePath(path, name, error)) host.ask("That could not be renamed.", error, {"OK"});
		break;
	case Cmd::FolderTrash: {
		if (host.ask("Move “" + baseName(path) + "” to the Trash?", "", {"Cancel", "Move to Trash"}) != 1) break;
		if (!host.moveToTrash(path, error)) {
			host.ask("That could not be moved to the Trash.", error, {"OK"});
			break;
		}
		// Clean tabs for it close. Unsaved ones stay, marked as no longer on disk.
		const std::string prefix = path + "/";
		const auto snapshot = documents;
		for (const auto &document : snapshot) {
			if (document->path != path && document->path.rfind(prefix, 0) != 0) continue;
			if (document->dirty()) {
				document->missing = true;
				document->forceDirty = true;
				continue;
			}
			for (int pn = 0; pn < 2; pn++) {
				const auto &list = paneAt(pn).tabs;
				for (int i = static_cast<int>(list.size()) - 1; i >= 0; i--)
					if (list[i].document == document) closeTab(pn, i);
			}
		}
		host.foldersChanged();
		refreshChrome();
		break;
	}
	case Cmd::FolderCopyPath:
		host.setClipboard(path);
		break;
	case Cmd::FolderReveal:
		host.reveal(path);
		break;
	case Cmd::FolderFindInFiles:
		host.showFind(FindTab::Files, selectionOrWord(), path);
		break;
	case Cmd::FolderRemoveRoot:
		removeFolder(path);
		break;
	default:
		break;
	}
}

#pragma mark - Editing helpers

long Workspace::currentLine() const { return s(SCI_LINEFROMPOSITION, s(SCI_GETCURRENTPOS)); }

// The lines covered by the selection. A selection ending at the start of a line doesn't
// count that line. With `wholeDocumentIfEmpty`, no selection means the entire document.
void Workspace::selectedLines(long &start, long &end, bool wholeDocumentIfEmpty) const {
	const long selectionStart = s(SCI_GETSELECTIONSTART);
	const long selectionEnd = s(SCI_GETSELECTIONEND);
	if (selectionStart == selectionEnd && wholeDocumentIfEmpty) {
		start = 0;
		end = s(SCI_GETLENGTH);
		return;
	}
	const long firstLine = s(SCI_LINEFROMPOSITION, selectionStart);
	long lastLine = s(SCI_LINEFROMPOSITION, selectionEnd);
	if (lastLine > firstLine && s(SCI_POSITIONFROMLINE, lastLine) == selectionEnd) lastLine--;
	start = s(SCI_POSITIONFROMLINE, firstLine);
	end = s(SCI_GETLINEENDPOSITION, lastLine);
}

void Workspace::replaceRange(long start, long end, const std::string &value, bool select) {
	if (rangeOf(start, end) == value) return;
	s(SCI_BEGINUNDOACTION);
	s(SCI_SETTARGETRANGE, start, end);
	s(SCI_REPLACETARGET, value.size(), reinterpret_cast<intptr_t>(value.data()));
	s(SCI_ENDUNDOACTION);
	if (select) s(SCI_SETSEL, start, start + static_cast<long>(value.size()));
}

void Workspace::lineOperation(LineOp op) {
	long start, end;
	selectedLines(start, end, true);
	const bool hadSelection = s(SCI_GETSELECTIONSTART) != s(SCI_GETSELECTIONEND);
	replaceRange(start, end, applyLineOp(rangeOf(start, end), op, activeDocument()->eol), hadSelection);
}

void Workspace::comment(CommentMode mode) {
	const Language *language = activeDocument()->language;
	if (language->commentLine.empty()) {
		host.flash(language->displayName + " has no line comment syntax");
		return;
	}
	long start, end;
	selectedLines(start, end, false);
	const long firstLine = s(SCI_LINEFROMPOSITION, start);
	const long lastLine = s(SCI_LINEFROMPOSITION, end);
	std::vector<std::string> lines;
	for (long line = firstLine; line <= lastLine; line++) lines.push_back(rangeOf(s(SCI_POSITIONFROMLINE, line), s(SCI_GETLINEENDPOSITION, line)));
	const std::vector<std::string> updated = commentLines(lines, language->commentLine, mode);
	const bool hadSelection = s(SCI_GETSELECTIONSTART) != s(SCI_GETSELECTIONEND);
	s(SCI_BEGINUNDOACTION);
	// From the bottom up, so earlier line positions stay valid.
	for (long i = static_cast<long>(updated.size()) - 1; i >= 0; i--) {
		const long line = firstLine + i;
		s(SCI_SETTARGETRANGE, s(SCI_POSITIONFROMLINE, line), s(SCI_GETLINEENDPOSITION, line));
		s(SCI_REPLACETARGET, updated[i].size(), reinterpret_cast<intptr_t>(updated[i].data()));
	}
	s(SCI_ENDUNDOACTION);
	if (hadSelection) s(SCI_SETSEL, start, s(SCI_GETLINEENDPOSITION, lastLine));
}

void Workspace::blockComment() {
	const Language *language = activeDocument()->language;
	if (language->commentStart.empty() || language->commentEnd.empty()) {
		host.flash(language->displayName + " has no block comment syntax");
		return;
	}
	const long start = s(SCI_GETSELECTIONSTART);
	const long end = s(SCI_GETSELECTIONEND);
	if (start == end) return;
	const std::string &open = language->commentStart;
	const std::string &close = language->commentEnd;
	const std::string value = rangeOf(start, end);
	std::string out;
	if (value.size() >= open.size() + close.size() && value.compare(0, open.size(), open) == 0 &&
	    value.compare(value.size() - close.size(), close.size(), close) == 0) {
		out = value.substr(open.size(), value.size() - open.size() - close.size());
	} else {
		out = open + value + close;
	}
	replaceRange(start, end, out, true);
}

void Workspace::toggleBookmark(long line) {
	if (s(SCI_MARKERGET, line) & (1 << kMarkerBookmark)) s(SCI_MARKERDELETE, line, kMarkerBookmark);
	else s(SCI_MARKERADD, line, kMarkerBookmark);
}

void Workspace::goToBookmark(bool forward) {
	const long mask = 1 << kMarkerBookmark;
	long found = forward ? s(SCI_MARKERNEXT, currentLine() + 1, mask) : s(SCI_MARKERPREVIOUS, currentLine() - 1, mask);
	if (found < 0) found = forward ? s(SCI_MARKERNEXT, 0, mask) : s(SCI_MARKERPREVIOUS, s(SCI_GETLINECOUNT) - 1, mask);
	if (found < 0) return;
	s(SCI_ENSUREVISIBLEENFORCEPOLICY, found);
	s(SCI_GOTOLINE, found);
}

void Workspace::removeLines(bool keepBookmarked) {
	const long mask = 1 << kMarkerBookmark;
	s(SCI_BEGINUNDOACTION);
	for (long line = s(SCI_GETLINECOUNT) - 1; line >= 0; line--) {
		const bool bookmarked = (s(SCI_MARKERGET, line) & mask) != 0;
		if (bookmarked == keepBookmarked) continue;
		const long start = s(SCI_POSITIONFROMLINE, line);
		const long end = s(SCI_POSITIONFROMLINE, line + 1);
		if (end > start) {
			s(SCI_DELETERANGE, start, end - start);
		} else if (line > 0) {
			// The last line has no line ending of its own, so remove the one before it.
			const long previousEnd = s(SCI_GETLINEENDPOSITION, line - 1);
			s(SCI_DELETERANGE, previousEnd, s(SCI_GETLINEENDPOSITION, line) - previousEnd);
		}
	}
	s(SCI_ENDUNDOACTION);
}

void Workspace::foldLevel(int level, bool expand) {
	s(SCI_COLOURISE, 0, -1);
	const long lines = s(SCI_GETLINECOUNT);
	for (long line = 0; line < lines; line++) {
		const long value = s(SCI_GETFOLDLEVEL, line);
		if ((value & SC_FOLDLEVELHEADERFLAG) && (value & SC_FOLDLEVELNUMBERMASK) - SC_FOLDLEVELBASE == level - 1)
			s(SCI_FOLDLINE, line, expand ? SC_FOLDACTION_EXPAND : SC_FOLDACTION_CONTRACT);
	}
}

void Workspace::summary() {
	const Document *document = activeDocument();
	const std::string content = text();
	long words = 0;
	bool inWord = false;
	for (char32_t cp : utf8ToCodePoints(content)) {
		const bool word = isWordCodePoint(cp);
		if (word && !inWord) words++;
		inWord = word;
	}
	host.ask("Summary",
	         "Path: " + (document->path.empty() ? std::string("(not saved)") : document->path) +
	         "\nCharacters: " + std::to_string(s(SCI_COUNTCHARACTERS, 0, s(SCI_GETLENGTH))) +
	         "\nWords: " + std::to_string(words) +
	         "\nLines: " + std::to_string(s(SCI_GETLINECOUNT)) +
	         "\nBytes in memory (UTF-8): " + std::to_string(content.size()) +
	         "\nEncoding: " + document->encoding->name +
	         "\nLine endings: " + eolName(document->eol), {"OK"});
}

void Workspace::goToLine() {
	std::string answer;
	if (!host.promptText("Go to Line", "Line (1 – " + std::to_string(s(SCI_GETLINECOUNT)) + "):", "", answer)) return;
	const long line = std::strtol(answer.c_str(), nullptr, 10);
	if (line < 1) return;
	const long target = std::min<long>(line - 1, s(SCI_GETLINECOUNT) - 1);
	s(SCI_ENSUREVISIBLEENFORCEPOLICY, target);
	s(SCI_GOTOLINE, target);
}

void Workspace::hashSelection(HashKind kind) {
	const long start = s(SCI_GETSELECTIONSTART);
	const long end = s(SCI_GETSELECTIONEND);
	if (start == end) {
		host.flash("Select some text to hash first");
		return;
	}
	const std::string value = rangeOf(start, end);
	const std::string digest = hashHex(value.data(), value.size(), kind);
	host.setClipboard(digest);
	host.flash("Copied " + digest);
}

void Workspace::hashFiles(HashKind kind) {
	std::vector<std::string> paths;
	if (!host.chooseFilesToOpen(std::string(), paths)) return;
	std::string out;
	for (const std::string &path : paths) {
		std::string bytes;
		if (readFile(path, bytes, false)) out += hashHex(bytes.data(), bytes.size(), kind) + "  " + baseName(path) + "\n";
		else out += "(unreadable)  " + path + "\n";
	}
	host.setClipboard(out);
	host.ask("Copied to the clipboard", out, {"OK"});
}

void Workspace::macroRecord(unsigned message, uintptr_t w, intptr_t l) {
	if (!recording) return;
	// These messages pass text through a pointer that won't outlive the notification.
	const bool hasText = message == SCI_REPLACESEL || message == SCI_ADDTEXT || message == SCI_INSERTTEXT || message == SCI_APPENDTEXT;
	std::string value;
	if (hasText && l) {
		const char *pointer = reinterpret_cast<const char *>(l);
		value = (message == SCI_ADDTEXT || message == SCI_APPENDTEXT) ? std::string(pointer, w) : std::string(pointer);
	}
	macro.push_back({message, w, hasText ? 0 : l, value});
}

void Workspace::macroPlayOnce() {
	for (const MacroStep &step : macro)
		s(step.message, step.w, step.text.empty() ? step.l : reinterpret_cast<intptr_t>(step.text.c_str()));
}

std::string Workspace::selectionOrWord() const {
	const std::string selected = selectedText();
	if (!selected.empty()) return selected;
	const long position = s(SCI_GETCURRENTPOS);
	return rangeOf(s(SCI_WORDSTARTPOSITION, position, 1), s(SCI_WORDENDPOSITION, position, 1));
}

void Workspace::runFromMenu(bool backward, bool useSelection) {
	if (useSelection) {
		const std::string word = selectionOrWord();
		if (!word.empty()) host.setFindText(word);
	}
	FindOptions options = host.currentFindOptions();
	if (options.what.empty()) {
		host.showFind(FindTab::Find, selectionOrWord(), std::string());
		return;
	}
	options.backward = backward;
	options.inSelection = false;
	const SearchResult result = findNext(options);
	if (result == SearchResult::NotFound) host.flash("Can't find “" + options.what + "”");
	else if (result == SearchResult::InvalidPattern) host.flash("That regular expression isn't valid");
}

#pragma mark - Commands

namespace {
int encodeTab(int pane, int index) { return pane * kPaneArgFactor + index; }
}  // namespace

void Workspace::run(Cmd cmd, int arg, const std::string &argument) {
	Document *document = activeDocument();
	if (!document) return;
	const int tabPane = arg / kPaneArgFactor;
	const int tabIndex = arg % kPaneArgFactor;
	const auto applyPreferences = [&]() {
		applyViewPreferences(0);
		applyViewPreferences(1);
		saveSettings();
	};
	const auto resetZoomMargins = [&]() {
		paneAt(focus).lineDigits = 0;
		updateLineNumberMargin(focus);
	};
	const auto copyToClipboardIfAny = [&](const std::string &value) {
		if (!value.empty()) host.setClipboard(value);
	};
	switch (cmd) {
	case Cmd::None: break;
	case Cmd::New: createUntitledDocument(); break;
	case Cmd::Open: {
		std::vector<std::string> paths;
		if (host.chooseFilesToOpen(document->path.empty() ? std::string() : directoryOf(document->path), paths)) openFiles(paths);
		break;
	}
	case Cmd::OpenRecent:
		if (!openPath(argument)) {
			settings.recent.erase(std::remove(settings.recent.begin(), settings.recent.end(), argument), settings.recent.end());
			saveSettings();
		}
		break;
	case Cmd::ClearRecent:
		settings.recent.clear();
		saveSettings();
		break;
	case Cmd::OpenFolder: {
		std::string path;
		if (host.chooseFolder(document->path.empty() ? std::string() : directoryOf(document->path), path)) addFolder(path);
		break;
	}
	case Cmd::Reload:
		if (document->path.empty()) break;
		if (document->dirty() && host.ask("Reload “" + document->title() + "” from disk?", "This discards your unsaved changes and can't be undone.", {"Cancel", "Reload"}) != 1) break;
		reloadActive();
		break;
	case Cmd::Save: saveActive(); break;
	case Cmd::SaveAs: saveAsDialog(*document); break;
	case Cmd::SaveCopy: {
		std::string path, bytes, error;
		if (!host.chooseSavePath(document->title(), std::string(), path)) break;
		if (!encode(text(), document->encoding, bytes)) host.ask("This text can't be saved in the current encoding.", "Nothing was written.", {"OK"});
		else if (!writeFileAtomically(bytes, path, error)) host.ask("The copy could not be saved.", error, {"OK"});
		break;
	}
	case Cmd::SaveAll: {
		const auto snapshot = documents;
		for (const auto &d : snapshot) {
			if (!d->dirty()) continue;
			if (!d->path.empty()) {
				if (!saveDocument(*d, d->path)) return;
			} else if (!saveAsDialog(*d)) {
				return;
			}
		}
		break;
	}
	case Cmd::Close: closeActive(); break;
	case Cmd::CloseAll: {
		std::vector<std::pair<int, std::shared_ptr<Document>>> targets;
		for (int pn = 1; pn >= 0; pn--)
			for (const Tab &tab : paneAt(pn).tabs) targets.emplace_back(pn, tab.document);
		closeTabs(targets);
		break;
	}
	case Cmd::CloseOthers: {
		std::vector<std::pair<int, std::shared_ptr<Document>>> targets;
		const Pane &p = paneAt(focus);
		for (int i = 0; i < static_cast<int>(p.tabs.size()); i++)
			if (i != p.active) targets.emplace_back(focus, p.tabs[i].document);
		closeTabs(targets);
		break;
	}
	case Cmd::CloseUnchanged: {
		std::vector<std::pair<int, std::shared_ptr<Document>>> targets;
		for (int pn = 1; pn >= 0; pn--)
			for (const Tab &tab : paneAt(pn).tabs)
				if (!tab.document->dirty()) targets.emplace_back(pn, tab.document);
		closeTabs(targets);
		break;
	}
	case Cmd::Reveal:
		if (!document->path.empty()) host.reveal(document->path);
		break;
	case Cmd::Trash: {
		if (document->path.empty()) break;
		if (host.ask("Move “" + document->title() + "” to the Trash?", document->dirty() ? "Unsaved changes in this tab will be lost." : "", {"Cancel", "Move to Trash"}) != 1) break;
		std::string error;
		if (!host.moveToTrash(document->path, error)) {
			host.ask("The file could not be moved to the Trash.", error, {"OK"});
			break;
		}
		document->forceDirty = false;
		{
			BackgroundDocument background(host, *document);
			background.view.send(SCI_SETSAVEPOINT);
		}
		document->modified = false;
		closeActive();
		host.foldersChanged();
		break;
	}
	case Cmd::Quit: host.quit(); break;

	case Cmd::Undo: s(SCI_UNDO); break;
	case Cmd::Redo: s(SCI_REDO); break;
	case Cmd::Cut: s(SCI_CUT); break;
	case Cmd::Copy: s(SCI_COPY); break;
	case Cmd::Paste: s(SCI_PASTE); break;
	case Cmd::SelectAll: s(SCI_SELECTALL); break;
	case Cmd::InsertDate: {
		char buffer[128];
		const time_t now = time(nullptr);
		struct tm local;
		localtime_r(&now, &local);
		strftime(buffer, sizeof(buffer), arg == 0 ? "%x %H:%M" : "%A, %B %e, %Y %H:%M:%S", &local);
		s(SCI_REPLACESEL, 0, reinterpret_cast<intptr_t>(buffer));
		break;
	}
	case Cmd::CopyPath:
		if (arg == 0) copyToClipboardIfAny(document->path.empty() ? document->title() : document->path);
		else if (arg == 1) copyToClipboardIfAny(document->title());
		else copyToClipboardIfAny(document->path.empty() ? std::string() : directoryOf(document->path));
		break;
	case Cmd::Case: {
		const long start = s(SCI_GETSELECTIONSTART);
		const long end = s(SCI_GETSELECTIONEND);
		if (start != end) replaceRange(start, end, convertCase(rangeOf(start, end), static_cast<CaseOp>(arg)), true);
		break;
	}
	case Cmd::LineOperation: lineOperation(static_cast<LineOp>(arg)); break;
	case Cmd::DuplicateLine: s(SCI_SELECTIONDUPLICATE); break;
	case Cmd::MoveLineUp: s(SCI_MOVESELECTEDLINESUP); break;
	case Cmd::MoveLineDown: s(SCI_MOVESELECTEDLINESDOWN); break;
	case Cmd::JoinLines:
	case Cmd::SplitLines: {
		long start, end;
		selectedLines(start, end, false);
		s(SCI_SETTARGETRANGE, start, end);
		s(cmd == Cmd::JoinLines ? SCI_LINESJOIN : SCI_LINESSPLIT);
		break;
	}
	case Cmd::InsertBlankAbove: {
		const long line = currentLine();
		s(SCI_GOTOPOS, s(SCI_POSITIONFROMLINE, line));
		s(SCI_NEWLINE);
		s(SCI_GOTOLINE, line);
		break;
	}
	case Cmd::InsertBlankBelow:
		s(SCI_GOTOPOS, s(SCI_GETLINEENDPOSITION, currentLine()));
		s(SCI_NEWLINE);
		break;
	case Cmd::Comment: comment(static_cast<CommentMode>(arg)); break;
	case Cmd::BlockComment: blockComment(); break;
	case Cmd::EolConvert:
		s(SCI_CONVERTEOLS, arg);
		s(SCI_SETEOLMODE, arg);
		document->eol = static_cast<Eol>(arg);
		refreshChrome();
		break;
	case Cmd::EolToSpace: {
		long start, end;
		selectedLines(start, end, true);
		replaceRange(start, end, joinLinesWithSpace(rangeOf(start, end)), false);
		break;
	}
	case Cmd::TabsToSpaces: {
		long start, end;
		selectedLines(start, end, true);
		replaceRange(start, end, tabsToSpaces(rangeOf(start, end), static_cast<int>(s(SCI_GETTABWIDTH))), false);
		break;
	}
	case Cmd::SpacesToTabs: {
		long start, end;
		selectedLines(start, end, true);
		replaceRange(start, end, spacesToTabs(rangeOf(start, end), static_cast<int>(s(SCI_GETTABWIDTH)), arg == 1), false);
		break;
	}

	case Cmd::ShowFind: host.showFind(FindTab::Find, selectionOrWord(), std::string()); break;
	case Cmd::ShowReplace: host.showFind(FindTab::Replace, selectionOrWord(), std::string()); break;
	case Cmd::ShowFindInFiles: {
		std::string directory;
		if (!document->path.empty()) directory = directoryOf(document->path);
		if (!folderRoots.empty() && directory.empty()) directory = folderRoots.front();
		host.showFind(FindTab::Files, selectionOrWord(), directory);
		break;
	}
	case Cmd::FindNext: runFromMenu(false, false); break;
	case Cmd::FindPrevious: runFromMenu(true, false); break;
	case Cmd::SelectFindNext: runFromMenu(false, true); break;
	case Cmd::SelectFindPrevious: runFromMenu(true, true); break;
	case Cmd::ToggleResults: showResults(!resultsVisible); break;
	case Cmd::GoToLine: goToLine(); break;
	case Cmd::MatchingBrace: {
		const long position = s(SCI_GETCURRENTPOS);
		long match = s(SCI_BRACEMATCH, position);
		if (match < 0 && position > 0) match = s(SCI_BRACEMATCH, position - 1);
		if (match >= 0) s(SCI_GOTOPOS, match);
		break;
	}

	case Cmd::ToggleBookmark: toggleBookmark(currentLine()); break;
	case Cmd::NextBookmark: goToBookmark(true); break;
	case Cmd::PreviousBookmark: goToBookmark(false); break;
	case Cmd::ClearBookmarks: s(SCI_MARKERDELETEALL, kMarkerBookmark); break;
	case Cmd::CopyBookmarked: {
		std::string out;
		const long mask = 1 << kMarkerBookmark;
		for (long line = s(SCI_MARKERNEXT, 0, mask); line >= 0; line = s(SCI_MARKERNEXT, line + 1, mask)) {
			out += rangeOf(s(SCI_POSITIONFROMLINE, line), s(SCI_GETLINEENDPOSITION, line));
			out += eolString(document->eol);
		}
		copyToClipboardIfAny(out);
		break;
	}
	case Cmd::RemoveBookmarked: removeLines(false); break;
	case Cmd::RemoveUnbookmarked: removeLines(true); break;
	case Cmd::InvertBookmarks: {
		const long lines = s(SCI_GETLINECOUNT);
		for (long line = 0; line < lines; line++) toggleBookmark(line);
		break;
	}

	case Cmd::AlwaysOnTop: alwaysOnTop = !alwaysOnTop; host.setAlwaysOnTop(alwaysOnTop); break;
	case Cmd::FullScreen: fullScreen = !fullScreen; host.setFullScreen(fullScreen); break;
	case Cmd::ToggleSpace: settings.showSpace = !settings.showSpace; applyPreferences(); break;
	case Cmd::ToggleEol: settings.showEol = !settings.showEol; applyPreferences(); break;
	case Cmd::ToggleAllCharacters: {
		const bool on = !(settings.showSpace && settings.showEol);
		settings.showSpace = settings.showEol = on;
		applyPreferences();
		break;
	}
	case Cmd::ToggleGuides: settings.showGuides = !settings.showGuides; applyPreferences(); break;
	case Cmd::ToggleWrapSymbol: settings.showWrapSymbol = !settings.showWrapSymbol; applyPreferences(); break;
	case Cmd::ToggleWrap: settings.wordWrap = !settings.wordWrap; applyPreferences(); break;
	case Cmd::ZoomIn: s(SCI_ZOOMIN); resetZoomMargins(); break;
	case Cmd::ZoomOut: s(SCI_ZOOMOUT); resetZoomMargins(); break;
	case Cmd::ZoomReset: s(SCI_SETZOOM, 0); resetZoomMargins(); break;
	case Cmd::ActivateTab: activate(focus, arg); host.focusPane(focus); break;
	case Cmd::NextTab: {
		const Pane &p = paneAt(focus);
		activate(focus, (p.active + 1) % static_cast<int>(p.tabs.size()));
		host.focusPane(focus);
		break;
	}
	case Cmd::PreviousTab: {
		const Pane &p = paneAt(focus);
		const int count = static_cast<int>(p.tabs.size());
		activate(focus, (p.active + count - 1) % count);
		host.focusPane(focus);
		break;
	}
	case Cmd::MoveTabForward: moveTab(focus, paneAt(focus).active, paneAt(focus).active + 1); break;
	case Cmd::MoveTabBackward: moveTab(focus, paneAt(focus).active, paneAt(focus).active - 1); break;
	case Cmd::MoveToOtherView: moveTabToOtherView(focus, paneAt(focus).active); break;
	case Cmd::CloneToOtherView: cloneTabToOtherView(focus, paneAt(focus).active); break;
	case Cmd::FocusOtherView:
		if (splitVisible()) {
			focus = 1 - focus;
			refreshChrome();
			host.focusPane(focus);
		}
		break;
	case Cmd::RotateSplit:
		stacked = !stacked;
		host.splitChanged(splitVisible(), stacked);
		break;
	case Cmd::SyncVertical:
		syncVertical = !syncVertical;
		syncLine = view(1).send(SCI_GETFIRSTVISIBLELINE) - view(0).send(SCI_GETFIRSTVISIBLELINE);
		break;
	case Cmd::SyncHorizontal:
		syncHorizontal = !syncHorizontal;
		syncColumn = view(1).send(SCI_GETXOFFSET) - view(0).send(SCI_GETXOFFSET);
		break;

	case Cmd::ToggleFolderPanel:
		if (folderRoots.empty() && !folderPanel) {
			run(Cmd::OpenFolder);
		} else {
			setFolderPanelVisible(!folderPanel);
		}
		break;
	case Cmd::ToggleHiddenInFolders:
		settings.showHiddenInFolders = !settings.showHiddenInFolders;
		saveSettings();
		host.foldersChanged();
		break;
	case Cmd::RefreshFolders: host.foldersChanged(); break;
	case Cmd::CloseAllFolders:
		folderRoots.clear();
		expandedFolders.clear();
		setFolderPanelVisible(false);
		host.foldersChanged();
		break;
	case Cmd::FolderNewFile: case Cmd::FolderNewFolder: case Cmd::FolderRename: case Cmd::FolderTrash:
	case Cmd::FolderCopyPath: case Cmd::FolderReveal: case Cmd::FolderFindInFiles: case Cmd::FolderRemoveRoot:
		runFolderCommand(cmd, argument);
		break;

	case Cmd::FoldAll:
		s(SCI_COLOURISE, 0, -1);
		s(SCI_FOLDALL, SC_FOLDACTION_CONTRACT);
		break;
	case Cmd::UnfoldAll: s(SCI_FOLDALL, SC_FOLDACTION_EXPAND); break;
	case Cmd::FoldCurrent:
	case Cmd::UnfoldCurrent: {
		const long line = currentLine();
		const long header = (s(SCI_GETFOLDLEVEL, line) & SC_FOLDLEVELHEADERFLAG) ? line : s(SCI_GETFOLDPARENT, line);
		if (header >= 0) s(SCI_FOLDLINE, header, cmd == Cmd::FoldCurrent ? SC_FOLDACTION_CONTRACT : SC_FOLDACTION_EXPAND);
		break;
	}
	case Cmd::FoldLevel: foldLevel(arg, false); break;
	case Cmd::UnfoldLevel: foldLevel(arg, true); break;
	case Cmd::Summary: summary(); break;

	case Cmd::ReopenEncoding: reinterpretActiveAsEncoding(argument); break;
	case Cmd::ConvertEncoding: convertActiveToEncoding(argument); break;
	case Cmd::SetLanguage: setActiveLanguage(argument); break;
	case Cmd::SetTheme:
		settings.theme = argument;
		saveSettings();
		applyThemeEverywhere();
		break;
	case Cmd::ChooseFont: {
		std::string family = settings.fontName;
		double size = settings.fontSize;
		if (host.chooseFont(family, size)) {
			settings.fontName = family;
			settings.fontSize = size;
			saveSettings();
			applyThemeEverywhere();
		}
		break;
	}
	case Cmd::SetTabWidth:
		settings.tabWidth = arg;
		saveSettings();
		applyLanguageTo(0);
		applyLanguageTo(1);
		break;
	case Cmd::ToggleInsertSpaces:
		settings.tabsToSpaces = !settings.tabsToSpaces;
		saveSettings();
		applyLanguageTo(0);
		applyLanguageTo(1);
		break;
	case Cmd::HashSelection: hashSelection(static_cast<HashKind>(arg)); break;
	case Cmd::HashFiles: hashFiles(static_cast<HashKind>(arg)); break;
	case Cmd::MacroStart:
		macro.clear();
		recording = true;
		s(SCI_STARTRECORD);
		host.flash("Recording macro");
		break;
	case Cmd::MacroStop:
		s(SCI_STOPRECORD);
		recording = false;
		host.flash("Recorded " + std::to_string(macro.size()) + " steps");
		break;
	case Cmd::MacroPlay:
		s(SCI_BEGINUNDOACTION);
		macroPlayOnce();
		s(SCI_ENDUNDOACTION);
		break;
	case Cmd::MacroPlayToEnd: {
		if (macro.empty()) break;
		s(SCI_BEGINUNDOACTION);
		long previous = -1;
		// Stops once a run no longer moves forward, so a macro can't loop forever.
		while (currentLine() != previous && currentLine() < s(SCI_GETLINECOUNT) - 1) {
			previous = currentLine();
			macroPlayOnce();
		}
		s(SCI_ENDUNDOACTION);
		break;
	}
	case Cmd::About:
		host.ask("omapad",
		         "A native text editor modelled on Notepad++, built on Scintilla and Lexilla. Language keywords, style data and themes come from Notepad++ 8.9.8.1 under the GNU GPL v3.\n\n"
		         "omapad is an independent project. It is not Notepad++, and isn't affiliated with or endorsed by its authors or by Omarchy.", {"OK"});
		break;
	case Cmd::SelectDocument: clickTab(tabPane, tabIndex); break;

	case Cmd::TabClose: closeTab(tabPane, tabIndex); break;
	case Cmd::TabCloseOthers:
	case Cmd::TabCloseLeft:
	case Cmd::TabCloseRight: {
		std::vector<std::pair<int, std::shared_ptr<Document>>> targets;
		const auto &list = paneAt(tabPane).tabs;
		for (int i = 0; i < static_cast<int>(list.size()); i++) {
			const bool wanted = (cmd == Cmd::TabCloseOthers && i != tabIndex) || (cmd == Cmd::TabCloseLeft && i < tabIndex) || (cmd == Cmd::TabCloseRight && i > tabIndex);
			if (wanted) targets.emplace_back(tabPane, list[i].document);
		}
		closeTabs(targets);
		break;
	}
	case Cmd::TabReveal: {
		const auto &list = paneAt(tabPane).tabs;
		if (tabIndex < static_cast<int>(list.size()) && !list[tabIndex].document->path.empty()) host.reveal(list[tabIndex].document->path);
		break;
	}
	case Cmd::TabCopyPath: {
		const auto &list = paneAt(tabPane).tabs;
		if (tabIndex < static_cast<int>(list.size())) copyToClipboardIfAny(list[tabIndex].document->path);
		break;
	}
	case Cmd::TabMoveToOther: moveTabToOtherView(tabPane, tabIndex); break;
	case Cmd::TabCloneToOther: cloneTabToOtherView(tabPane, tabIndex); break;
	}
}

MenuState Workspace::stateOf(const MenuItem &item) {
	MenuState state;
	const Document *document = activeDocument();
	if (!document) return state;
	const bool saved = !document->path.empty();
	switch (item.cmd) {
	case Cmd::ToggleWrap: state.checked = settings.wordWrap; break;
	case Cmd::ToggleSpace: state.checked = settings.showSpace; break;
	case Cmd::ToggleEol: state.checked = settings.showEol; break;
	case Cmd::ToggleGuides: state.checked = settings.showGuides; break;
	case Cmd::ToggleWrapSymbol: state.checked = settings.showWrapSymbol; break;
	case Cmd::ToggleAllCharacters: state.checked = settings.showSpace && settings.showEol; break;
	case Cmd::AlwaysOnTop: state.checked = alwaysOnTop; break;
	case Cmd::FullScreen: state.checked = fullScreen; break;
	case Cmd::ToggleResults: state.checked = resultsVisible; break;
	case Cmd::ToggleInsertSpaces: state.checked = settings.tabsToSpaces; break;
	case Cmd::SetTabWidth: state.checked = settings.tabWidth == item.arg; break;
	case Cmd::SetLanguage: state.checked = document->language->name == item.text; break;
	case Cmd::EolConvert: state.checked = static_cast<int>(document->eol) == item.arg; break;
	case Cmd::SetTheme: state.checked = settings.theme == item.text; break;
	case Cmd::ReopenEncoding:
		state.checked = item.text == document->encoding->identifier;
		state.enabled = saved;
		break;
	case Cmd::ConvertEncoding: state.checked = item.text == document->encoding->identifier; break;
	case Cmd::MacroStart: state.enabled = !recording; break;
	case Cmd::MacroStop: state.enabled = recording; break;
	case Cmd::MacroPlay:
	case Cmd::MacroPlayToEnd: state.enabled = !recording && !macro.empty(); break;
	case Cmd::Reload:
	case Cmd::Reveal:
	case Cmd::Trash: state.enabled = saved; break;
	case Cmd::FocusOtherView:
	case Cmd::SyncVertical:
	case Cmd::SyncHorizontal:
		state.enabled = splitVisible();
		if (item.cmd == Cmd::SyncVertical) state.checked = syncVertical;
		if (item.cmd == Cmd::SyncHorizontal) state.checked = syncHorizontal;
		break;
	case Cmd::RotateSplit: state.checked = stacked; state.enabled = splitVisible(); break;
	case Cmd::ToggleFolderPanel: state.checked = folderPanel; break;
	case Cmd::ToggleHiddenInFolders: state.checked = settings.showHiddenInFolders; break;
	case Cmd::CloseAllFolders:
	case Cmd::RefreshFolders: state.enabled = !folderRoots.empty(); break;
	case Cmd::SelectDocument:
		state.checked = item.arg / kPaneArgFactor == focus && item.arg % kPaneArgFactor == paneAt(focus).active;
		break;
	default:
		break;
	}
	return state;
}

std::vector<MenuItem> Workspace::dynamicMenu(MenuItem::Dynamic kind) const {
	std::vector<MenuItem> out;
	if (kind == MenuItem::Dynamic::Recent) {
		for (const std::string &path : settings.recent) {
			MenuItem item;
			item.title = baseName(path);
			item.cmd = Cmd::OpenRecent;
			item.text = path;
			out.push_back(item);
		}
		MenuItem separator;
		separator.separator = true;
		if (!out.empty()) out.push_back(separator);
		MenuItem clear;
		clear.title = out.empty() ? "No Recent Files" : "Clear Menu";
		clear.cmd = out.empty() ? Cmd::None : Cmd::ClearRecent;
		out.push_back(clear);
	} else if (kind == MenuItem::Dynamic::Documents) {
		for (int pn = 0; pn < 2; pn++) {
			const auto &list = paneAt(pn).tabs;
			for (int i = 0; i < static_cast<int>(list.size()); i++) {
				MenuItem item;
				item.title = list[i].document->title() + (splitVisible() ? (pn == 0 ? "  (first view)" : "  (second view)") : "");
				item.cmd = Cmd::SelectDocument;
				item.arg = encodeTab(pn, i);
				out.push_back(item);
			}
		}
	}
	return out;
}

std::vector<MenuItem> Workspace::tabMenu(int pane, int index) const {
	std::vector<MenuItem> out;
	const auto add = [&](const char *title, Cmd cmd) {
		MenuItem item;
		item.title = title;
		item.cmd = cmd;
		item.arg = encodeTab(pane, index);
		out.push_back(item);
	};
	const auto separator = [&]() {
		MenuItem item;
		item.separator = true;
		out.push_back(item);
	};
	add("Close", Cmd::TabClose);
	add("Close Others", Cmd::TabCloseOthers);
	add("Close to the Left", Cmd::TabCloseLeft);
	add("Close to the Right", Cmd::TabCloseRight);
	separator();
	add(pane == 0 ? "Move to Second View" : "Move to First View", Cmd::TabMoveToOther);
	add(pane == 0 ? "Clone to Second View" : "Clone to First View", Cmd::TabCloneToOther);
	const auto &list = paneAt(pane).tabs;
	if (index >= 0 && index < static_cast<int>(list.size()) && !list[index].document->path.empty()) {
		separator();
#ifdef __APPLE__
		add("Reveal in Finder", Cmd::TabReveal);
#else
		add("Show in File Manager", Cmd::TabReveal);
#endif
		add("Copy Full Path", Cmd::TabCopyPath);
	}
	return out;
}

std::vector<MenuItem> Workspace::folderMenu(const std::string &path, bool isDirectory, bool isRoot) const {
	std::vector<MenuItem> out;
	const auto add = [&](const char *title, Cmd cmd, const std::string &target) {
		MenuItem item;
		item.title = title;
		item.cmd = cmd;
		item.text = target;
		out.push_back(item);
	};
	const auto separator = [&]() {
		MenuItem item;
		item.separator = true;
		out.push_back(item);
	};
	const std::string directory = isDirectory ? path : directoryOf(path);
	add("New File…", Cmd::FolderNewFile, directory);
	add("New Folder…", Cmd::FolderNewFolder, directory);
	add("Find in Files…", Cmd::FolderFindInFiles, directory);
	separator();
	if (!isRoot) {
		add("Rename…", Cmd::FolderRename, path);
#ifdef __APPLE__
		add("Move to Trash", Cmd::FolderTrash, path);
#else
		add("Move to Trash", Cmd::FolderTrash, path);
#endif
		separator();
	}
	add("Copy Path", Cmd::FolderCopyPath, path);
#ifdef __APPLE__
	add("Reveal in Finder", Cmd::FolderReveal, path);
#else
	add("Show in File Manager", Cmd::FolderReveal, path);
#endif
	if (isRoot) {
		separator();
		add("Remove from Workspace", Cmd::FolderRemoveRoot, path);
	}
	return out;
}

std::vector<MenuItem> Workspace::mainMenu() const {
#ifdef __APPLE__
	const auto key = [](const char *mac, const char *) { return std::string(mac); };
#else
	const auto key = [](const char *, const char *linux_) { return std::string(linux_); };
#endif
	const auto item = [](const std::string &title, Cmd cmd, const std::string &shortcut = std::string(), int arg = 0) {
		MenuItem m;
		m.title = title;
		m.cmd = cmd;
		m.shortcut = shortcut;
		m.arg = arg;
		return m;
	};
	const auto textItem = [](const std::string &title, Cmd cmd, const std::string &text) {
		MenuItem m;
		m.title = title;
		m.cmd = cmd;
		m.text = text;
		return m;
	};
	const auto edit = [](const std::string &title, Cmd cmd, const std::string &shortcut, const char *action) {
		MenuItem m;
		m.title = title;
		m.cmd = cmd;
		m.shortcut = shortcut;
		m.macAction = action;
		return m;
	};
	const auto separator = []() {
		MenuItem m;
		m.separator = true;
		return m;
	};
	const auto sub = [](const std::string &title, std::vector<MenuItem> children) {
		MenuItem m;
		m.title = title;
		m.children = std::move(children);
		return m;
	};
	const auto dynamic = [](const std::string &title, MenuItem::Dynamic kind) {
		MenuItem m;
		m.title = title;
		m.dynamic = kind;
		return m;
	};
	const auto op = [&](const std::string &title, LineOp value) { return item(title, Cmd::LineOperation, "", static_cast<int>(value)); };
	const auto kase = [&](const std::string &title, CaseOp value, const std::string &shortcut = std::string()) { return item(title, Cmd::Case, shortcut, static_cast<int>(value)); };

	std::vector<MenuItem> file = {
		item("New", Cmd::New, "Mod+N"),
		item("Open…", Cmd::Open, "Mod+O"),
		dynamic("Open Recent", MenuItem::Dynamic::Recent),
		item("Open Folder as Workspace…", Cmd::OpenFolder, "Mod+Alt+O"),
#ifdef __APPLE__
		item("Reveal in Finder", Cmd::Reveal),
#else
		item("Show in File Manager", Cmd::Reveal),
#endif
		item("Reload from Disk", Cmd::Reload, "Mod+R"),
		separator(),
		item("Save", Cmd::Save, "Mod+S"),
		item("Save As…", Cmd::SaveAs, key("Mod+Shift+S", "Mod+Alt+S")),
		item("Save a Copy As…", Cmd::SaveCopy),
		item("Save All", Cmd::SaveAll, key("Mod+Alt+S", "Mod+Shift+S")),
		separator(),
		item("Close", Cmd::Close, "Mod+W"),
		item("Close All", Cmd::CloseAll, "Mod+Shift+W"),
		sub("Close Multiple Documents", {
			item("Close All but Active Document", Cmd::CloseOthers),
			item("Close All Unchanged", Cmd::CloseUnchanged),
		}),
		item("Move to Trash", Cmd::Trash),
	};
#ifndef __APPLE__
	file.push_back(separator());
	file.push_back(item("Exit", Cmd::Quit));
#endif

	std::vector<MenuItem> sorts = {
		item("Duplicate Current Line", Cmd::DuplicateLine, "Mod+D"),
		op("Remove Duplicate Lines", LineOp::RemoveDuplicates),
		op("Remove Consecutive Duplicate Lines", LineOp::RemoveConsecutiveDuplicates),
		item("Split Lines", Cmd::SplitLines),
		item("Join Lines", Cmd::JoinLines, "Mod+J"),
		item("Move Up Current Line", Cmd::MoveLineUp, key("Mod+Ctrl+Up", "Mod+Shift+Up")),
		item("Move Down Current Line", Cmd::MoveLineDown, key("Mod+Ctrl+Down", "Mod+Shift+Down")),
		op("Remove Empty Lines", LineOp::RemoveEmpty),
		op("Remove Empty Lines (Containing Blank Characters)", LineOp::RemoveBlank),
		item("Insert Blank Line Above Current", Cmd::InsertBlankAbove),
		item("Insert Blank Line Below Current", Cmd::InsertBlankBelow),
		op("Reverse Line Order", LineOp::Reverse),
		op("Randomize Line Order", LineOp::Randomize),
		separator(),
		op("Sort Lines Lexicographically Ascending", LineOp::SortLexAsc),
		op("Sort Lines Lex. Ascending Ignoring Case", LineOp::SortFoldAsc),
		op("Sort Lines As Integers Ascending", LineOp::SortIntAsc),
		op("Sort Lines As Decimals Ascending", LineOp::SortDecimalAsc),
		op("Sort Lines By Length Ascending", LineOp::SortLengthAsc),
		separator(),
		op("Sort Lines Lexicographically Descending", LineOp::SortLexDesc),
		op("Sort Lines Lex. Descending Ignoring Case", LineOp::SortFoldDesc),
		op("Sort Lines As Integers Descending", LineOp::SortIntDesc),
		op("Sort Lines As Decimals Descending", LineOp::SortDecimalDesc),
		op("Sort Lines By Length Descending", LineOp::SortLengthDesc),
	};

	std::vector<MenuItem> editMenu = {
		edit("Undo", Cmd::Undo, "Mod+Z", "undo:"),
		edit("Redo", Cmd::Redo, key("Mod+Shift+Z", "Mod+Y"), "redo:"),
		separator(),
		edit("Cut", Cmd::Cut, "Mod+X", "cut:"),
		edit("Copy", Cmd::Copy, "Mod+C", "copy:"),
		edit("Paste", Cmd::Paste, "Mod+V", "paste:"),
		edit("Select All", Cmd::SelectAll, "Mod+A", "selectAll:"),
		separator(),
		sub("Insert", {
			item("Date and Time (short)", Cmd::InsertDate, "", 0),
			item("Date and Time (long)", Cmd::InsertDate, "", 1),
		}),
		sub("Copy to Clipboard", {
			item("Current Full File Path", Cmd::CopyPath, "", 0),
			item("Current Filename", Cmd::CopyPath, "", 1),
			item("Current Folder Path", Cmd::CopyPath, "", 2),
		}),
		sub("Convert Case to", {
			kase("UPPERCASE", CaseOp::Upper, "Mod+Shift+U"),
			kase("lowercase", CaseOp::Lower, "Mod+U"),
			kase("Proper Case", CaseOp::Proper),
			kase("Proper Case (blend)", CaseOp::ProperBlend),
			kase("Sentence case", CaseOp::Sentence),
			kase("Sentence case (blend)", CaseOp::SentenceBlend),
			kase("iNVERT cASE", CaseOp::Invert),
			kase("ranDOm CasE", CaseOp::Random),
		}),
		sub("Line Operations", sorts),
		sub("Comment/Uncomment", {
			item("Toggle Single Line Comment", Cmd::Comment, key("Mod+/", "Mod+Q"), static_cast<int>(CommentMode::Toggle)),
			item("Single Line Comment", Cmd::Comment, "", static_cast<int>(CommentMode::Comment)),
			item("Single Line Uncomment", Cmd::Comment, "", static_cast<int>(CommentMode::Uncomment)),
			item("Block Comment", Cmd::BlockComment, key("Mod+Alt+/", "Mod+Shift+Q")),
		}),
		sub("EOL Conversion", {
			item("Windows (CR LF)", Cmd::EolConvert, "", static_cast<int>(Eol::CRLF)),
			item("Unix (LF)", Cmd::EolConvert, "", static_cast<int>(Eol::LF)),
			item("Macintosh (CR)", Cmd::EolConvert, "", static_cast<int>(Eol::CR)),
		}),
		sub("Blank Operations", {
			op("Trim Trailing Space", LineOp::TrimTrailing),
			op("Trim Leading Space", LineOp::TrimLeading),
			op("Trim Leading and Trailing Space", LineOp::TrimBoth),
			item("EOL to Space", Cmd::EolToSpace),
			separator(),
			item("TAB to Space", Cmd::TabsToSpaces),
			item("Space to TAB (All)", Cmd::SpacesToTabs, "", 0),
			item("Space to TAB (Leading)", Cmd::SpacesToTabs, "", 1),
		}),
	};

	std::vector<MenuItem> search = {
		item("Find…", Cmd::ShowFind, "Mod+F"),
		item("Find in Files…", Cmd::ShowFindInFiles, "Mod+Shift+F"),
		item("Find Next", Cmd::FindNext, key("Mod+G", "F3")),
		item("Find Previous", Cmd::FindPrevious, key("Mod+Shift+G", "Shift+F3")),
		item("Select and Find Next", Cmd::SelectFindNext, key("Mod+E", "Mod+F3")),
		item("Select and Find Previous", Cmd::SelectFindPrevious, key("Mod+Shift+E", "Mod+Shift+F3")),
		item("Replace…", Cmd::ShowReplace, key("Mod+Alt+F", "Mod+H")),
		item("Search Results Window", Cmd::ToggleResults, "F7"),
		separator(),
		item("Go to Line…", Cmd::GoToLine, key("Mod+L", "Mod+G")),
		item("Go to Matching Brace", Cmd::MatchingBrace, "Mod+B"),
		separator(),
		sub("Bookmark", {
			item("Toggle Bookmark", Cmd::ToggleBookmark, "Mod+F2"),
			item("Next Bookmark", Cmd::NextBookmark, "F2"),
			item("Previous Bookmark", Cmd::PreviousBookmark, "Shift+F2"),
			item("Clear All Bookmarks", Cmd::ClearBookmarks),
			item("Copy Bookmarked Lines", Cmd::CopyBookmarked),
			item("Remove Bookmarked Lines", Cmd::RemoveBookmarked),
			item("Remove Non-Bookmarked Lines", Cmd::RemoveUnbookmarked),
			item("Inverse Bookmarks", Cmd::InvertBookmarks),
		}),
	};

	std::vector<MenuItem> tabsMenu;
	for (int i = 0; i < 9; i++)
		tabsMenu.push_back(item("Tab " + std::to_string(i + 1), Cmd::ActivateTab, "Mod+" + std::to_string(i + 1), i));
	tabsMenu.push_back(item("Next Tab", Cmd::NextTab, key("Mod+Shift+]", "Mod+PageDown")));
	tabsMenu.push_back(item("Previous Tab", Cmd::PreviousTab, key("Mod+Shift+[", "Mod+PageUp")));
	tabsMenu.push_back(item("Move Tab Forward", Cmd::MoveTabForward));
	tabsMenu.push_back(item("Move Tab Backward", Cmd::MoveTabBackward));

	std::vector<MenuItem> foldLevels, unfoldLevels;
	for (int i = 1; i <= 8; i++) {
		foldLevels.push_back(item(std::to_string(i), Cmd::FoldLevel, "", i));
		unfoldLevels.push_back(item(std::to_string(i), Cmd::UnfoldLevel, "", i));
	}

	std::vector<MenuItem> viewMenu = {
		item("Always on Top", Cmd::AlwaysOnTop),
		item("Toggle Full Screen", Cmd::FullScreen, key("Mod+Ctrl+F", "F11")),
		separator(),
		item("Folder as Workspace", Cmd::ToggleFolderPanel, "Mod+Alt+B"),
		sub("Split View", {
			item("Move to Other View", Cmd::MoveToOtherView, "Mod+Alt+Right"),
			item("Clone to Other View", Cmd::CloneToOtherView, "Mod+Alt+Shift+Right"),
			item("Focus on Other View", Cmd::FocusOtherView, "F8"),
			separator(),
			item("Stack Views", Cmd::RotateSplit),
			item("Synchronize Vertical Scrolling", Cmd::SyncVertical),
			item("Synchronize Horizontal Scrolling", Cmd::SyncHorizontal),
		}),
		sub("Show Symbol", {
			item("Show Space and Tab", Cmd::ToggleSpace),
			item("Show End of Line", Cmd::ToggleEol),
			item("Show All Characters", Cmd::ToggleAllCharacters),
			item("Show Indent Guide", Cmd::ToggleGuides),
			item("Show Wrap Symbol", Cmd::ToggleWrapSymbol),
		}),
		sub("Zoom", {
			item("Zoom In", Cmd::ZoomIn, "Mod+="),
			item("Zoom Out", Cmd::ZoomOut, "Mod+-"),
			item("Restore Default Zoom", Cmd::ZoomReset, "Mod+0"),
		}),
		sub("Tab", tabsMenu),
		item("Word Wrap", Cmd::ToggleWrap, "Alt+Z"),
		separator(),
		item("Fold All", Cmd::FoldAll, key("Mod+Alt+0", "Alt+0")),
		item("Unfold All", Cmd::UnfoldAll, key("Mod+Alt+Shift+0", "Alt+Shift+0")),
		item("Fold Current Level", Cmd::FoldCurrent),
		item("Unfold Current Level", Cmd::UnfoldCurrent),
		sub("Fold Level", foldLevels),
		sub("Unfold Level", unfoldLevels),
		separator(),
		item("Summary…", Cmd::Summary),
	};

	std::vector<MenuItem> reopen, convert;
	for (const Encoding &e : allEncodings()) {
		reopen.push_back(textItem(e.name, Cmd::ReopenEncoding, e.identifier));
		convert.push_back(textItem(e.name, Cmd::ConvertEncoding, e.identifier));
	}

	std::vector<MenuItem> languageMenu;
	languageMenu.push_back(textItem(catalog.plainText() ? catalog.plainText()->displayName : "Normal text", Cmd::SetLanguage, "normal"));
	languageMenu.push_back(separator());
	std::string letter;
	for (const Language *language : catalog.languages()) {
		if (language == catalog.plainText() || language->displayName.empty()) continue;
		const std::string first(1, static_cast<char>(std::toupper(static_cast<unsigned char>(language->displayName[0]))));
		if (first != letter) {
			letter = first;
			languageMenu.push_back(sub(letter, {}));
		}
		languageMenu.back().children.push_back(textItem(language->displayName, Cmd::SetLanguage, language->name));
	}

	std::vector<MenuItem> themeMenu;
	themeMenu.push_back(textItem(catalog.hasOmarchyTheme() ? "Automatic (Follow Omarchy)" : "Automatic (Follow System)", Cmd::SetTheme, "Auto"));
	themeMenu.push_back(textItem("Default", Cmd::SetTheme, "Default"));
	themeMenu.push_back(separator());
	for (const std::string &name : catalog.themeNames()) themeMenu.push_back(textItem(name, Cmd::SetTheme, name));

	std::vector<MenuItem> widths;
	for (int width : {2, 3, 4, 8}) widths.push_back(item(std::to_string(width), Cmd::SetTabWidth, "", width));

	std::vector<MenuItem> toolsMenu;
	const std::pair<const char *, HashKind> hashes[] = {{"MD5", HashKind::MD5}, {"SHA-1", HashKind::SHA1}, {"SHA-256", HashKind::SHA256}, {"SHA-512", HashKind::SHA512}};
	for (const auto &hash : hashes) {
		toolsMenu.push_back(sub(hash.first, {
			item("Generate from Files…", Cmd::HashFiles, "", static_cast<int>(hash.second)),
			item("Generate from Selection into Clipboard", Cmd::HashSelection, "", static_cast<int>(hash.second)),
		}));
	}

	return {
		sub("File", file),
		sub("Edit", editMenu),
		sub("Search", search),
		sub("View", viewMenu),
		sub("Encoding", {sub("Reopen with Encoding", reopen), sub("Convert To", convert)}),
		sub("Language", languageMenu),
		sub("Settings", {
			sub("Theme", themeMenu),
			item("Font…", Cmd::ChooseFont, key("Mod+T", "")),
			sub("Tab Size", widths),
			item("Insert Spaces for Tabs", Cmd::ToggleInsertSpaces),
		}),
		sub("Tools", toolsMenu),
		sub("Macro", {
			item("Start Recording", Cmd::MacroStart),
			item("Stop Recording", Cmd::MacroStop),
			item("Playback", Cmd::MacroPlay, "Mod+Shift+P"),
			item("Run to End of File", Cmd::MacroPlayToEnd),
		}),
		sub("Folders", {
			item("Open Folder as Workspace…", Cmd::OpenFolder),
			item("Show Hidden Files", Cmd::ToggleHiddenInFolders),
			item("Refresh", Cmd::RefreshFolders),
			item("Close All Folders", Cmd::CloseAllFolders),
		}),
		dynamic("Window", MenuItem::Dynamic::Documents),
#ifndef __APPLE__
		sub("Help", {item("About omapad", Cmd::About)}),
#endif
	};
}

}  // namespace omapad
