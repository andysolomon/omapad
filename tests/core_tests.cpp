#include <cstring>
// Tests pure logic directly, then drives the real Workspace with real Scintilla views,
// against real files in a temporary folder. Runs the same on macOS and Linux.
#include "TestViews.h"
#include "SciLexer.h"
#include "Scintilla.h"
#include "TextCore.h"
#include "Workspace.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace omapad;
namespace fs = std::filesystem;

static int gFailures = 0;
static int gChecks = 0;

static void check(bool ok, const char *what, const std::string &detail = "") {
	gChecks++;
	if (!ok) {
		gFailures++;
		printf("FAIL: %s %s\n", what, detail.c_str());
	}
}

static void checkEq(const std::string &actual, const std::string &expected, const char *what) {
	check(actual == expected, what, "\n  got:      [" + actual + "]\n  expected: [" + expected + "]");
}

static std::string readAll(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	std::stringstream s;
	s << in.rdbuf();
	return s.str();
}

static std::string writeFile(const std::string &dir, const std::string &name, const std::string &data) {
	const std::string path = dir + "/" + name;
	std::ofstream out(path, std::ios::binary);
	out << data;
	return path;
}

static void sleepForMtime() { usleep(1100 * 1000); }

// A host with real editor views and no windows. Questions get canned answers.
struct TestHost : Host {
	Workspace *workspace = nullptr;
	std::unique_ptr<SciView> panes[2], results, background, map, printer;
	std::deque<std::function<void()>> queue;
	std::mutex queueLock;
	int answer = 0;
	int prompts = 0;
	std::string lastTitle;
	std::string promptAnswer;
	bool promptAccepted = false;
	std::string clipboard;
	bool splitVisible = false, splitStacked = false, resultsVisible = false, folderVisible = false;
	int foldersChangedCount = 0;
	std::string left, right, flashed, trashError;
	std::vector<std::string> trashed;
	FindOptions find;
	int menusChangedCount = 0, functionListChangedCount = 0, mapperRuns = 0;
	bool mapVisible = false, functionVisible = false;
	std::function<bool(std::vector<FormField> &)> formAnswer;
	std::string lastFormTitle;

	TestHost() {
		for (int i = 0; i < 2; i++)
			panes[i] = makeHiddenSciView([this, i](const SCNotification &n) { if (workspace) workspace->editorNotification(i, n); });
		results = makeHiddenSciView([this](const SCNotification &n) { if (workspace) workspace->resultsNotification(n); });
		background = makeHiddenSciView(nullptr);
		map = makeHiddenSciView(nullptr);
		printer = makeHiddenSciView(nullptr);
	}
	SciView &paneView(int pane) override { return *panes[pane]; }
	SciView &resultsView() override { return *results; }
	SciView &backgroundView() override { return *background; }
	SciView &printView() override { return *printer; }
	SciView &mapView() override { return *map; }
	int ask(const std::string &title, const std::string &, const std::vector<std::string> &) override {
		prompts++;
		lastTitle = title;
		return answer;
	}
	std::vector<std::string> filesToOpen;
	std::string savePath, savePathSuggested;
	bool chooseFilesToOpen(const std::string &, std::vector<std::string> &out) override {
		out = filesToOpen;
		return !out.empty();
	}
	bool chooseSavePath(const std::string &name, const std::string &, std::string &out) override {
		savePathSuggested = name;
		out = savePath;
		return !out.empty();
	}
	bool chooseFolder(const std::string &, std::string &) override { return false; }
	bool chooseFont(std::string &, double &) override { return false; }
	bool promptText(const std::string &, const std::string &, const std::string &, std::string &out) override {
		out = promptAnswer;
		return promptAccepted;
	}
	void setClipboard(const std::string &text) override { clipboard = text; }
	void reveal(const std::string &) override {}
	bool moveToTrash(const std::string &path, std::string &error) override {
		std::error_code fsError;
		fs::remove_all(path, fsError);
		trashed.push_back(path);
		error = fsError.message();
		return !fsError;
	}
	void tabsChanged() override {}
	void statusChanged(const std::string &l, const std::string &r) override { left = l; right = r; }
	void flash(const std::string &message) override { flashed = message; }
	void splitChanged(bool visible, bool stacked) override { splitVisible = visible; splitStacked = stacked; }
	void setResultsVisible(bool visible) override { resultsVisible = visible; }
	void setFolderPanelVisible(bool visible) override { folderVisible = visible; }
	void foldersChanged() override { foldersChangedCount++; }
	void focusPane(int) override {}
	void themeChanged() override {}
	void menusChanged() override { menusChangedCount++; }
	bool runForm(const std::string &title, std::vector<FormField> &fields) override {
		lastFormTitle = title;
		return formAnswer ? formAnswer(fields) : false;
	}
	void runShortcutMapper() override { mapperRuns++; }
	bool print(PrintJob &job, const PrintOutput &output, std::string &error) override {
		printRuns++;
		if (duringPrint) duringPrint();
		lastPrintOutput = output;
		lastPrintStart = job.settings().start;
		lastPrintEnd = job.settings().end;
		lastPrintColourMode = job.settings().colourMode;
		lastPrintPageCount = 0;
		if (void *surface = makeTestSurface()) {
			job.paginate(surface, 500, 700);
			lastPrintPageCount = job.pageCount();
			job.drawPage(surface, 0, 500, 700);
			lastPrintFooter = job.footer(0).middle;
			lastPrintHeader = job.header(0).left;
			freeTestSurface(surface);
		}
		if (!printError.empty()) {
			error = printError;
			return false;
		}
		if (!output.pdfPath.empty()) pdfWritten = writeTestPdf(job, output, error);
		return true;
	}
	void setDocumentMapVisible(bool visible) override { mapVisible = visible; }
	void setFunctionListVisible(bool visible) override { functionVisible = visible; }
	void functionListChanged() override { functionListChangedCount++; }
	void showFind(FindTab, const std::string &, const std::string &) override {}
	void setFindText(const std::string &text) override { find.what = text; }
	FindOptions currentFindOptions() override { return find; }
	void setFindStatus(const std::string &, bool) override {}
	void setFindInFilesRunning(bool) override {}
	void setFullScreen(bool) override {}
	void setAlwaysOnTop(bool) override {}
	bool systemIsDark() override { return false; }
	void post(std::function<void()> work) override {
		std::lock_guard<std::mutex> lock(queueLock);
		queue.push_back(std::move(work));
	}
	int printRuns = 0, lastPrintPageCount = 0, lastPrintColourMode = -1;
	intptr_t lastPrintStart = 0, lastPrintEnd = 0;
	PrintOutput lastPrintOutput;
	std::string lastPrintFooter, lastPrintHeader, printError;
	std::function<void()> duringPrint;
	bool pdfWritten = false;
	void quit() override {}
	void drain() {
		for (;;) {
			std::function<void()> work;
			{
				std::lock_guard<std::mutex> lock(queueLock);
				if (queue.empty()) return;
				work = std::move(queue.front());
				queue.pop_front();
			}
			work();
		}
	}
};

struct Fixture {
	TestHost host;
	Workspace w;
	Fixture(const std::string &support)
		: w(host, OMAPAD_RESOURCES, support + "/settings", support + "/state") {
		host.workspace = &w;
		w.start();
	}
	SciView &v() { return host.paneView(w.focusedPane()); }
	void addText(const char *text) { v().send(SCI_ADDTEXT, strlen(text), reinterpret_cast<intptr_t>(text)); }
	void end() { v().send(SCI_DOCUMENTEND); }
};

static FindOptions findOptions(const std::string &what, const std::string &with, SearchMode mode) {
	FindOptions o;
	o.what = what;
	o.with = with;
	o.mode = mode;
	o.wrap = true;
	o.matchCase = true;
	return o;
}

static std::string encodeOrFail(const std::string &utf8, const char *id) {
	std::string out;
	check(encode(utf8, encodingWithIdentifier(id), out), "encode succeeds", id);
	return out;
}

static void testEncodings() {
	Decoded d = decodeAuto("hello");
	checkEq(d.encoding->identifier, "utf8", "plain ASCII reads as UTF-8");
	d = decodeAuto("caf\xC3\xA9");
	checkEq(d.encoding->identifier, "utf8", "valid UTF-8 detected");
	checkEq(d.utf8, "caf\xC3\xA9", "UTF-8 text unchanged");
	d = decodeAuto("\xEF\xBB\xBFhi");
	checkEq(d.encoding->identifier, "utf8bom", "UTF-8 BOM detected");
	checkEq(d.utf8, "hi", "BOM not part of the text");
	d = decodeAuto(std::string("\xFF\xFEh\0i\0", 6));
	checkEq(d.encoding->identifier, "utf16le", "UTF-16 LE BOM detected");
	checkEq(d.utf8, "hi", "UTF-16 LE decoded");
	d = decodeAuto(std::string("\xFE\xFF\0h\0i", 6));
	checkEq(d.encoding->identifier, "utf16be", "UTF-16 BE BOM detected");
	checkEq(d.utf8, "hi", "UTF-16 BE decoded");
	d = decodeAuto("caf\xE9");
	checkEq(d.encoding->identifier, "cp1252", "invalid UTF-8 falls back to Windows-1252");
	checkEq(d.utf8, "caf\xC3\xA9", "Windows-1252 decoded");

	// 0x81 is undefined in Windows-1252, so only Latin-1 can hold it without loss.
	const std::string weird = "a\x81z";
	d = decodeAuto(weird);
	std::string again;
	check(encode(d.utf8, d.encoding, again) && again == weird, "undefined bytes survive a round trip", d.encoding->identifier);

	// Every encoding must give back exactly what it read.
	for (const char *id : {"utf8", "utf8bom", "utf16le", "utf16be", "cp1252", "latin1"}) {
		const std::string text = "caf\xC3\xA9\r\nline two\n";
		const std::string encoded = encodeOrFail(text, id);
		Decoded back = decodeAuto(encoded);
		if (strcmp(id, "latin1") != 0) checkEq(back.encoding->identifier, id, "encoding detected after writing");
		checkEq(back.utf8, text, "text round trips");
	}
	// Each of these needs the operating system's converter to know the encoding.
	for (const Encoding &e : allEncodings()) {
		std::string bytes, out;
		check(encode("abc", &e, bytes), "encoding is available here", e.identifier);
		check(decodeAs(bytes, &e, out) && out == "abc", "encoding reads back", e.identifier);
	}
	std::string jp, jpBack;
	check(encode("\xE6\xBC\xA2", encodingWithIdentifier("shiftjis"), jp) && decodeAs(jp, encodingWithIdentifier("shiftjis"), jpBack) && jpBack == "\xE6\xBC\xA2", "Shift-JIS keeps kanji");

	std::string lost;
	check(!encode("\xF0\x9F\x98\x80", encodingWithIdentifier("cp1252"), lost), "an emoji can't silently vanish in Windows-1252");
	check(!encode("\xE6\xBC\xA2", encodingWithIdentifier("latin1"), lost), "CJK can't silently vanish in Latin-1");
	check(encode("\xF0\x9F\x98\x80", encodingWithIdentifier("utf16le"), lost), "UTF-16 holds an emoji");

	check(isValidUtf8("\xE2\x82\xAC", 3), "valid 3-byte sequence");
	check(!isValidUtf8("\xE2\x82", 2), "truncated sequence rejected");
	check(!isValidUtf8("\xC0\xAF", 2), "overlong sequence rejected");
	check(!isValidUtf8("\xED\xA0\x80", 3), "surrogate rejected");
	check(!isValidUtf8("\xF4\x90\x80\x80", 4), "beyond U+10FFFF rejected");
	check(isValidUtf8("", 0), "empty is valid");
	checkEq(decodeAuto("").encoding->identifier, "utf8", "empty file is UTF-8");
	// A big file exercises the converter's output growth.
	const std::string big(3 * 1024 * 1024, 'x');
	checkEq(decodeAuto(encodeOrFail(big, "utf16le")).utf8, big, "large UTF-16 round trip");
}

static void testEol() {
	check(detectEol("a\r\nb\n") == Eol::CRLF, "first line ending decides: CRLF");
	check(detectEol("a\nb\r\n") == Eol::LF, "first line ending decides: LF");
	check(detectEol("a\rb") == Eol::CR, "CR detected");
	check(detectEol("abc") == Eol::LF, "no line ending falls back");
	checkEq(convertEol("a\r\nb\nc\rd", Eol::LF), "a\nb\nc\nd", "mixed to LF");
	checkEq(convertEol("a\nb", Eol::CRLF), "a\r\nb", "LF to CRLF");
}

static void testLineOps() {
	checkEq(applyLineOp("b\na\nc\n", LineOp::SortLexAsc, Eol::LF), "a\nb\nc\n", "sort keeps the final newline at the end");
	checkEq(applyLineOp("b\na\nc", LineOp::SortLexAsc, Eol::LF), "a\nb\nc", "sort doesn't invent a final newline");
	checkEq(applyLineOp("b\na\nc", LineOp::SortLexDesc, Eol::LF), "c\nb\na", "sort descending");
	checkEq(applyLineOp("b\nA\nc", LineOp::SortFoldAsc, Eol::LF), "A\nb\nc", "sort ignoring case");
	checkEq(applyLineOp("\xC3\x89\n\xC3\xA8", LineOp::SortFoldAsc, Eol::LF), "\xC3\xA8\n\xC3\x89", "sort ignoring case handles accents");
	checkEq(applyLineOp("10\n9\n100", LineOp::SortIntAsc, Eol::LF), "9\n10\n100", "integers sort by value");
	checkEq(applyLineOp("10\nx\n9", LineOp::SortIntAsc, Eol::LF), "9\n10\nx", "non-numbers sort last");
	checkEq(applyLineOp("1.5\n1.25\n-2", LineOp::SortDecimalAsc, Eol::LF), "-2\n1.25\n1.5", "decimals");
	checkEq(applyLineOp("aaa\nb\ncc", LineOp::SortLengthAsc, Eol::LF), "b\ncc\naaa", "by length");
	checkEq(applyLineOp("\xC3\xA9\xC3\xA9\nabc", LineOp::SortLengthAsc, Eol::LF), "\xC3\xA9\xC3\xA9\nabc", "length counts characters, not bytes");
	checkEq(applyLineOp("a\nb\nc", LineOp::Reverse, Eol::LF), "c\nb\na", "reverse");
	checkEq(applyLineOp("a\nb\na\nc\nb", LineOp::RemoveDuplicates, Eol::LF), "a\nb\nc", "remove duplicates keeps the first");
	checkEq(applyLineOp("a\na\nb\na", LineOp::RemoveConsecutiveDuplicates, Eol::LF), "a\nb\na", "remove consecutive only");
	checkEq(applyLineOp("a\n\n  \nb", LineOp::RemoveEmpty, Eol::LF), "a\n  \nb", "remove empty leaves blanks");
	checkEq(applyLineOp("a\n\n  \nb", LineOp::RemoveBlank, Eol::LF), "a\nb", "remove blank");
	checkEq(applyLineOp("  a  \n\tb\t", LineOp::TrimTrailing, Eol::LF), "  a\n\tb", "trim trailing");
	checkEq(applyLineOp("  a  \n\tb\t", LineOp::TrimLeading, Eol::LF), "a  \nb\t", "trim leading");
	checkEq(applyLineOp("  a  ", LineOp::TrimBoth, Eol::LF), "a", "trim both");
	checkEq(applyLineOp("b\r\na\r\n", LineOp::SortLexAsc, Eol::CRLF), "a\r\nb\r\n", "CRLF preserved");
	checkEq(applyLineOp("", LineOp::SortLexAsc, Eol::LF), "", "empty text");
	checkEq(applyLineOp("\n\n", LineOp::RemoveEmpty, Eol::LF), "", "only empty lines");
	checkEq(joinLinesWithSpace("a\nb\r\nc\n"), "a b c", "EOL to space");
	checkEq(tabsToSpaces("\ta\tb", 4), "    a   b", "tabs align to stops");
	checkEq(spacesToTabs("        a", 4, true), "\t\ta", "leading spaces to tabs");
	checkEq(spacesToTabs("  a    b", 4, true), "  a    b", "partial indent and inner spaces stay when leading only");
	checkEq(spacesToTabs("    a b", 4, false), "\ta b", "a lone inner space stays a space");
	const std::string shuffled = applyLineOp("a\nb\nc\nd", LineOp::Randomize, Eol::LF);
	checkEq(applyLineOp(shuffled, LineOp::SortLexAsc, Eol::LF), "a\nb\nc\nd", "randomize keeps every line");
}

static void testCase() {
	checkEq(convertCase("Hello wOrld", CaseOp::Upper), "HELLO WORLD", "upper");
	checkEq(convertCase("Hello wOrld", CaseOp::Lower), "hello world", "lower");
	checkEq(convertCase("hello wOrld don't", CaseOp::Proper), "Hello World Don't", "proper");
	checkEq(convertCase("hello wOrld", CaseOp::ProperBlend), "Hello WOrld", "proper blend");
	checkEq(convertCase("hello WORLD. next one", CaseOp::Sentence), "Hello world. Next one", "sentence");
	checkEq(convertCase("hello WORLD. next", CaseOp::SentenceBlend), "Hello WORLD. Next", "sentence blend");
	checkEq(convertCase("aB", CaseOp::Invert), "Ab", "invert");
	checkEq(convertCase("\xC3\xA9", CaseOp::Upper), "\xC3\x89", "Unicode upper");
	checkEq(convertCase("\xC3\xA9t\xC3\xA9 \xC3\xA9t\xC3\xA9", CaseOp::Proper), "\xC3\x89t\xC3\xA9 \xC3\x89t\xC3\xA9", "Unicode proper");
	checkEq(convertCase("", CaseOp::Proper), "", "empty");
	checkEq(convertCase("\xF0\x9F\x98\x80 a", CaseOp::Proper), "\xF0\x9F\x98\x80 A", "emoji survives");
}

static void testExtended() {
	checkEq(unescapeExtended("a\\nb\\tc\\\\d"), "a\nb\tc\\d", "basic escapes");
	checkEq(unescapeExtended("\\x41\\d066\\o103"), "ABC", "numeric escapes");
	checkEq(unescapeExtended("\\u20AC"), "\xE2\x82\xAC", "unicode escape");
	checkEq(unescapeExtended("\\x4"), "\\x4", "incomplete stays literal");
	checkEq(unescapeExtended("\\q"), "\\q", "unknown stays literal");
	checkEq(unescapeExtended("end\\"), "end\\", "trailing backslash stays");
	const std::string nul = unescapeExtended("a\\0b");
	check(nul.size() == 3 && nul[1] == '\0', "NUL escape");
}

static void testComments() {
	using V = std::vector<std::string>;
	check(commentLines({"a", "  b"}, "#", CommentMode::Toggle) == V{"# a", "  # b"}, "comment keeps indent");
	check(commentLines({"# a", "  # b"}, "#", CommentMode::Toggle) == V{"a", "  b"}, "toggle uncomments");
	check(commentLines({"# a", "b"}, "#", CommentMode::Toggle) == V{"# # a", "# b"}, "mixed lines get commented");
	check(commentLines({"a", "", "b"}, "//", CommentMode::Toggle) == V{"// a", "", "// b"}, "blank lines untouched");
	check(commentLines({"#a"}, "#", CommentMode::Uncomment) == V{"a"}, "uncomment without a space");
	check(commentLines({""}, "#", CommentMode::Toggle) == V{""}, "only blank stays blank");
}

static void testHash() {
	checkEq(hashHex("abc", 3, HashKind::MD5), "900150983cd24fb0d6963f7d28e17f72", "MD5");
	checkEq(hashHex("abc", 3, HashKind::SHA1), "a9993e364706816aba3e25717850c26c9cd0d89d", "SHA-1");
	checkEq(hashHex("abc", 3, HashKind::SHA256), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256");
	checkEq(hashHex("abc", 3, HashKind::SHA512).substr(0, 16), "ddaf35a193617aba", "SHA-512");
}

static void testEditorFiles(const std::string &tmp, const std::string &support) {
	Fixture f(support);
	Workspace &e = f.w;
	TestHost &host = f.host;

	check(e.documentCount() == 1, "starts with one tab");
	checkEq(e.activeTitle(), "new 1", "first tab is untitled");

	// The empty first tab is replaced by the first file opened.
	const std::string a = writeFile(tmp, "a.py", "import os\nprint('caf\xC3\xA9')\n");
	check(e.openPath(a), "open a file");
	check(e.documentCount() == 1, "placeholder tab reused");
	checkEq(e.activeLanguageName(), "python", "language from extension");
	checkEq(e.text(), "import os\nprint('caf\xC3\xA9')\n", "text loaded");
	check(!e.activeIsDirty(), "fresh file is clean");

	// Re-opening focuses the existing tab.
	const std::string b = writeFile(tmp, "b.txt", "b\r\nb2\r\n");
	check(e.openPath(b), "open second");
	check(e.documentCount() == 2, "second tab");
	checkEq(e.activeEolName(), "Windows (CR LF)", "CRLF detected");
	e.openPath(a);
	check(e.documentCount() == 2 && e.activeTab(0) == 0, "same file not opened twice");

	// Each document keeps its own text across tab switches.
	f.v().send(SCI_GOTOPOS, 0);
	f.addText("X");
	check(e.activeIsDirty(), "edit marks dirty");
	e.clickTab(0, 1);
	checkEq(e.text(), "b\r\nb2\r\n", "other tab untouched");
	check(!e.activeIsDirty(), "other tab still clean");
	e.clickTab(0, 0);
	check(e.activeIsDirty(), "dirty state kept across switch");
	check(e.text().rfind("Ximport", 0) == 0, "edit kept across switch");

	// Saving writes exact bytes and clears the dirty state.
	check(e.saveActive(), "save");
	check(!e.activeIsDirty(), "clean after save");
	checkEq(readAll(a), "Ximport os\nprint('caf\xC3\xA9')\n", "saved bytes exact");
	f.v().send(SCI_UNDO);
	check(e.activeIsDirty(), "undo past the save point is dirty");
	f.v().send(SCI_REDO);
	check(!e.activeIsDirty(), "redo back to the save point is clean");

	// Line endings survive a save without being normalized.
	e.clickTab(0, 1);
	f.end();
	f.addText("c");
	check(e.saveActive(), "save CRLF file");
	checkEq(readAll(b), "b\r\nb2\r\nc", "CRLF bytes preserved");

	// Permissions survive the temporary-file save.
	chmod(b.c_str(), 0640);
	f.addText("d");
	check(e.saveActive(), "save with permissions");
	struct stat info;
	stat(b.c_str(), &info);
	check((info.st_mode & 0777) == 0640, "file permissions preserved");
	int leftovers = 0;
	for (const auto &entry : fs::directory_iterator(tmp))
		if (entry.path().filename().string().find(".omapad-") != std::string::npos) leftovers++;
	check(leftovers == 0, "no temporary files left behind");

	// A symlink stays a symlink, and its target gets the new contents.
	const std::string target = writeFile(tmp, "target.txt", "t");
	const std::string link = tmp + "/link.txt";
	fs::create_symlink(target, link);
	check(e.openPath(link), "open symlink");
	f.end();
	f.addText("u");
	check(e.saveActive(), "save through symlink");
	check(fs::is_symlink(link), "symlink not replaced by a file");
	checkEq(readAll(target), "tu", "symlink target updated");
	check(e.closeActive(), "close symlink tab");

	// Unwritable location: failure is reported and the text stays open and dirty.
	const std::string locked = tmp + "/locked";
	fs::create_directories(locked);
	const std::string lockedFile = writeFile(locked, "f.txt", "orig");
	check(e.openPath(lockedFile), "open file in folder");
	chmod(lockedFile.c_str(), 0400);
	chmod(locked.c_str(), 0500);
	f.addText("new");
	host.prompts = 0;
	check(!e.saveActive(), "save to read-only location fails");
	check(host.prompts == 1, "failed save tells the person");
	check(e.activeIsDirty(), "failed save stays dirty");
	checkEq(readAll(lockedFile), "orig", "original untouched by failed save");
	chmod(locked.c_str(), 0700);
	chmod(lockedFile.c_str(), 0600);

	// Close prompt: Cancel keeps it, Don't Save closes it.
	const int before = e.documentCount();
	host.answer = 1;  // Cancel.
	check(!e.closeActive(), "cancel keeps the tab");
	check(e.documentCount() == before, "tab still there");
	host.answer = 2;  // Don't Save.
	check(e.closeActive(), "don't save closes");
	check(e.documentCount() == before - 1, "tab gone");
	checkEq(readAll(lockedFile), "orig", "discarded edit not written");

	// Directories and missing files fail cleanly.
	host.prompts = 0;
	check(!e.openPath(tmp), "folder can't be opened");
	check(!e.openPath(tmp + "/missing.txt"), "missing file can't be opened");
	check(host.prompts == 2, "both failures reported");

	// Closing the last tab leaves one empty tab.
	host.answer = 2;
	while (e.tabs(0).size() > 1) e.closeActive();
	e.closeActive();
	check(e.documentCount() == 1, "always one tab");
	check(!e.activeIsDirty() && e.activePath().empty(), "the remaining tab is new and clean");
	checkEq(e.text(), "", "and empty");

	// Detection by first line and file name.
	e.openPath(writeFile(tmp, "runme", "#!/usr/bin/env python3\nprint(1)\n"));
	checkEq(e.activeLanguageName(), "python", "language from shebang");
	e.openPath(writeFile(tmp, "Makefile", "all:\n\techo\n"));
	checkEq(e.activeLanguageName(), "makefile", "language from file name");
	e.openPath(writeFile(tmp, "PKGBUILD", "pkgname=x\n"));
	checkEq(e.activeLanguageName(), "bash", "PKGBUILD is shell");

	// Save As picks up the language of the new name.
	e.createUntitledDocument();
	e.setText("x = 1\n");
	checkEq(e.activeLanguageName(), "normal", "untitled is plain text");
	check(e.saveActiveToPath(tmp + "/new.py"), "save as");
	checkEq(e.activeLanguageName(), "python", "language follows the new name");
	checkEq(e.activeTitle(), "new.py", "title follows");

	// Auto indent copies whitespace exactly.
	e.createUntitledDocument();
	e.setText("    a");
	f.end();
	f.v().send(SCI_NEWLINE);
	checkEq(e.text(), "    a\n    ", "auto indent keeps spaces");
	e.setText("\t  b");
	f.end();
	f.v().send(SCI_NEWLINE);
	checkEq(e.text(), "\t  b\n\t  ", "auto indent keeps a tab and space mix");

	// Commands reach their targets through the command table.
	e.setText("b\na\n");
	e.run(Cmd::LineOperation, static_cast<int>(LineOp::SortLexAsc));
	checkEq(e.text(), "a\nb\n", "line operation command");
	e.setSelection(0, 3);
	e.run(Cmd::Case, static_cast<int>(CaseOp::Upper));
	checkEq(e.text(), "A\nB\n", "case command");
	f.v().send(SCI_UNDO);
	checkEq(e.text(), "a\nb\n", "case command undoes in one step");
	e.setActiveLanguage("python");
	e.setSelection(0, 3);
	e.run(Cmd::Comment, static_cast<int>(CommentMode::Toggle));
	checkEq(e.text(), "# a\n# b\n", "comment command");
	e.run(Cmd::Comment, static_cast<int>(CommentMode::Toggle));
	checkEq(e.text(), "a\nb\n", "uncomment command");
	e.run(Cmd::CopyPath, 1);
	checkEq(host.clipboard, e.activeTitle(), "copy filename command");

	// Every menu item carries something a person can act on.
	int items = 0;
	std::function<void(const std::vector<MenuItem> &)> walk = [&](const std::vector<MenuItem> &menu) {
		for (const MenuItem &item : menu) {
			if (item.separator) continue;
			items++;
			check(!item.title.empty(), "menu item has a title");
			check(item.cmd != Cmd::None || !item.children.empty() || item.dynamic != MenuItem::Dynamic::None, "menu item does something", item.title);
			e.stateOf(item);
			walk(item.children);
		}
	};
	walk(e.mainMenu());
	check(items > 200, "menus are populated", std::to_string(items));
}

static void testEditorSearch(const std::string &tmp, const std::string &support) {
	Fixture f(support);
	Workspace &e = f.w;

	e.setText("foo bar\nFoo baz\nfoobar\n");
	e.setSelection(0, 0);
	FindOptions o = findOptions("foo", "", SearchMode::Normal);
	check(e.findNext(o) == SearchResult::Found, "find");
	checkEq(e.selectedText(), "foo", "selection is the match");
	check(e.findNext(o) == SearchResult::Found, "find again");
	check(f.v().send(SCI_GETSELECTIONSTART) == 16, "skips different case");
	check(e.findNext(o) == SearchResult::Found && f.v().send(SCI_GETSELECTIONSTART) == 0, "wraps around");
	o.wrap = false;
	e.setSelection(16, 19);
	check(e.findNext(o) == SearchResult::NotFound, "stops without wrap");
	o.wrap = true;
	o.backward = true;
	e.setSelection(16, 19);
	check(e.findNext(o) == SearchResult::Found && f.v().send(SCI_GETSELECTIONSTART) == 0, "finds backward");
	o.backward = false;

	check(e.countMatches(o) == 2, "count respects case");
	o.matchCase = false;
	check(e.countMatches(o) == 3, "count ignoring case");
	o.wholeWord = true;
	check(e.countMatches(o) == 2, "count whole words");

	// Literal mode treats regex characters literally.
	e.setText("a.b axb");
	o = findOptions("a.b", "X", SearchMode::Normal);
	check(e.replaceAll(o) == 1, "literal replace all");
	checkEq(e.text(), "X axb", "dot is literal");
	f.v().send(SCI_UNDO);
	checkEq(e.text(), "a.b axb", "one undo reverts replace all");

	// Regular expressions, with both group syntaxes.
	e.setText("john smith\njane doe\n");
	o = findOptions("(\\w+) (\\w+)", "\\2, \\1", SearchMode::Regex);
	check(e.replaceAll(o) == 2, "regex replace all");
	checkEq(e.text(), "smith, john\ndoe, jane\n", "backslash groups");
	e.setText("john smith\n");
	o.with = "$2-$1";
	check(e.replaceAll(o) == 1, "regex replace with dollar groups");
	checkEq(e.text(), "smith-john\n", "dollar groups");

	// Zero-length matches can't hang the loop.
	e.setText("a\nb\n");
	o = findOptions("^", "> ", SearchMode::Regex);
	check(e.replaceAll(o) >= 2, "line-start replace runs");
	check(e.text().rfind("> a\n> b\n", 0) == 0, "line-start replace result", e.text());
	e.setText("abc");
	o = findOptions("x*", "-", SearchMode::Regex);
	check(e.replaceAll(o) >= 3, "empty matches terminate");
	check(e.countMatches(findOptions("z*", "", SearchMode::Regex)) >= 1, "empty count terminates");
	e.setText("a\nb\n");
	e.setSelection(0, 0);
	o = findOptions("^", "", SearchMode::Regex);
	long previous = -1;
	bool moved = true;
	for (int i = 0; i < 3 && moved; i++) {
		e.findNext(o);
		const long now = f.v().send(SCI_GETCURRENTPOS);
		moved = now != previous;
		previous = now;
	}
	check(moved, "repeated find on an empty match moves on");

	// A bad regex is reported instead of crashing, and changes nothing.
	e.setText("text");
	o = findOptions("(unclosed", "x", SearchMode::Regex);
	check(e.findNext(o) == SearchResult::InvalidPattern, "invalid regex on find");
	check(e.countMatches(o) == -1, "invalid regex on count");
	check(e.replaceAll(o) == -1, "invalid regex on replace");
	checkEq(e.text(), "text", "text unchanged");

	// A very long line can't crash a regex search. The C++ library recurses per character.
	std::string longLine(4 * 1000 * 1000, 'a');
	longLine += "zz\n";
	e.setText(longLine);
	e.setSelection(0, 0);
	const SearchResult longResult = e.findNext(findOptions("(a|b)*zz", "", SearchMode::Regex));
	check(longResult == SearchResult::Found || longResult == SearchResult::InvalidPattern || longResult == SearchResult::NotFound, "long line regex survives");

	// Extended mode.
	e.setText("a\nb\n");
	o = findOptions("\\n", "\\t", SearchMode::Extended);
	check(e.replaceAll(o) == 2, "extended replace");
	checkEq(e.text(), "a\tb\t", "newlines became tabs");

	// Replace growing and shrinking, in a selection only.
	e.setText("aa aa aa");
	e.setSelection(3, 5);
	o = findOptions("a", "bbb", SearchMode::Normal);
	o.inSelection = true;
	check(e.replaceAll(o) == 2, "replace within selection");
	checkEq(e.text(), "aa bbbbbb aa", "only the selection changed");
	checkEq(e.selectedText(), "bbbbbb", "selection tracks the new length");

	// A replacement containing the pattern can't loop forever.
	e.setText("a a");
	o = findOptions("a", "aa", SearchMode::Normal);
	check(e.replaceAll(o) == 2, "self-containing replacement terminates");
	checkEq(e.text(), "aa aa", "self-containing result");

	// Replace one: only a selected match gets replaced, then it moves on.
	e.setText("cat cat");
	e.setSelection(0, 0);
	o = findOptions("cat", "dog", SearchMode::Normal);
	e.replaceOne(o);
	checkEq(e.text(), "cat cat", "first replace only selects");
	e.replaceOne(o);
	checkEq(e.text(), "dog cat", "second replaces the selection");
	checkEq(e.selectedText(), "cat", "and selects the next");

	// Non-ASCII and multi-byte searching.
	e.setText("caf\xC3\xA9 \xF0\x9F\x98\x80 caf\xC3\xA9");
	check(e.countMatches(findOptions("caf\xC3\xA9", "", SearchMode::Normal)) == 2, "accent search");
	o = findOptions("\xF0\x9F\x98\x80", "!", SearchMode::Normal);
	check(e.replaceAll(o) == 1, "emoji replace");
	checkEq(e.text(), "caf\xC3\xA9 ! caf\xC3\xA9", "emoji replaced");

	// Find All lists matches, and a result jumps to its match.
	e.setText("one\ntwo one\n");
	check(e.findAllInDocument(findOptions("one", "", SearchMode::Normal)) == 2, "find all count");
	check(f.host.resultsVisible, "results shown");
	check(e.resultsText().find("Line 2:") != std::string::npos, "results name the line");
	check(e.jumpToResultLine(2), "jump to a result");
	check(f.v().send(SCI_GETSELECTIONSTART) == 8, "jump selects the match");
	check(!e.jumpToResultLine(0), "the heading isn't a result");
	check(!e.jumpToResultLine(99), "beyond the end isn't a result");

	// Find in files.
	const std::string root = tmp + "/tree";
	fs::create_directories(root + "/sub");
	fs::create_directories(root + "/.git");
	writeFile(root, "one.txt", "needle here\nno match\nNEEDLE twice needle\n");
	writeFile(root + "/sub", "two.py", "# needle\n");
	writeFile(root + "/.git", "config", "needle\n");
	writeFile(root, "binary.bin", std::string("needle\0\0\0", 9));
	writeFile(root, "wide.txt", encodeOrFail("needle16\n", "utf16le"));
	writeFile(root, "accent.txt", "CAF\xC3\x89 caf\xC3\xA9\n");
	std::string hugeLine(2 * 1000 * 1000, 'q');
	writeFile(root, "minified.js", hugeLine + "needleq\n");

	long matches = -2, files = -2;
	const auto search = [&](const FindOptions &options) {
		matches = -2;
		e.findInFiles(options, [&](long m, long n) { matches = m; files = n; });
		for (int i = 0; i < 6000 && matches == -2; i++) {
			f.host.drain();
			usleep(10 * 1000);
		}
		f.host.drain();
	};
	o = findOptions("needle", "", SearchMode::Normal);
	o.directory = root;
	o.filters = "*";
	o.recursive = true;
	search(o);
	check(matches == 5, "find in files respects case, skips binary and hidden", std::to_string(matches));
	check(files == 4, "file count", std::to_string(files));
	check(e.resultsText().find("two.py") != std::string::npos, "results list the files");
	o.includeHidden = true;
	search(o);
	check(matches == 6, "hidden folders on request", std::to_string(matches));
	o.includeHidden = false;
	o.recursive = false;
	search(o);
	check(matches == 4, "no subfolders on request", std::to_string(matches));
	o.recursive = true;
	o.filters = "*.py";
	search(o);
	check(matches == 1 && files == 1, "filter by name");
	o.filters = "*";
	o.matchCase = false;
	search(o);
	check(matches == 6, "case-insensitive in files", std::to_string(matches));
	o.what = "caf\xC3\xA9";
	search(o);
	check(matches == 2, "case-insensitive handles accents", std::to_string(matches));
	o.what = "needle";
	o.wholeWord = true;
	o.matchCase = true;
	search(o);
	check(matches == 3, "whole word in files", std::to_string(matches));
	o.wholeWord = false;
	o.mode = SearchMode::Regex;
	o.what = "nee+dle\\d+";
	search(o);
	check(matches == 1, "regex in files, including UTF-16", std::to_string(matches));
	o.what = "(a|q)*needle";
	search(o);
	check(matches >= 1, "regex on a long line survives", std::to_string(matches));
	o.what = "(bad";
	search(o);
	check(matches == -1, "invalid regex in files");

	// A result from Find in Files opens the file at the match.
	o = findOptions("twice", "", SearchMode::Normal);
	o.directory = root;
	search(o);
	check(matches == 1, "single result");
	check(e.jumpToResultLine(2), "jump into a file");
	checkEq(fs::path(e.activePath()).filename().string(), "one.txt", "the right file opened");
	checkEq(e.selectedText(), "twice", "the match is selected");
}

static void testEncodingAndExternal(const std::string &tmp, const std::string &support) {
	Fixture f(support);
	Workspace &e = f.w;
	TestHost &host = f.host;

	// A Windows-1252 file reads and saves byte for byte.
	const std::string path = writeFile(tmp, "latin.txt", "caf\xE9\n");
	e.openPath(path);
	checkEq(e.activeEncodingIdentifier(), "cp1252", "encoding detected in editor");
	checkEq(e.text(), "caf\xC3\xA9\n", "shown correctly");
	f.end();
	f.addText("x");
	check(e.saveActive(), "save Windows-1252");
	checkEq(readAll(path), "caf\xE9\nx", "written in original encoding");

	// A character the encoding can't hold blocks the save instead of losing it.
	f.addText("\xF0\x9F\x98\x80");
	host.prompts = 0;
	check(!e.saveActive(), "unrepresentable save refused");
	check(host.prompts == 1, "and explained");
	check(e.activeIsDirty(), "still unsaved");
	checkEq(readAll(path), "caf\xE9\nx", "file untouched");

	// Converting to something that can't hold the text is refused too.
	host.prompts = 0;
	check(!e.convertActiveToEncoding("latin1"), "conversion refused");
	check(host.prompts == 1, "conversion refusal explained");
	checkEq(e.activeEncodingIdentifier(), "cp1252", "encoding unchanged");

	// Converting to UTF-8 marks it unsaved, and saving writes UTF-8.
	check(e.convertActiveToEncoding("utf8"), "convert to UTF-8");
	check(e.activeIsDirty(), "conversion is unsaved");
	check(e.saveActive(), "save after converting");
	const std::string afterConvert = readAll(path);
	check(isValidUtf8(afterConvert.data(), afterConvert.size()), "file is UTF-8 now");
	check(!e.activeIsDirty(), "clean after saving the conversion");

	check(e.convertActiveToEncoding("utf16le"), "convert to UTF-16");
	check(e.saveActive(), "save UTF-16");
	checkEq(readAll(path).substr(0, 2), "\xFF\xFE", "BOM written");

	// Reinterpreting reads the same bytes differently without touching the file.
	const std::string ambiguous = "caf\xC3\xA9";
	const std::string ambiguousPath = writeFile(tmp, "ambiguous.txt", ambiguous);
	e.openPath(ambiguousPath);
	checkEq(e.activeEncodingIdentifier(), "utf8", "valid UTF-8 first");
	check(e.reinterpretActiveAsEncoding("latin1"), "reopen as Latin-1");
	checkEq(e.text(), "caf\xC3\x83\xC2\xA9", "bytes read as Latin-1");
	check(!e.activeIsDirty(), "reinterpreting isn't an edit");
	checkEq(readAll(ambiguousPath), ambiguous, "file untouched by reopening");
	e.openPath(writeFile(tmp, "invalid.txt", "\xFF\xFF\xFFz"));
	host.prompts = 0;
	check(!e.reinterpretActiveAsEncoding("utf8"), "invalid bytes refused");
	check(host.prompts == 1, "refusal explained");
	check(!e.text().empty(), "text still there");

	// Changed on disk, kept or reloaded.
	const std::string watched = writeFile(tmp, "watched.txt", "v1\n");
	e.openPath(watched);
	sleepForMtime();
	writeFile(tmp, "watched.txt", "v2\n");
	host.answer = 0;  // Keep.
	host.prompts = 0;
	e.checkExternalChanges();
	check(host.prompts == 1, "external change noticed");
	checkEq(e.text(), "v1\n", "kept current text");
	host.prompts = 0;
	e.checkExternalChanges();
	check(host.prompts == 0, "not asked again about the same change");
	sleepForMtime();
	writeFile(tmp, "watched.txt", "v3\n");
	host.answer = 1;  // Reload.
	e.checkExternalChanges();
	checkEq(e.text(), "v3\n", "reloaded");
	check(!e.activeIsDirty(), "clean after reload");

	// Deleted on disk: the text stays and is marked unsaved.
	fs::remove(watched);
	host.prompts = 0;
	e.checkExternalChanges();
	check(host.prompts == 1, "deletion noticed");
	checkEq(e.text(), "v3\n", "text kept after deletion");
	check(e.activeIsDirty(), "marked unsaved after deletion");
	host.prompts = 0;
	e.checkExternalChanges();
	check(host.prompts == 0, "not asked again about the deletion");
	check(e.saveActive(), "saving recreates it");
	checkEq(readAll(watched), "v3\n", "recreated contents");
}

static void testSplitView(const std::string &tmp, const std::string &support) {
	Fixture f(support);
	Workspace &e = f.w;
	TestHost &host = f.host;

	const std::string a = writeFile(tmp, "split-a.txt", "alpha\nbeta\ngamma\n");
	const std::string b = writeFile(tmp, "split-b.txt", "second file\n");
	e.openPath(a);
	e.openPath(b);
	check(!e.splitVisible(), "one view to start");

	// Moving needs a tab left behind in the first view.
	check(e.moveTabToOtherView(0, 1), "move to second view");
	check(host.splitVisible, "second view appears");
	check(e.focusedPane() == 1, "focus follows the moved tab");
	check(e.tabs(0).size() == 1 && e.tabs(1).size() == 1, "one tab in each view");
	checkEq(e.activePath(), b, "second view shows the moved file");
	checkEq(e.text(), "second file\n", "with its text");
	host.flashed.clear();
	check(!e.moveTabToOtherView(0, 0), "the first view keeps its only tab");
	check(!host.flashed.empty(), "and says why");

	// Each view edits independently.
	e.paneFocused(0);
	checkEq(e.activePath(), a, "first view shows the first file");
	f.end();
	f.addText("one");
	check(e.activeIsDirty(), "edit in the first view");
	e.paneFocused(1);
	check(!e.activeIsDirty(), "second view's file is clean");

	// Moving the only tab out of the second view closes that view, and loses nothing.
	f.addText("two");
	check(e.moveTabToOtherView(1, 0), "move back");
	check(!host.splitVisible, "second view goes away");
	check(e.focusedPane() == 0, "focus returns to the first view");
	check(e.tabs(0).size() == 2, "both tabs together");
	checkEq(e.activePath(), b, "moved tab is active");
	check(e.activeIsDirty(), "unsaved edit survived the move");
	checkEq(e.text(), "twosecond file\n", "text survived the move");

	// A clone is the same document in two views.
	host.answer = 2;
	e.clickTab(0, 0);
	const int documentsBefore = e.documentCount();
	check(e.cloneTabToOtherView(0, 0), "clone");
	check(host.splitVisible, "clone opens the second view");
	check(e.documentCount() == documentsBefore, "a clone adds no document");
	checkEq(e.activePath(), a, "clone shows the same file");
	f.v().send(SCI_DOCUMENTSTART);
	f.addText("shared ");
	const std::string shared = e.text();
	e.paneFocused(0);
	checkEq(e.text(), shared, "an edit in one view shows in the other");
	check(e.cloneTabToOtherView(0, 0), "cloning again");
	check(e.tabs(1).size() == 1, "doesn't add a second clone");

	// Scroll and selection stay separate per view.
	e.paneFocused(0);
	e.setSelection(1, 3);
	e.paneFocused(1);
	e.setSelection(5, 9);
	e.paneFocused(0);
	check(f.v().send(SCI_GETSELECTIONSTART) == 1, "first view keeps its selection");
	e.paneFocused(1);
	check(f.v().send(SCI_GETSELECTIONSTART) == 5, "second view keeps its selection");

	// Closing one view of a dirty clone asks nothing and keeps the text.
	host.prompts = 0;
	check(e.closeTab(1, 0), "close the clone");
	check(host.prompts == 0, "no prompt while another view has it");
	check(!host.splitVisible, "second view closes with its last tab");
	check(e.focusedPane() == 0, "focus back in the first view");
	e.clickTab(0, 0);
	checkEq(e.text(), shared, "text still open");
	check(e.activeIsDirty(), "and still unsaved");

	// Closing the last copy of a dirty document does ask.
	host.prompts = 0;
	host.answer = 1;
	check(!e.closeTab(0, 0), "last copy prompts and can cancel");
	check(host.prompts == 1, "prompted");

	// Saving in one view clears dirty in the other.
	e.cloneTabToOtherView(0, 0);
	check(e.saveActive(), "save from the clone");
	e.paneFocused(0);
	e.clickTab(0, 0);
	check(!e.activeIsDirty(), "clean in both views");

	// Opening a file already in the other view goes there rather than duplicating it.
	const int beforeOpen = e.documentCount();
	e.paneFocused(0);
	e.openPath(a);
	check(e.documentCount() == beforeOpen, "no duplicate document");

	// A document deleted in one view doesn't crash the other when reloaded.
	e.paneFocused(1);
	host.answer = 2;
	e.closeTab(1, 0);

	// Stacking and synchronized scrolling.
	std::string many;
	for (int i = 0; i < 400; i++) many += "line " + std::to_string(i) + "\n";
	const std::string m = writeFile(tmp, "split-long.txt", many);
	e.openPath(m);
	e.cloneTabToOtherView(e.focusedPane(), e.activeTab(e.focusedPane()));
	e.run(Cmd::RotateSplit);
	check(host.splitStacked, "stack views");
	e.paneFocused(0);
	e.run(Cmd::SyncVertical);
	host.paneView(0).send(SCI_SETFIRSTVISIBLELINE, 50);
	SCNotification scroll{};
	scroll.nmhdr.code = SCN_UPDATEUI;
	scroll.updated = SC_UPDATE_V_SCROLL;
	e.editorNotification(0, scroll);
	check(host.paneView(1).send(SCI_GETFIRSTVISIBLELINE) >= 40, "scrolling follows when synchronized", std::to_string(host.paneView(1).send(SCI_GETFIRSTVISIBLELINE)));
	e.run(Cmd::SyncVertical);

	// Replace in all open documents reaches both views' files, and doesn't disturb them.
	e.paneFocused(0);
	const int tabs0 = e.activeTab(0);
	const long replaced = e.replaceAllOpenDocuments(findOptions("line 3\n", "LINE\n", SearchMode::Normal));
	check(replaced >= 1, "replace in all open documents", std::to_string(replaced));
	check(e.activeTab(0) == tabs0, "active tab didn't change");
	check(e.activeIsDirty(), "changed document is dirty");
	f.v().send(SCI_UNDO);
}

static void testFolders(const std::string &tmp, const std::string &support) {
	Fixture f(support);
	Workspace &e = f.w;
	TestHost &host = f.host;

	const std::string root = tmp + "/project";
	fs::create_directories(root + "/src/deep");
	fs::create_directories(root + "/.hidden");
	writeFile(root, "readme.md", "# hi\n");
	writeFile(root, "Zeta.txt", "z\n");
	writeFile(root, ".env", "secret\n");
	writeFile(root + "/src", "main.py", "print(1)\n");
	fs::create_directory_symlink(root, root + "/src/loop");

	check(e.addFolder(root), "add a folder");
	check(host.folderVisible, "panel shows");
	check(e.folders().size() == 1, "one root");
	check(e.addFolder(root + "/"), "adding again");
	check(e.folders().size() == 1, "doesn't duplicate, even with a trailing slash");
	check(e.isFolderExpanded(root), "root starts expanded");
	host.prompts = 0;
	check(!e.addFolder(root + "/readme.md"), "a file isn't a folder");
	check(host.prompts == 1, "and says so");

	auto entries = e.listFolder(root);
	check(entries.size() == 3, "hidden entries left out", std::to_string(entries.size()));
	checkEq(entries[0].name, "src", "folders first");
	check(entries[0].isDirectory, "folder flagged");
	checkEq(entries[1].name, "readme.md", "then files, ignoring case");
	checkEq(entries[2].name, "Zeta.txt", "sorted");
	e.run(Cmd::ToggleHiddenInFolders);
	check(e.listFolder(root).size() == 5, "hidden entries on request");
	e.run(Cmd::ToggleHiddenInFolders);
	check(e.listFolder(root + "/missing").empty(), "missing folder lists nothing");

	// A symlink to a folder can't loop the tree.
	bool sawLoop = false;
	for (const FolderEntry &entry : e.listFolder(root + "/src")) {
		if (entry.name != "loop") continue;
		sawLoop = true;
		check(entry.isSymlink && !entry.isDirectory, "a linked folder doesn't expand");
	}
	check(sawLoop, "link is listed");

	// New files open straight away. Bad names are refused.
	std::string error;
	check(e.createFileIn(root, "new.py", error), "new file", error);
	checkEq(e.activePath(), root + "/new.py", "new file opened");
	checkEq(e.activeLanguageName(), "python", "with its language");
	check(!e.createFileIn(root, "new.py", error), "can't create over an existing file");
	checkEq(readAll(root + "/readme.md"), "# hi\n", "other files untouched");
	for (const char *bad : {"", ".", "..", "a/b", "../escape"}) {
		check(!e.createFileIn(root, bad, error), "bad file name refused", bad);
		check(!e.createFolderIn(root, bad, error), "bad folder name refused", bad);
		check(!e.renamePath(root + "/Zeta.txt", bad, error), "bad rename refused", bad);
	}
	check(!fs::exists(tmp + "/escape"), "nothing escaped the folder");
	check(e.createFolderIn(root, "docs", error), "new folder", error);
	check(fs::is_directory(root + "/docs"), "folder exists");
	check(e.isFolderExpanded(root), "parent stays expanded");

	// Renaming keeps open documents attached, including unsaved text.
	f.addText("x = 1\n");
	check(e.renamePath(root + "/new.py", "renamed.txt", error), "rename", error);
	checkEq(e.activePath(), root + "/renamed.txt", "open tab follows the rename");
	checkEq(e.activeLanguageName(), "normal", "language follows the new name");
	check(e.activeIsDirty(), "unsaved text kept");
	check(e.saveActive(), "save after rename");
	checkEq(readAll(root + "/renamed.txt"), "x = 1\n", "saved to the new name");
	check(!fs::exists(root + "/new.py"), "old name gone");
	check(!e.renamePath(root + "/renamed.txt", "Zeta.txt", error), "rename can't overwrite");
	checkEq(readAll(root + "/Zeta.txt"), "z\n", "the existing file survives");

	// Renaming a folder carries everything open inside it.
	e.openPath(root + "/src/main.py");
	e.setFolderExpanded(root + "/src", true);
	check(e.renamePath(root + "/src", "code", error), "rename folder", error);
	checkEq(e.activePath(), root + "/code/main.py", "open tab follows its folder");
	check(e.isFolderExpanded(root + "/code"), "expansion follows too");
	check(!e.isFolderExpanded(root + "/src"), "the old path is gone");
	f.end();
	f.addText("y");
	check(e.saveActive(), "save inside the renamed folder");
	checkEq(readAll(root + "/code/main.py"), "print(1)\ny", "written to the new location");
	sleepForMtime();
	host.prompts = 0;
	e.checkExternalChanges();
	check(host.prompts == 0, "a rename isn't an outside change");

	// Trash: clean tabs close, unsaved ones stay marked.
	host.answer = 1;  // Confirm.
	e.openPath(writeFile(root, "clean.txt", "c\n"));
	const int beforeTrash = e.documentCount();
	e.runFolderCommand(Cmd::FolderTrash, root + "/clean.txt");
	check(!fs::exists(root + "/clean.txt"), "file trashed");
	check(e.documentCount() == beforeTrash - 1, "its clean tab closed");
	e.openPath(writeFile(root, "dirty.txt", "d\n"));
	f.addText("edit");
	e.runFolderCommand(Cmd::FolderTrash, root + "/dirty.txt");
	checkEq(e.activePath(), root + "/dirty.txt", "unsaved tab stays");
	check(e.activeIsDirty(), "still unsaved");
	host.answer = 0;  // Decline.
	const std::string keep = writeFile(root, "keep.txt", "k\n");
	e.runFolderCommand(Cmd::FolderTrash, keep);
	check(fs::exists(keep), "declining keeps the file");

	// Prompted creation through the folder command.
	host.promptAccepted = true;
	host.promptAnswer = "typed.md";
	e.runFolderCommand(Cmd::FolderNewFile, root);
	check(fs::exists(root + "/typed.md"), "prompted file created");
	host.promptAccepted = false;
	host.promptAnswer = "not-this.md";
	e.runFolderCommand(Cmd::FolderNewFile, root);
	check(!fs::exists(root + "/not-this.md"), "cancelling creates nothing");

	e.run(Cmd::FolderCopyPath, 0, root + "/readme.md");
	checkEq(host.clipboard, root + "/readme.md", "copy path");
	check(!e.folderMenu(root, true, true).empty(), "root has a menu");
	check(!e.folderMenu(root + "/readme.md", false, false).empty(), "file has a menu");

	e.removeFolder(root);
	check(e.folders().empty(), "removed from the workspace");
	check(fs::exists(root + "/readme.md"), "removing doesn't touch the disk");
	check(!host.folderVisible, "panel hides with nothing to show");
	host.answer = 2;
}

static void testSession(const std::string &tmp) {
	const std::string support = tmp + "/session-support";
	const std::string clean = writeFile(tmp, "s-clean.txt", "clean\n");
	const std::string dirty = writeFile(tmp, "s-dirty.py", "disk\n");
	const std::string gone = writeFile(tmp, "s-gone.txt", "gone\n");
	const std::string right = writeFile(tmp, "s-right.txt", "right\n");
	const std::string folder = tmp + "/s-folder";
	fs::create_directories(folder + "/inner");
	{
		Fixture f(support);
		Workspace &e = f.w;
		e.openPath(clean);
		e.setSelection(2, 4);
		e.openPath(dirty);
		f.end();
		f.addText("unsave");
		e.createUntitledDocument();
		e.setText("untitled work \xF0\x9F\x98\x80\n");
		e.openPath(gone);
		e.openPath(right);
		check(e.moveTabToOtherView(0, e.activeTab(0)), "move to second view");
		e.cloneTabToOtherView(0, 1) ;  // A dirty document, showing in both views.
		e.run(Cmd::RotateSplit);
		e.addFolder(folder);
		e.setFolderExpanded(folder + "/inner", true);
		e.paneFocused(0);
		e.clickTab(0, 1);
		check(e.writeSession(), "session written");
		struct stat info;
		stat((support + "/state/session.json").c_str(), &info);
		check((info.st_mode & 0777) == 0600, "session file is private");
		int backups = 0;
		for (const auto &entry : fs::directory_iterator(support + "/state/backup")) { (void)entry; backups++; }
		check(backups == 2, "one backup per unsaved document", std::to_string(backups));
		check(e.writeSession(), "writing twice is fine");
	}
	fs::remove(gone);
	checkEq(readAll(dirty), "disk\n", "unsaved text never reached the original file");
	{
		Fixture f(support);
		Workspace &e = f.w;
		check(e.restoreSession(), "session restored");
		check(e.documentCount() == 4, "all but the deleted clean file", std::to_string(e.documentCount()));
		check(e.splitVisible(), "second view restored");
		check(f.host.splitVisible, "and shown");
		check(f.host.splitStacked, "stacking restored");
		check(e.focusedPane() == 0, "focused view restored");
		check(e.tabs(1).size() == 2, "second view's tabs restored", std::to_string(e.tabs(1).size()));
		checkEq(e.activePath(), dirty, "right tab active");
		checkEq(e.text(), "disk\nunsave", "unsaved edits restored");
		check(e.activeIsDirty(), "restored edits still unsaved");
		checkEq(e.activeLanguageName(), "python", "language restored");
		e.clickTab(0, 0);
		checkEq(e.text(), "clean\n", "clean file restored from disk");
		check(!e.activeIsDirty(), "clean file still clean");
		checkEq(e.selectedText(), "ea", "selection restored");
		e.clickTab(0, 2);
		checkEq(e.text(), "untitled work \xF0\x9F\x98\x80\n", "untitled work restored");
		checkEq(e.activeTitle(), "new 1", "untitled name restored");
		check(e.activePath().empty(), "untitled still has no path");
		check(e.folders().size() == 1 && e.folders()[0] == folder, "folder restored");
		check(e.folderPanelVisible() && f.host.folderVisible, "panel restored");
		check(e.isFolderExpanded(folder + "/inner"), "expansion restored");

		// The restored clone is really one document: an edit shows in both views.
		e.clickTab(0, 1);
		f.end();
		f.addText("!");
		const std::string edited = e.text();
		for (int i = 0; i < static_cast<int>(e.tabs(1).size()); i++) {
			e.clickTab(1, i);
			if (e.activePath() == dirty) checkEq(e.text(), edited, "clone restored as one document");
		}

		// Saving a restored document clears its backup.
		e.paneFocused(0);
		e.clickTab(0, 1);
		check(e.saveActive(), "save a restored document");
		checkEq(readAll(dirty), edited, "restored text saved");
		check(!e.activeIsDirty(), "clean after saving");
		check(e.writeSession(), "session rewritten");
		int backups = 0;
		for (const auto &entry : fs::directory_iterator(support + "/state/backup")) { (void)entry; backups++; }
		check(backups == 1, "its backup is gone", std::to_string(backups));
	}

	// A hostile session file can't read or delete anything outside the backup folder.
	const std::string hostile = tmp + "/hostile-support";
	fs::create_directories(hostile + "/state/backup");
	const std::string secret = writeFile(tmp, "secret.txt", "secret\n");
	writeFile(hostile + "/state", "session.json",
	          "{\"version\":2,\"focus\":\"x\",\"folders\":[\"/nonexistent\",7],\"documents\":["
	          "{\"backup\":\"../../secret\",\"untitledName\":\"evil\"},\"not a dictionary\",{\"path\":42},"
	          "{\"path\":\"" + secret + "\",\"encoding\":\"nope\",\"eol\":77,\"language\":\"nope\"}],"
	          "\"panes\":[{\"tabs\":[{\"document\":3,\"anchor\":-5,\"caret\":99999999,\"firstLine\":-1},{\"document\":99},\"x\"],\"active\":\"y\"},\"bad\"]}");
	{
		Fixture f(hostile);
		Workspace &e = f.w;
		check(e.restoreSession(), "malformed session handled");
		check(e.documentCount() == 1, "only the valid entry opened", std::to_string(e.documentCount()));
		checkEq(e.activePath(), secret, "the valid entry");
		check(e.folders().empty(), "a missing folder is dropped");
		check(e.writeSession(), "session rewritten after hostile input");
	}
	checkEq(readAll(secret), "secret\n", "file outside the backup folder untouched");

	// Garbage instead of JSON just means no session.
	const std::string garbage = tmp + "/garbage-support";
	fs::create_directories(garbage + "/state");
	writeFile(garbage + "/state", "session.json", "{not json");
	writeFile(garbage, "settings.json", "garbage");
	{
		Fixture f(garbage);
		check(!f.w.restoreSession(), "garbage session ignored");
		check(f.w.documentCount() == 1, "still usable");
	}
	// Settings outlive the app, and bad values are ignored.
	const std::string prefs = tmp + "/prefs-support";
	{
		Fixture f(prefs);
		f.w.run(Cmd::ToggleWrap);
		f.w.run(Cmd::SetTabWidth, 8);
		f.w.run(Cmd::SetTheme, 0, "Monokai");
	}
	{
		Fixture f(prefs);
		check(f.w.settings.wordWrap, "word wrap remembered");
		check(f.w.settings.tabWidth == 8, "tab width remembered");
		checkEq(f.w.settings.theme, "Monokai", "theme remembered");
		check(f.host.paneView(0).send(SCI_GETWRAPMODE) == SC_WRAP_WORD, "word wrap applied");
	}
	fs::create_directories(tmp + "/badprefs/settings");
	writeFile(tmp + "/badprefs/settings", "settings.json", "{\"tabWidth\":-3,\"fontSize\":99999,\"theme\":5,\"recent\":[1,\"x\"]}");
	{
		Fixture f(tmp + "/badprefs");
		check(f.w.settings.tabWidth == 4, "bad tab width ignored");
		check(f.w.settings.fontSize < 100, "bad font size ignored");
		check(f.w.settings.recent.size() == 1, "bad recent entry ignored");
	}
}

static void testOmarchyTheme(const std::string &tmp, const std::string &support) {
	const std::string toml =
		"# comment\nmode = \"dark\"\n\naccent = \"#7aa2f7\"\nselection = \"#292e42\"\nmuted = \"#414868\"\n"
		"background = \"#1a1b26\"\nlighter_background = \"#24283b\"\nforeground = \"#a9b1d6\"\ndark_foreground = \"#565f89\"\n"
		"red = \"#f7768e\"\nyellow = \"#e0af68\"\norange = \"#eb927b\"\ngreen = \"#9ece6a\"\ncyan = \"#449dab\"\nmagenta = \"#ad8ee6\"\nblue = \"#7aa2f7\"\n"
		"bad = \"#zzzzzz\"\nshort = \"#fff\"\n[section]\nbackground_ignored\n";
	Palette palette = parsePalette(toml);
	check(palette.valid(), "palette parses");
	check(palette.dark, "mode read");
	checkEq(palette.get("background"), "1a1b26", "colour read without the hash");
	check(!palette.colors.count("bad"), "bad colour ignored");
	check(!palette.colors.count("short"), "short colour ignored");
	check(!parsePalette("").valid(), "empty palette isn't valid");
	check(!parsePalette("mode = \"light\"\nbackground = \"#fff\"").valid(), "partial palette isn't valid");
	check(!parsePalette("mode = \"light\"\nbackground = \"#ffffff\"\nforeground = \"#000000\"").dark, "light mode read");
	check(parsePalette(std::string(100000, '=')).colors.empty(), "garbage parses to nothing");

	const std::string themeFile = writeFile(tmp, "colors.toml", toml);
	setenv("OMAPAD_OMARCHY_THEME", themeFile.c_str(), 1);
	{
		Fixture f(support + "-omarchy");
		Workspace &e = f.w;
		checkEq(e.effectiveTheme(), "Omarchy", "follows Omarchy automatically");
		check(e.themeIsDark(), "theme is dark");
		const ChromeColors chrome = e.chromeColors();
		check(chrome.valid, "chrome colours available");
		checkEq(chrome.background, "1a1b26", "chrome background");
		checkEq(chrome.accent, "7aa2f7", "chrome accent");

		const auto rgb = [](int hex) { return static_cast<long>(((hex & 0xFF) << 16) | (hex & 0xFF00) | ((hex >> 16) & 0xFF)); };
		e.openPath(writeFile(tmp, "o.py", "def f():\n    return \"s\"  # note\n"));
		check(f.v().send(SCI_STYLEGETBACK, STYLE_DEFAULT) == rgb(0x1a1b26), "editor background matches Omarchy");
		f.v().send(SCI_COLOURISE, 0, -1);
		const auto colourAt = [&](long position) { return static_cast<long>(f.v().send(SCI_STYLEGETFORE, f.v().send(SCI_GETSTYLEAT, position))); };
		check(colourAt(0) == rgb(0xad8ee6), "keywords use the palette", std::to_string(colourAt(0)));
		check(colourAt(e.text().find('"')) == rgb(0x9ece6a), "strings use the palette");
		check(colourAt(e.text().find('#')) == rgb(0x565f89), "comments use the palette");

		// Switching Omarchy's theme changes omapad right away.
		std::string light = toml;
		light.replace(light.find("#1a1b26"), 7, "#fafafa");
		writeFile(tmp, "colors.toml", light);
		check(e.reloadOmarchyTheme(), "theme change noticed");
		check(f.v().send(SCI_STYLEGETBACK, STYLE_DEFAULT) == rgb(0xfafafa), "new colours applied to open tabs");
		check(!e.reloadOmarchyTheme(), "no change, no work");

		// A half written file never replaces a good palette.
		writeFile(tmp, "colors.toml", "mode = \"dark\"\nback");
		check(!e.reloadOmarchyTheme(), "half written palette ignored");
		check(f.v().send(SCI_STYLEGETBACK, STYLE_DEFAULT) == rgb(0xfafafa), "colours kept");
		writeFile(tmp, "colors.toml", toml);
		e.reloadOmarchyTheme();

		// A chosen theme wins, and Automatic goes back to following Omarchy.
		e.run(Cmd::SetTheme, 0, "Monokai");
		checkEq(e.effectiveTheme(), "Monokai", "a chosen theme wins");
		check(!e.chromeColors().valid, "and has no Omarchy chrome");
		e.run(Cmd::SetTheme, 0, "Auto");
		checkEq(e.effectiveTheme(), "Omarchy", "automatic follows Omarchy again");
	}
	setenv("OMAPAD_OMARCHY_THEME", (tmp + "/does-not-exist.toml").c_str(), 1);
	{
		Fixture f(support + "-no-omarchy");
		checkEq(f.w.effectiveTheme(), "", "without Omarchy, the default theme");
		check(!f.w.chromeColors().valid, "and no Omarchy chrome");
	}
}

static void testHighlighting(const std::string &tmp, const std::string &support) {
	Fixture f(support);
	Workspace &e = f.w;
	const auto styleAt = [&](long position) {
		f.v().send(SCI_COLOURISE, 0, -1);
		return static_cast<long>(f.v().send(SCI_GETSTYLEAT, position));
	};
	e.openPath(writeFile(tmp, "h.py", "def f():\n    return 1  # note\n"));
	check(styleAt(0) == 5, "Python keyword styled", std::to_string(styleAt(0)));
	check(styleAt(e.text().find('#')) == 1, "Python comment styled");
	check(f.v().send(SCI_GETFOLDLEVEL, 0) & SC_FOLDLEVELHEADERFLAG, "Python folding works");

	e.openPath(writeFile(tmp, "h.cpp", "int main() { return 0; } // c\n"));
	check(styleAt(0) != 0, "C++ type keyword styled", std::to_string(styleAt(0)));
	check(styleAt(13) == 5, "C++ keyword styled", std::to_string(styleAt(13)));

	e.openPath(writeFile(tmp, "h.html", "<html><script>var x = 1;</script></html>\n"));
	check(styleAt(1) == 1, "HTML tag styled", std::to_string(styleAt(1)));
	const long var = static_cast<long>(e.text().find("var"));
	check(styleAt(var) != 0 && styleAt(var) != styleAt(1), "embedded script styled differently");

	// Every language can be selected and lexed without crashing, in every theme mode.
	e.setText("if (x) { y = \"s\"; } // c\n# d\n<a b='c'>\n");
	int languages = 0, withoutLexer = 0;
	std::function<void(const std::vector<MenuItem> &)> walk = [&](const std::vector<MenuItem> &menu) {
		for (const MenuItem &item : menu) {
			walk(item.children);
			if (item.cmd != Cmd::SetLanguage) continue;
			e.run(Cmd::SetLanguage, 0, item.text);
			f.v().send(SCI_COLOURISE, 0, -1);
			languages++;
			char lexer[64] = {0};
			f.v().send(SCI_GETLEXERLANGUAGE, 0, reinterpret_cast<intptr_t>(lexer));
			if (item.text != "normal" && item.text != "nfo" && (!strcmp(lexer, "null") || !lexer[0])) {
				withoutLexer++;
				printf("  no lexer for language: %s\n", item.text.c_str());
			}
		}
	};
	walk(e.mainMenu());
	check(languages >= 90, "all languages exercised", std::to_string(languages));
	check(withoutLexer == 0, "every language has a real lexer", std::to_string(withoutLexer));
}

static void testRegexEngine() {
	Regex bad("(unclosed", Regex::Options{});
	check(!bad.valid() && !bad.error().empty(), "a bad pattern reports its error");

	const std::string subject = "alpha\nbeta\ngamma";
	Regex::Match m;
	Regex lookbehind("(?<=alpha\\n)beta", Regex::Options{});
	check(lookbehind.search(subject.data(), subject.size(), 0, m) == Regex::Result::Found && m.start == 6 && m.end == 10, "lookbehind");
	Regex keep("alpha\\n\\Kbeta", Regex::Options{});
	check(keep.search(subject.data(), subject.size(), 0, m) == Regex::Result::Found && m.start == 6, "\\K resets the match start");
	Regex across("alpha\\nbeta", Regex::Options{});
	check(across.search(subject.data(), subject.size(), 0, m) == Regex::Result::Found, "a pattern can cross lines");
	Regex dotStops("alpha.beta", Regex::Options{});
	check(dotStops.search(subject.data(), subject.size(), 0, m) == Regex::Result::NotFound, ". stops at a newline by default");
	Regex::Options dotAll;
	dotAll.dotMatchesNewline = true;
	Regex dotCrosses("alpha.beta", dotAll);
	check(dotCrosses.search(subject.data(), subject.size(), 0, m) == Regex::Result::Found, ". crosses a newline when asked");
	Regex anchors("^beta$", Regex::Options{});
	check(anchors.search(subject.data(), subject.size(), 0, m) == Regex::Result::Found, "^ and $ match at line boundaries");

	Regex::Options fold;
	fold.ignoreCase = true;
	const std::string accented = "ÉCOLE école";
	Regex unicode("école", fold);
	check(unicode.search(accented.data(), accented.size(), 0, m) == Regex::Result::Found && m.start == 0, "ignoring case works on accents");

	// A very long line used to overflow the stack with the C++ regex library.
	const std::string longLine(4 * 1024 * 1024, 'a');
	Regex star("(a|b)*c", Regex::Options{});
	check(star.search(longLine.data(), longLine.size(), 0, m) != Regex::Result::Found, "a huge line doesn't crash");
	const std::string invalid = std::string("ok \xFF\xFE bad needle");
	Regex needle("needle", Regex::Options{});
	check(needle.search(invalid.data(), invalid.size(), 0, m) == Regex::Result::Found, "invalid UTF-8 doesn't stop a search");

	const std::string text = "John Smith";
	Regex names("(\\w+) (\\w+)", Regex::Options{});
	check(names.search(text.data(), text.size(), 0, m) == Regex::Result::Found, "groups match");
	checkEq(expandReplacement("\\2, \\1", text.data(), m), "Smith, John", "backslash groups");
	checkEq(expandReplacement("$2 ${1}x", text.data(), m), "Smith Johnx", "dollar groups");
	checkEq(expandReplacement("\\U\\1\\E \\L\\2", text.data(), m), "JOHN smith", "case changes");
	checkEq(expandReplacement("a\\tb\\nc\\\\", text.data(), m), "a\tb\nc\\", "escapes");
}

static void testRegexInDocument(const std::string &support) {
	Fixture f(support);
	Workspace &e = f.w;
	e.setText("one\ntwo\nthree\n");
	FindOptions o = findOptions("one\\ntwo", "joined", SearchMode::Regex);
	check(e.countMatches(o) == 1, "multi-line pattern counts");
	check(e.replaceAll(o) == 1, "multi-line pattern replaces");
	checkEq(e.text(), "joined\nthree\n", "multi-line replacement");

	e.setText("key=value\nname=omapad\n");
	o = findOptions("(\\w+)=(\\w+)", "\\2:\\1", SearchMode::Regex);
	check(e.replaceAll(o) == 2, "group replace all");
	checkEq(e.text(), "value:key\nomapad:name\n", "group replacement");

	// A zero-length match must not loop forever.
	e.setText("abc");
	o = findOptions("x*", "-", SearchMode::Regex);
	check(e.replaceAll(o) == 4, "empty matches advance", std::to_string(e.replaceAll(findOptions("nomatch", "", SearchMode::Regex))));
	checkEq(e.text(), "-a-b-c-", "empty match replacement");

	e.setText("price 10\nprice 20\n");
	o = findOptions("(?<=price )\\d+", "N", SearchMode::Regex);
	check(e.replaceAll(o) == 2, "lookbehind in a document");
	checkEq(e.text(), "price N\nprice N\n", "lookbehind replacement");

	e.setText("start\nmiddle\nend\n");
	o = findOptions("start.*end", "X", SearchMode::Regex);
	check(e.countMatches(o) == 0, "dot stays on its line");
	o.dotMatchesNewline = true;
	check(e.countMatches(o) == 1, ". matches newline option");

	o = findOptions("(bad", "", SearchMode::Regex);
	check(e.findNext(o) == SearchResult::InvalidPattern, "bad pattern reported by find");
	check(e.countMatches(o) == -1, "bad pattern reported by count");
	check(e.replaceAll(o) == -1, "bad pattern reported by replace");

	e.setText("héllo wörld\n");
	e.setSelection(0, 0);
	o = findOptions("w.rld", "", SearchMode::Regex);
	check(e.findNext(o) == SearchResult::Found, "regex finds across multi-byte text");
	checkEq(e.selectedText(), "wörld", "the regex match is whole characters");
	f.host.answer = 0;
	e.run(Cmd::CloseAll);
}

static void testPreferences(const std::string &tmp) {
	const std::string support = tmp + "/prefs";
	{
		Fixture f(support);
		std::vector<FormField> fields = f.w.preferenceFields();
		const auto set = [&](const char *key, const std::string &value) {
			for (FormField &field : fields) if (field.key == key) { field.value = value; return; }
			check(false, "preference field exists", key);
		};
		check(f.v().send(SCI_GETMARGINWIDTHN, 0) > 0, "line numbers start on");
		set("lineNumbers", "0");
		set("edgeColumn", "80");
		set("caretWidth", "3");
		set("tabWidth", "999");  // Past the limit, so it's clamped.
		set("tabsToSpaces", "1");
		set("scrollBeyondEnd", "1");
		set("highlightCurrentLine", "0");
		set("defaultEol", "crlf");
		set("autoComplete", "0");
		set("backupSeconds", "120");
		f.w.applyPreferenceFields(fields);
		check(f.v().send(SCI_GETMARGINWIDTHN, 0) == 0, "line numbers turn off");
		check(f.v().send(SCI_GETEDGECOLUMN) == 80 && f.v().send(SCI_GETEDGEMODE) == EDGE_LINE, "column guide shows");
		check(f.v().send(SCI_GETCARETWIDTH) == 3, "caret width applies");
		check(f.v().send(SCI_GETTABWIDTH) == 32, "tab width is clamped");
		check(f.v().send(SCI_GETUSETABS) == 0, "tabs to spaces applies");
		check(f.v().send(SCI_GETENDATLASTLINE) == 0, "scroll past end applies");
		check(f.v().send(SCI_GETCARETLINEVISIBLE) == 0, "current line highlight turns off");
		check(f.host.paneView(1).send(SCI_GETEDGECOLUMN) == 80, "the other pane gets it too");
		f.w.createUntitledDocument();
		check(f.v().send(SCI_GETEOLMODE) == SC_EOL_CRLF, "new documents use the default line ending");

		f.host.formAnswer = [&](std::vector<FormField> &form) {
			for (FormField &field : form) if (field.key == "edgeColumn") field.value = "100";
			return true;
		};
		f.w.run(Cmd::ShowPreferences);
		checkEq(f.host.lastFormTitle, "Preferences", "preferences open a form");
		check(f.v().send(SCI_GETEDGECOLUMN) == 100, "the form's answer applies");
		f.host.formAnswer = [&](std::vector<FormField> &form) {
			for (FormField &field : form) if (field.key == "edgeColumn") field.value = "60";
			return false;
		};
		f.w.run(Cmd::ShowPreferences);
		check(f.v().send(SCI_GETEDGECOLUMN) == 100, "cancelling changes nothing");
		f.w.run(Cmd::CloseAll);
	}
	Fixture f(support);
	check(f.w.settings.edgeColumn == 100 && !f.w.settings.lineNumbers && f.w.settings.defaultEol == "crlf" && !f.w.settings.autoComplete && f.w.settings.backupSeconds == 120,
	      "preferences persist");
	check(f.v().send(SCI_GETEDGECOLUMN) == 100 && f.v().send(SCI_GETMARGINWIDTHN, 0) == 0, "preferences apply at launch");

	f.w.run(Cmd::CloseAll);
}

static void testPanelsAndBackupInterval(const std::string &tmp) {
	const std::string support = tmp + "/panels";
	const std::string state = support + "/state/session.json";
	const std::string path = writeFile(tmp, "panels.py", "def one():\n    pass\n");
	{
		Fixture f(support);
		check(f.w.openPath(path), "open for the session");
		f.w.run(Cmd::ToggleFunctionList);
		f.w.run(Cmd::ToggleDocumentMap);

		// Backups wait for their interval, counted in ticks of a second.
		f.w.settings.backupSeconds = 3;
		std::error_code error;
		fs::remove(state, error);
		f.w.tick();
		f.w.tick();
		check(!fs::exists(state), "no backup before the interval");
		f.w.tick();
		check(fs::exists(state), "a backup at the interval");
		fs::remove(state, error);
		f.w.settings.backupSeconds = 0;
		for (int i = 0; i < 5; i++) f.w.tick();
		check(!fs::exists(state), "an interval of 0 means no timed backups");
		f.w.settings.backupSeconds = 7;
		// A session is only rewritten when it changes, so change it to bring the file back.
		f.w.run(Cmd::ToggleFunctionList);
		f.w.writeSession();
		f.w.run(Cmd::ToggleFunctionList);
		check(f.w.writeSession() && fs::exists(state), "session saved");
	}
	{
		Fixture f(support);
		check(f.w.restoreSession(), "session restored");
		checkEq(f.w.activePath(), path, "the file came back");
		check(f.host.functionVisible && f.w.functionListVisible(), "the Function List came back");
		check(f.host.mapVisible && f.w.documentMapVisible(), "the Document Map came back");
		check(f.host.map->send(SCI_GETDOCPOINTER) == f.v().send(SCI_GETDOCPOINTER), "the restored map shows the restored document");
		f.w.settings.restoreSessionOnStart = false;
		std::vector<FormField> fields = f.w.preferenceFields();
		for (FormField &field : fields) if (field.key == "restoreSessionOnStart") field.value = "0";
		f.w.applyPreferenceFields(fields);
		check(f.w.writeSession(), "session saved again");
	}
	{
		Fixture f(support);
		check(!f.w.settings.restoreSessionOnStart, "the session preference persists");
		f.w.restoreSession();
		check(f.w.activePath().empty(), "with it off, nothing is reopened", f.w.activePath());
		f.host.answer = 2;
		f.w.run(Cmd::CloseAll);
	}
}

static void testShortcuts(const std::string &tmp) {
	const std::string support = tmp + "/keys";
	checkEq(normalizeShortcut("shift+mod+f"), "Mod+Shift+F", "shortcut text is normalized");
	checkEq(normalizeShortcut("Shift+Alt+Mod+u"), "Mod+Alt+Shift+U", "modifiers have one order");
	checkEq(normalizeShortcut("ALT+ctrl+Space"), "Ctrl+Alt+Space", "named keys keep their name");
	checkEq(normalizeShortcut(""), "", "no shortcut stays empty");
	{
		Fixture f(support);
		std::map<std::string, std::string> seen;
		for (const ShortcutEntry &entry : f.w.shortcutEntries()) {
			if (entry.defaultShortcut.empty()) continue;
			const auto other = seen.find(entry.defaultShortcut);
			check(other == seen.end(), "no two commands share a default shortcut", entry.defaultShortcut + ": " + entry.id + (other == seen.end() ? "" : " and " + other->second));
			seen[entry.defaultShortcut] = entry.id;
		}
		check(seen.size() > 40, "many commands have shortcuts");
		const auto entryFor = [&](const std::string &id) {
			for (const ShortcutEntry &entry : f.w.shortcutEntries()) if (entry.id == id) return entry;
			return ShortcutEntry{};
		};
		checkEq(entryFor("Edit/Undo").shortcut, "Mod+Z", "ids are menu paths");

		check(f.w.assignShortcut("Edit/Undo", "Mod+Alt+Shift+U"), "assigning a free shortcut");
		checkEq(entryFor("Edit/Undo").shortcut, "Mod+Alt+Shift+U", "the new shortcut shows");
		check(f.host.menusChangedCount == 1, "menus are rebuilt");
		const auto menuShortcut = [&](const std::string &menuTitle, const std::string &title) {
			for (const MenuItem &top : f.w.mainMenu())
				if (top.title == menuTitle)
					for (const MenuItem &item : top.children)
						if (item.title == title) return item.shortcut;
			return std::string("(missing)");
		};
		checkEq(menuShortcut("Edit", "Undo"), "Mod+Alt+Shift+U", "the menu shows the new shortcut");

		f.host.answer = 0;
		check(!f.w.assignShortcut("Edit/Redo", "Mod+Alt+Shift+U"), "declining a conflict refuses");
		check(f.host.lastTitle.find("already used") != std::string::npos, "a conflict asks first");
		checkEq(entryFor("Edit/Undo").shortcut, "Mod+Alt+Shift+U", "the old command keeps it");
		f.host.answer = 1;
		check(f.w.assignShortcut("Edit/Redo", "Mod+Alt+Shift+U"), "accepting a conflict moves it");
		checkEq(entryFor("Edit/Redo").shortcut, "Mod+Alt+Shift+U", "the new command has it");
		checkEq(entryFor("Edit/Undo").shortcut, "", "the old command loses it");
		check(!f.w.assignShortcut("Edit/NoSuchThing", "Mod+Q"), "unknown commands are refused");
		f.w.run(Cmd::ShowShortcutMapper);
		check(f.host.mapperRuns == 1, "the mapper opens");
		f.w.run(Cmd::CloseAll);
	}
	{
		Fixture f(support);
		bool found = false;
		for (const ShortcutEntry &entry : f.w.shortcutEntries())
			if (entry.id == "Edit/Redo") found = entry.shortcut == "Mod+Alt+Shift+U";
		check(found, "shortcuts persist");
		f.w.resetShortcuts();
		for (const ShortcutEntry &entry : f.w.shortcutEntries())
			check(entry.shortcut == entry.defaultShortcut, "reset restores defaults", entry.id);
		f.w.run(Cmd::CloseAll);
	}
}

static void testCompletion(const std::string &tmp, const std::string &support) {
	Fixture f(support);
	Workspace &e = f.w;
	const std::string path = writeFile(tmp, "complete.py", "");
	check(e.openPath(path), "open python");

	std::vector<std::string> names = e.completionCandidates("pri", true, false, 0);
	check(std::find(names.begin(), names.end(), "print") != names.end(), "language keywords complete");

	e.setText("alphabet alpha alpine other\nalp");
	e.setSelection(e.text().size(), e.text().size());
	names = e.completionCandidates("alp", false, true, e.text().size());
	checkEq(names.size() == 3 ? names[0] + "," + names[1] + "," + names[2] : "wrong count", "alpha,alphabet,alpine", "document words, sorted, without the word being typed");

	check(e.showCompletion(false, true, true), "a completion list opens");
	checkEq(e.lastCompletionList, "alpha\nalphabet\nalpine", "the list has the words");
	f.v().send(SCI_AUTOCCANCEL);

	e.setText("zzz");
	e.setSelection(3, 3);
	check(!e.showCompletion(false, true, true), "nothing to offer");
	checkEq(f.host.flashed, "No completions", "an explicit request says why");

	// Typing only offers once the threshold is reached.
	e.setText("hello hxllo\nh");
	e.setSelection(e.text().size(), e.text().size());
	check(!e.showCompletion(false, true, false), "below the threshold");
	e.setText("hello hxllo\nhe");
	e.setSelection(e.text().size(), e.text().size());
	check(e.showCompletion(false, true, false), "at the threshold");
	f.v().send(SCI_AUTOCCANCEL);

	// Function hints.
	const std::string cpp = writeFile(tmp, "hint.php", "");
	check(e.openPath(cpp), "open php");
	check(e.activeLanguageName() == "php", "php detected", e.activeLanguageName());
	e.setText("<?php substr(");
	e.setSelection(e.text().size(), e.text().size());
	check(e.showParameterHint(true), "hint opens inside a call");
	check(e.hintActive() && e.hintParameter() == 0, "on the first parameter");
	e.setText("<?php substr($a, ");
	e.setSelection(e.text().size(), e.text().size());
	check(e.showParameterHint(true) && e.hintParameter() == 1, "commas move to the next parameter");
	e.setText("<?php substr(strlen($a), ");
	e.setSelection(e.text().size(), e.text().size());
	check(e.showParameterHint(true) && e.hintParameter() == 1, "nested calls are skipped");
	e.setText("<?php substr(strlen(");
	e.setSelection(e.text().size(), e.text().size());
	check(e.showParameterHint(true) && e.hintParameter() == 0, "the innermost call wins");
	e.setText("<?php nosuchfunction(");
	e.setSelection(e.text().size(), e.text().size());
	check(!e.showParameterHint(true) && !e.hintActive(), "unknown functions have no hint");

	// Paths complete from the disk.
	writeFile(tmp, "pathtarget.txt", "x");
	e.setText(tmp + "/pathtar");
	e.setSelection(e.text().size(), e.text().size());
	e.run(Cmd::CompletePath);
	checkEq(e.lastCompletionList, "pathtarget.txt", "paths complete");
	f.v().send(SCI_AUTOCCANCEL);

	ApiKeyword keyword;
	keyword.name = "max";
	ApiOverload overload;
	overload.returnValue = "int";
	overload.parameters = {"int a", "int b"};
	overload.description = "bigger one";
	keyword.overloads = {overload, overload};
	const std::string tip = formatHint(keyword, overload, "(", ")", ",", 0, 2);
	check(tip.find("int max(int a, int b)") != std::string::npos && tip.find("bigger one") != std::string::npos, "hint text", tip);
	const auto range = hintHighlight(keyword, overload, "(", ",", 0, 2, 1);
	checkEq(tip.substr(range.first, range.second - range.first), "int b", "hint highlights the current parameter");
	e.run(Cmd::CloseAll);
}

static void testFunctionList(const std::string &tmp, const std::string &support) {
	FunctionListCatalog catalog(OMAPAD_RESOURCES);
	const auto names = [](const std::vector<FunctionEntry> &entries) {
		std::string out;
		for (const FunctionEntry &entry : entries) out += (entry.className.empty() ? "" : entry.className + "::") + entry.name + " ";
		return out;
	};
	check(catalog.supports("python") && catalog.supports("cpp"), "languages with rules");
	check(!catalog.supports("nosuchlanguage"), "languages without rules");
	checkEq(names(catalog.parse("python", "def top():\n    pass\n\nclass Thing:\n    def method(self):\n        pass\n# def commented():\n")),
	        "top() Thing::method(self) ", "python functions and classes");
	checkEq(names(catalog.parse("c", "int add(int a, int b) { return a + b; }\n/* int gone(void) {} */\nvoid run(void)\n{\n}\n")), "add(int a, int b) run(void) ", "c functions, skipping comments");
	const std::string cppNames = names(catalog.parse("cpp", "class Box {\npublic:\n  int size() { return 1; }\n};\nint Box2::width() { return 2; }\nint main() { return 0; }\n"));
	check(cppNames.find("main") != std::string::npos && cppNames.find("size") != std::string::npos, "c++ functions", cppNames);
	const std::string javaNames = names(catalog.parse("java", "public class App {\n  public void go(int x) { }\n  private int stop() { return 1; }\n}\n"));
	check(javaNames.find("go") != std::string::npos && javaNames.find("stop") != std::string::npos, "java methods", javaNames);
	const std::string jsNames = names(catalog.parse("javascript.js", "function hello(a) { }\nconst x = 1;\n"));
	check(jsNames.find("hello") != std::string::npos, "javascript functions", jsNames);
	const std::string rubyNames = names(catalog.parse("ruby", "def greet(name)\nend\n"));
	check(rubyNames.find("greet") != std::string::npos, "ruby methods", rubyNames);
	const std::string bashNames = names(catalog.parse("bash", "build() {\n  echo\n}\nfunction deploy {\n  echo\n}\n"));
	check(bashNames.find("build") != std::string::npos && bashNames.find("deploy") != std::string::npos, "bash functions", bashNames);
	const std::string rustNames = names(catalog.parse("rust", "fn alpha() {}\nimpl Thing {\n  pub fn beta(&self) {}\n}\n"));
	check(rustNames.find("alpha") != std::string::npos && rustNames.find("beta") != std::string::npos, "rust functions", rustNames);

	// Every rule file parses, and none hangs on ordinary text.
	std::error_code error;
	long files = 0;
	std::string sample = "def a(): pass\nfunction b() { }\nvoid c(int x) { }\nclass D { void e() { } }\nsub f { }\n";
	for (int i = 0; i < 8; i++) sample += sample;
	for (const auto &file : fs::directory_iterator(std::string(OMAPAD_RESOURCES) + "/functionList", error)) {
		const std::string language = file.path().stem().string();
		check(catalog.supports(language), "rule file loads", language);
		catalog.parse(language, sample);
		files++;
	}
	check(files == 47, "all rule files were tried", std::to_string(files));

	Fixture f(support);
	Workspace &e = f.w;
	const std::string path = writeFile(tmp, "list.py", "def first():\n    pass\n\ndef second():\n    pass\n");
	check(e.openPath(path), "open a file for the list");
	check(e.activeLanguageHasFunctionList(), "python has a list");
	e.run(Cmd::ToggleFunctionList);
	check(f.host.functionVisible && e.functionListVisible(), "the panel opens");
	check(e.functionEntries().size() == 2, "the panel lists functions", std::to_string(e.functionEntries().size()));
	const int changes = f.host.functionListChangedCount;
	e.goToFunction(1);
	check(f.v().send(SCI_LINEFROMPOSITION, f.v().send(SCI_GETCURRENTPOS)) == 3, "going to a function moves the caret");
	e.goToFunction(99);  // Past the end, as a stale click might be.

	// After an edit, it refreshes once typing pauses, not on every key.
	e.setSelection(e.text().size(), e.text().size());
	const char *added = "def third():\n    pass\n";
	f.v().send(SCI_ADDTEXT, strlen(added), reinterpret_cast<intptr_t>(added));
	e.tick();
	check(f.host.functionListChangedCount == changes, "no refresh right after an edit");
	e.tick();
	check(f.host.functionListChangedCount == changes + 1 && e.functionEntries().size() == 3, "it refreshes after a pause");
	e.tick();
	check(f.host.functionListChangedCount == changes + 1, "no refresh when nothing changed");
	e.run(Cmd::ToggleFunctionList);
	check(!f.host.functionVisible, "the panel closes");
	e.run(Cmd::CloseAll);
}

static void testMultiSelectAndColumns(const std::string &support) {
	Fixture f(support);
	Workspace &e = f.w;
	e.setText("foo bar foo baz foo\n");
	e.setSelection(0, 3);
	e.run(Cmd::MultiSelectAll, 1);
	check(f.v().send(SCI_GETSELECTIONS) == 3, "select all occurrences", std::to_string(f.v().send(SCI_GETSELECTIONS)));
	e.setSelection(0, 3);
	e.run(Cmd::MultiSelectNext, 1);
	check(f.v().send(SCI_GETSELECTIONS) == 2, "select the next occurrence");
	check(f.v().send(SCI_GETSELECTIONNSTART, 1) == 8, "that's the second one");
	e.run(Cmd::MultiSelectSkip, 1);
	check(f.v().send(SCI_GETSELECTIONS) == 2 && f.v().send(SCI_GETSELECTIONNSTART, f.v().send(SCI_GETMAINSELECTION)) == 16, "skip moves on to the third");
	e.run(Cmd::MultiSelectUndo);
	check(f.v().send(SCI_GETSELECTIONS) == 1, "undo drops the latest");
	e.setText("foo bar\n");
	e.setSelection(1, 1);
	e.run(Cmd::MultiSelectAll, 1);
	checkEq(e.selectedText(), "foo", "with no selection, it starts from the word at the caret");

	// Typing into all of them.
	check(f.w.settings.multiEditing, "multi-edit starts on");
	check(f.v().send(SCI_GETADDITIONALSELECTIONTYPING) == 1, "multi-edit reaches the editor");

	e.setText("a\nb\nc\n");
	e.setSelection(1, 1);
	check(e.columnInsertText("X"), "column text");
	checkEq(e.text(), "aX\nbX\ncX\n X", "text fills to the end, padding as needed");
	check(!e.columnInsertText(""), "empty column text does nothing");

	e.setText("aaaa\nb\ncccc");
	e.setSelection(2, 2);
	check(e.columnInsertText("|"), "pads short lines");
	checkEq(e.text(), "aa|aa\nb |\ncc|cc", "short lines stay straight");

	e.setText("a\nb\nc\nd\n");
	e.setSelection(0, 0);
	e.columnInsertNumbers(1, 1, 1, false, 10);
	checkEq(e.text(), "1a\n2b\n3c\n4d\n5", "numbers count up");
	e.setText(std::string("x\n") + "x\n" + "x\n" + "x\n" + "x\n" + "x\n" + "x\n" + "x\n" + "x\n" + "x\n" + "x");
	e.setSelection(0, 0);
	e.columnInsertNumbers(1, 1, 1, true, 10);
	check(e.text().rfind("01x\n", 0) == 0 && e.text().find("11x") != std::string::npos, "leading zeros", e.text());
	e.setText("x\nx\nx\nx");
	e.setSelection(0, 0);
	e.columnInsertNumbers(10, 5, 2, false, 10);
	checkEq(e.text(), "10x\n10x\n15x\n15x", "repeat holds each value");
	e.setText("x\nx\nx");
	e.setSelection(0, 0);
	e.columnInsertNumbers(14, 1, 1, false, 16);
	checkEq(e.text(), " Ex\n Fx\n10x", "hexadecimal, right-aligned");

	e.setText("aaa\nbbb\nccc\nddd");
	f.v().send(SCI_SETRECTANGULARSELECTIONANCHOR, 1);
	f.v().send(SCI_SETRECTANGULARSELECTIONCARET, f.v().send(SCI_FINDCOLUMN, 1, 1));
	check(f.v().send(SCI_SELECTIONISRECTANGLE) != 0, "a rectangle is selected");
	e.columnInsertNumbers(1, 1, 1, false, 10);
	checkEq(e.text(), "a1aa\nb2bb\nccc\nddd", "a rectangle limits the lines");

	checkEq(formatNumber(255, 16, 0), "FF", "hex format");
	checkEq(formatNumber(5, 2, 0), "101", "binary format");
	checkEq(formatNumber(-8, 8, 0), "-10", "negative octal");
	checkEq(formatNumber(7, 10, 3), "007", "width pads");

	f.host.formAnswer = [](std::vector<FormField> &fields) {
		for (FormField &field : fields) if (field.key == "text") field.value = "T";
		return true;
	};
	e.setText("a\nb");
	e.setSelection(1, 1);
	e.run(Cmd::ColumnEditor);
	checkEq(f.host.lastFormTitle, "Column Editor", "the column form opens");
	checkEq(e.text(), "aT\nbT", "the form inserts");
	f.v().send(SCI_UNDO);
	checkEq(e.text(), "a\nb", "one undo reverses it all");
	e.run(Cmd::CloseAll);
}

static void testChangeHistoryAndStyles(const std::string &tmp, const std::string &support) {
	Fixture f(support);
	Workspace &e = f.w;
	const std::string path = writeFile(tmp, "history.txt", "one\ntwo\nthree\nfour\n");
	check(e.openPath(path), "open for change history");
	check(f.v().send(SCI_GETCHANGEHISTORY) != 0, "change history is on");
	const long mask = 0xFL << SC_MARKNUM_HISTORY_REVERTED_TO_ORIGIN;
	const auto firstChanged = [&]() {
		for (long line = 0; line < f.v().send(SCI_GETLINECOUNT); line++)
			if (f.v().send(SCI_MARKERGET, line) & mask) return line;
		return -1L;
	};
	check(firstChanged() < 0, "opening a file isn't a change");
	f.v().send(SCI_GOTOLINE, 2);
	f.v().send(SCI_ADDTEXT, 1, reinterpret_cast<intptr_t>("X"));
	check(firstChanged() == 2, "an edit marks its line");
	f.v().send(SCI_GOTOLINE, 0);
	e.run(Cmd::NextChange);
	check(f.v().send(SCI_LINEFROMPOSITION, f.v().send(SCI_GETCURRENTPOS)) == 2, "next change");
	e.run(Cmd::NextChange);
	check(f.v().send(SCI_LINEFROMPOSITION, f.v().send(SCI_GETCURRENTPOS)) == 2, "next change wraps");
	f.host.answer = 0;
	e.run(Cmd::ClearChangeHistory);
	check(firstChanged() == 2, "declining keeps the history");
	f.host.answer = 1;
	e.run(Cmd::ClearChangeHistory);
	check(firstChanged() < 0, "clearing removes the marks");
	check(f.v().send(SCI_GETCHANGEHISTORY) != 0, "history keeps recording after a clear");
	f.host.flashed.clear();
	e.run(Cmd::NextChange);
	checkEq(f.host.flashed, "No changes", "nothing to go to");
	// Two separate changes, the first spanning two lines, and wrapping both ways.
	f.v().send(SCI_GOTOLINE, 0);
	f.v().send(SCI_ADDTEXT, 1, reinterpret_cast<intptr_t>("Y"));
	f.v().send(SCI_GOTOLINE, 1);
	f.v().send(SCI_ADDTEXT, 1, reinterpret_cast<intptr_t>("Y"));
	f.v().send(SCI_GOTOLINE, 3);
	f.v().send(SCI_ADDTEXT, 1, reinterpret_cast<intptr_t>("Y"));
	const auto caretLine = [&]() { return f.v().send(SCI_LINEFROMPOSITION, f.v().send(SCI_GETCURRENTPOS)); };
	e.run(Cmd::NextChange);
	check(caretLine() == 0, "next wraps to the first change", std::to_string(caretLine()));
	e.run(Cmd::NextChange);
	check(caretLine() == 3, "a run of lines is one change", std::to_string(caretLine()));
	e.run(Cmd::PreviousChange);
	check(caretLine() == 0, "previous lands on the start of a run", std::to_string(caretLine()));
	e.run(Cmd::PreviousChange);
	check(caretLine() == 3, "previous wraps", std::to_string(caretLine()));
	f.host.answer = 0;
	e.run(Cmd::CloseAll);

	Fixture g(support);
	Workspace &w = g.w;
	w.setText("foo bar\nbaz foo\nfoo\nfoobar\n");
	w.setSelection(0, 3);
	w.run(Cmd::StyleAllOccurrences, 1);
	const long indicator = 25;
	check(g.v().send(SCI_INDICATORVALUEAT, indicator, 0) != 0, "style marks the first");
	check(g.v().send(SCI_INDICATORVALUEAT, indicator, 12) != 0, "style marks the others");
	check(g.v().send(SCI_INDICATORVALUEAT, indicator, 4) == 0, "style leaves the rest alone");
	w.setSelection(0, 3);
	w.run(Cmd::JumpStyleDown, 1);
	checkEq(w.selectedText(), "foo", "jump down selects a marked range");
	check(g.v().send(SCI_GETSELECTIONSTART) == 12, "jumps to the next");
	w.run(Cmd::JumpStyleUp, 1);
	check(g.v().send(SCI_GETSELECTIONSTART) == 0, "jumps back");
	w.run(Cmd::JumpStyleUp, 1);
	check(g.v().send(SCI_GETSELECTIONSTART) > 12, "jump up wraps");
	w.setSelection(4, 7);
	w.run(Cmd::StyleOneToken, 2);
	check(g.v().send(SCI_INDICATORVALUEAT, 24, 4) != 0, "style one token");
	w.run(Cmd::CopyStyled, 2);
	checkEq(g.host.clipboard, "bar\n", "copy styled text");
	w.run(Cmd::ClearStyle, 2);
	check(g.v().send(SCI_INDICATORVALUEAT, 24, 4) == 0, "clear one style");
	check(g.v().send(SCI_INDICATORVALUEAT, indicator, 0) != 0, "clearing one leaves the others");
	w.run(Cmd::ClearStyle, 0);
	check(g.v().send(SCI_INDICATORVALUEAT, indicator, 0) == 0, "clear all styles");
	g.host.flashed.clear();
	w.run(Cmd::JumpStyleDown, 1);
	checkEq(g.host.flashed, "Nothing marked with that style", "nothing to jump to");
	w.run(Cmd::CloseAll);
}

static const char *kTestLanguage = R"(<?xml version="1.0" encoding="UTF-8" ?>
<NotepadPlus>
    <UserLang name="Zed" ext="zed zdl" udlVersion="2.1">
        <Settings>
            <Global caseIgnored="yes" allowFoldOfComments="no" foldCompact="no" forcePureLC="0" decimalSeparator="0" />
            <Prefix Keywords1="no" Keywords2="yes" Keywords3="no" Keywords4="no" Keywords5="no" Keywords6="no" Keywords7="no" Keywords8="no" />
        </Settings>
        <KeywordLists>
            <Keywords name="Comments">00// 01 02((EOL)) 03/* 04*/</Keywords>
            <Keywords name="Numbers, prefix1">0x</Keywords>
            <Keywords name="Operators1">+ - =</Keywords>
            <Keywords name="Folders in code1, open">begin</Keywords>
            <Keywords name="Folders in code1, close">finish</Keywords>
            <Keywords name="Keywords1">alpha &quot;else if&quot;</Keywords>
            <Keywords name="Keywords2">pre</Keywords>
            <Keywords name="Delimiters">00&quot; 01\ 02&quot; 03 04 05 06 07 08 09 10 11 12 13 14 15 16 17 18 19 20 21 22 23</Keywords>
        </KeywordLists>
        <Styles>
            <WordsStyle name="DEFAULT" fgColor="111111" bgColor="FFFFFF" colorStyle="0" fontName="" fontStyle="0" nesting="0" />
            <WordsStyle name="COMMENTS" fgColor="00FF00" bgColor="FFFFFF" colorStyle="1" fontName="" fontStyle="2" nesting="0" />
            <WordsStyle name="KEYWORDS1" fgColor="FF0000" bgColor="000000" colorStyle="3" fontName="" fontStyle="1" nesting="0" />
            <WordsStyle name="DELIMITERS1" fgColor="0000FF" bgColor="FFFFFF" colorStyle="1" fontName="" fontStyle="0" nesting="1024" />
        </Styles>
    </UserLang>
</NotepadPlus>
)";

static void testUserLanguageFormat() {
	const std::vector<UserLanguage> parsed = parseUserLanguages(kTestLanguage);
	check(parsed.size() == 1, "the language is read");
	if (parsed.size() != 1) return;
	const UserLanguage &zed = parsed[0];
	check(zed.name == "Zed" && zed.extensions == "zed zdl", "name and extensions");
	check(zed.caseIgnored && !zed.foldCompact, "global settings");
	check(!zed.prefix[0] && zed.prefix[1], "keyword prefix settings");
	check(zed.keywords[SCE_USER_KWLIST_KEYWORDS1] == "alpha \"else if\"", "a keyword list");
	check(zed.styles[SCE_USER_STYLE_KEYWORD1].fg == "FF0000" && zed.styles[SCE_USER_STYLE_KEYWORD1].colorStyle == 3, "a style's colours");
	check(zed.styles[SCE_USER_STYLE_DELIMITER1].nesting == 1024, "a style's nesting");

	// Groups, including an alternative with a space in it.
	const std::vector<std::string> groups = userGroups("00# 00// 01 02((EOL)) 03/* 05((EOL `)) 05~~~", 6);
	check(groups[0] == "# //" && groups[1].empty() && groups[2] == "((EOL))" && groups[3] == "/*", "groups are split");
	check(groups[5] == "((EOL `)) ~~~", "a group can have spaces inside double parentheses");
	check(userGroupTokens(groups[5]).size() == 2, "double parentheses count as one alternative");
	check(encodeUserGroups(groups) == "00# 00// 01 02((EOL)) 03/* 04 05((EOL `)) 05~~~", "groups are written back");

	std::string line, open, close;
	userCommentTokens(zed, line, open, close);
	check(line == "//" && open == "/*" && close == "*/", "comment markers for the comment commands");

	check(lexerKeywords("one \"else if\" 'a b'") == "one else\vif a\bb", "quoted phrases get the lexer's markers");
	check(lexerKeywords("a\\\"b") == "a\"b", "an escaped quote stays");

	check(userLanguageFileName("My Lang/../x") == "My_Lang____x.udl.xml", "a file name can't leave the folder");
	check(userLanguageFileName("") == "language.udl.xml", "an empty name still gets a file name");

	// Writing and reading again keeps everything.
	const std::vector<UserLanguage> again = parseUserLanguages(writeUserLanguages(parsed));
	check(again.size() == 1, "the written language reads back");
	if (again.size() == 1) {
		bool same = again[0].name == zed.name && again[0].extensions == zed.extensions && again[0].caseIgnored == zed.caseIgnored;
		for (int i = 0; i < kUserKeywordLists; i++) same = same && again[0].keywords[i] == zed.keywords[i];
		for (int i = 0; i < kUserStyles; i++) {
			const UserStyle &a = again[0].styles[i], &b = zed.styles[i];
			same = same && a.fg == b.fg && a.bg == b.bg && a.colorStyle == b.colorStyle && a.fontStyle == b.fontStyle && a.nesting == b.nesting;
		}
		check(same, "writing loses nothing");
	}

	// The form doesn't change anything unless it's changed.
	UserLanguage edited = zed;
	std::vector<FormField> fields = userLanguageFields(edited);
	check(applyUserLanguageFields(fields, edited), "the form applies");
	bool formSame = edited.name == zed.name;
	for (int i = 0; i < kUserKeywordLists; i++) {
		if (i == SCE_USER_KWLIST_COMMENTS || i == SCE_USER_KWLIST_DELIMITERS) continue;
		formSame = formSame && edited.keywords[i] == zed.keywords[i];
	}
	for (int i = 0; i < kUserStyles; i++) {
		const UserStyle &a = edited.styles[i], &b = zed.styles[i];
		formSame = formSame && a.fg == b.fg && a.bg == b.bg && a.colorStyle == b.colorStyle && a.fontStyle == b.fontStyle;
	}
	check(formSame, "an untouched form keeps keywords and styles");
	check(edited.keywords[SCE_USER_KWLIST_COMMENTS] == zed.keywords[SCE_USER_KWLIST_COMMENTS], "an untouched form keeps the comments");
	check(edited.keywords[SCE_USER_KWLIST_DELIMITERS] == zed.keywords[SCE_USER_KWLIST_DELIMITERS], "an untouched form keeps the delimiters");
	check(edited.styles[SCE_USER_STYLE_DELIMITER1].nesting == 1024, "an untouched form keeps nesting");
	// Turning a colour off keeps it stored.
	for (FormField &field : fields) {
		if (field.key == "style" + std::to_string(SCE_USER_STYLE_KEYWORD1)) field.value = ",000000,5";
		if (field.key == "name") field.value = "";
	}
	check(!applyUserLanguageFields(fields, edited), "a language needs a name");
	check(edited.styles[SCE_USER_STYLE_KEYWORD1].colorStyle == 2 && edited.styles[SCE_USER_STYLE_KEYWORD1].fg == "FF0000", "a colour turned off stays stored");
	check(edited.styles[SCE_USER_STYLE_KEYWORD1].fontStyle == 5, "font style bits apply");

	// The files that come with omapad, which also show the real format.
	const std::vector<UserLanguage> markdown = loadUserLanguageFile(std::string(OMAPAD_RESOURCES) + "/userDefineLangs/markdown._preinstalled.udl.xml");
	check(markdown.size() == 1 && markdown[0].name == "Markdown (preinstalled)", "the shipped language is read");
	if (markdown.size() == 1) {
		check(userGroups(markdown[0].keywords[SCE_USER_KWLIST_DELIMITERS], 24)[5] == "``` ((EOL `)) ~~~", "the shipped delimiters are understood");
		UserLanguage copy = markdown[0];
		std::vector<FormField> form = userLanguageFields(copy);
		applyUserLanguageFields(form, copy);
		check(copy.keywords[SCE_USER_KWLIST_DELIMITERS] == markdown[0].keywords[SCE_USER_KWLIST_DELIMITERS], "the shipped delimiters survive the form");
		check(copy.keywords[SCE_USER_KWLIST_COMMENTS] == markdown[0].keywords[SCE_USER_KWLIST_COMMENTS], "the shipped comments survive the form");
	}

	// Formats from before 2.0.
	const std::vector<UserLanguage> old = parseUserLanguages(R"(<NotepadPlus><UserLang name="Old" ext="old">
		<Settings><Global caseIgnored="no" /><TreatAsSymbol comment="no" /><Prefix words1="yes" words2="no" words3="no" words4="no" /></Settings>
		<KeywordLists>
			<Keywords name="Delimiters">"'0"'0</Keywords>
			<Keywords name="Folder+">begin</Keywords><Keywords name="Words1">one two</Keywords>
			<Keywords name="Comment">0# 1/* 2*/</Keywords>
		</KeywordLists>
		<Styles><WordsStyle name="KEYWORD1" fgColor="123456" bgColor="FFFFFF" /></Styles>
	</UserLang></NotepadPlus>)");
	check(old.size() == 1, "an old language is read");
	if (old.size() == 1) {
		check(old[0].prefix[0], "old prefix settings");
		check(old[0].keywords[SCE_USER_KWLIST_KEYWORDS1] == "one two", "old keyword names");
		check(old[0].keywords[SCE_USER_KWLIST_FOLDERS_IN_CODE1_OPEN] == "begin", "old folding names");
		const std::vector<std::string> oldComments = userGroups(old[0].keywords[SCE_USER_KWLIST_COMMENTS], 5);
		check(oldComments[0] == "#" && oldComments[3] == "/*" && oldComments[4] == "*/", "old comments convert");
		const std::vector<std::string> oldDelimiters = userGroups(old[0].keywords[SCE_USER_KWLIST_DELIMITERS], 24);
		check(oldDelimiters[0] == "\"" && oldDelimiters[2] == "\"" && oldDelimiters[3] == "'" && oldDelimiters[5] == "'", "old delimiters convert");
		check(old[0].styles[SCE_USER_STYLE_KEYWORD1].fg == "123456" && old[0].styles[SCE_USER_STYLE_KEYWORD1].colorStyle == 3, "old styles, whose colours count as on");
	}
	check(parseUserLanguages("not xml").empty(), "a bad file has no languages");
}

static void testUserLanguageCommands(const std::string &tmp, const std::string &support) {
	const std::string definition = writeFile(tmp, "zed.udl.xml", kTestLanguage);
	const std::string source = "alpha other // note\n\"str alpha\" 0x1F\nbegin\n  prefixed\nfinish\nALPHA\n";
	{
		Fixture f(support);
		Workspace &e = f.w;
		// Importing.
		f.host.filesToOpen = {definition};
		const int menus = f.host.menusChangedCount;
		e.run(Cmd::ImportLanguage);
		check(f.host.flashed == "Imported 1 language", "importing says what it did");
		check(f.host.menusChangedCount > menus, "the menus change after an import");
		std::vector<MenuItem> menu = e.mainMenu();
		const MenuItem *languages = nullptr;
		for (const MenuItem &top : menu)
			if (top.title == "Language") languages = &top;
		bool listed = false;
		if (languages)
			for (const MenuItem &entry : languages->children) listed = listed || entry.text == "udl:Zed";
		check(listed, "the language is in the Language menu");

		// Files with its extension use it, and it colours them.
		const std::string path = writeFile(tmp, "code.ZED", source);
		check(e.openPath(path), "open a file in the language");
		check(e.activeLanguageName() == "udl:Zed", "the extension picks the language, whatever its case");
		SciView &v = f.v();
		v.send(SCI_COLOURISE, 0, -1);
		const auto styleOf = [&](const std::string &needle, int from = 0) {
			const size_t at = source.find(needle, from);
			return static_cast<int>(v.send(SCI_GETSTYLEAT, at));
		};
		check(styleOf("alpha") == SCE_USER_STYLE_KEYWORD1, "a keyword gets its style");
		check(styleOf("other") == SCE_USER_STYLE_DEFAULT, "other text stays default");
		check(styleOf("// note") == SCE_USER_STYLE_COMMENTLINE, "a line comment gets its style");
		check(styleOf("\"str") == SCE_USER_STYLE_DELIMITER1, "a delimiter gets its style");
		check(styleOf("0x1F") == SCE_USER_STYLE_NUMBER, "a prefixed number gets its style");
		check(styleOf("ALPHA") == SCE_USER_STYLE_KEYWORD1, "ignoring case works");
		check(styleOf("begin") == SCE_USER_STYLE_FOLDER_IN_CODE1, "a fold word gets its style");
		check(styleOf("prefixed") == SCE_USER_STYLE_KEYWORD2, "a prefix keyword matches the start of a word");
		// Nesting: keyword 1 inside delimiter 1.
		check(static_cast<int>(v.send(SCI_GETSTYLEAT, source.find("alpha", source.find("str")))) == SCE_USER_STYLE_KEYWORD1, "nesting lets a keyword show in a string");
		check((v.send(SCI_GETFOLDLEVEL, 2) & SC_FOLDLEVELHEADERFLAG) != 0, "fold words make a fold");
		// Whatever comes before it, the fold is on the line with the word.
		for (const char *before : {"x\n", "x // c\n", "// c\n\n", "x // a\n// b\n", "\"a\"\n", "0x1F\n", "/* a\nb */\n", "x // c\r\n", "x\r\n"}) {
			const std::string sample = std::string(before) + "begin\ny\nfinish\nz\n";
			v.send(SCI_SETTEXT, 0, reinterpret_cast<intptr_t>(sample.c_str()));
			v.send(SCI_COLOURISE, 0, -1);
			const intptr_t line = v.send(SCI_LINEFROMPOSITION, sample.find("begin"));
			check((v.send(SCI_GETFOLDLEVEL, line) & SC_FOLDLEVELHEADERFLAG) != 0, "the fold is on its own line", before);
			if (line > 0) check((v.send(SCI_GETFOLDLEVEL, line - 1) & SC_FOLDLEVELHEADERFLAG) == 0, "the line before isn't a fold", before);
			check((v.send(SCI_GETFOLDLEVEL, line + 3) & SC_FOLDLEVELNUMBERMASK) == (v.send(SCI_GETFOLDLEVEL, line) & SC_FOLDLEVELNUMBERMASK), "the fold closes on time", before);
		}
		v.send(SCI_SETTEXT, 0, reinterpret_cast<intptr_t>(source.c_str()));
		v.send(SCI_COLOURISE, 0, -1);
		check(v.send(SCI_STYLEGETBOLD, SCE_USER_STYLE_KEYWORD1) != 0, "styles apply bold");
		check(v.send(SCI_STYLEGETFORE, SCE_USER_STYLE_KEYWORD1) == 0x0000FF, "styles apply the colour");
		check(v.send(SCI_STYLEGETFORE, SCE_USER_STYLE_DEFAULT) != 0x111111, "a colour that's off leaves the theme's");

		// The comment commands use its markers.
		v.send(SCI_GOTOLINE, 3);
		e.run(Cmd::Comment);
		v.send(SCI_GOTOLINE, 3);
		check(v.send(SCI_POSITIONFROMLINE, 3) >= 0 && e.text().find("// ") != std::string::npos, "commenting uses the language's marker");
		e.run(Cmd::Comment);

		// Redefining it through the form changes open tabs.
		f.host.formAnswer = [&](std::vector<FormField> &form) {
			for (FormField &field : form) {
				if (field.key == "language") field.value = "Zed";
				if (field.key == "list" + std::to_string(SCE_USER_KWLIST_KEYWORDS1)) field.value = "other";
			}
			return true;
		};
		e.run(Cmd::DefineLanguage);
		v.send(SCI_COLOURISE, 0, -1);
		check(styleOf("other") == SCE_USER_STYLE_KEYWORD1, "new keywords apply to the open tab");
		check(styleOf("alpha") == SCE_USER_STYLE_DEFAULT, "old keywords stop applying");

		// Renaming keeps the tab on it, and leaves one file behind.
		f.host.formAnswer = [&](std::vector<FormField> &form) {
			for (FormField &field : form) {
				if (field.key == "language") field.value = "Zed";
				if (field.key == "name") field.value = "Zed Two";
			}
			return true;
		};
		e.run(Cmd::DefineLanguage);
		check(e.activeLanguageName() == "udl:Zed Two", "a renamed language stays on its tabs");
		check(!fs::exists(support + "/settings/userDefineLangs/Zed.udl.xml"), "renaming removes the old file");
		check(fs::exists(support + "/settings/userDefineLangs/Zed_Two.udl.xml"), "renaming writes the new file");

		// A new language needs a name, and becomes the tab's.
		f.host.formAnswer = [&](std::vector<FormField> &form) {
			for (FormField &field : form)
				if (field.key == "name") field.value = "";
			return true;
		};
		f.host.answer = 0;
		e.run(Cmd::DefineLanguage);
		check(!fs::exists(support + "/settings/userDefineLangs/language.udl.xml"), "no name, no language");
		f.host.formAnswer = [&](std::vector<FormField> &form) {
			for (FormField &field : form) {
				if (field.key == "name") field.value = "Fresh";
				if (field.key == "extensions") field.value = "fresh Fresh.conf";
			}
			return true;
		};
		e.run(Cmd::DefineLanguage);
		check(e.activeLanguageName() == "udl:Fresh", "a new language applies to the tab");
		const std::string whole = writeFile(tmp, "fresh.conf", "x");
		check(e.openPath(whole) && e.activeLanguageName() == "udl:Fresh", "a language can claim a whole file name");
		e.run(Cmd::Close);

		// Export writes a file that imports.
		f.host.savePath = tmp + "/exported.xml";
		f.host.formAnswer = [&](std::vector<FormField> &form) {
			for (FormField &field : form)
				if (field.key == "language") field.value = "Fresh";
			return true;
		};
		e.run(Cmd::ExportLanguage);
		check(f.host.savePathSuggested == "Fresh.udl.xml", "export suggests a name");
		check(loadUserLanguageFile(tmp + "/exported.xml").size() == 1, "the export reads back");
		check(e.writeSession(), "the session saves");
	}
	{
		// A new run still has them, and restores the tab's language.
		Fixture f(support);
		Workspace &e = f.w;
		check(e.restoreSession(), "the session comes back");
		bool found = false;
		for (const TabInfo &tab : e.tabs(0)) found = found || tab.title == "code.ZED";
		check(found, "the tab comes back");
		check(e.openPath(tmp + "/code.ZED") && e.activeLanguageName() == "udl:Fresh", "its language is remembered over a restart");

		// Removing puts tabs on normal text and takes the file away.
		f.host.answer = 1;
		f.host.formAnswer = [&](std::vector<FormField> &form) {
			for (FormField &field : form)
				if (field.key == "language") field.value = "Fresh";
			return true;
		};
		e.run(Cmd::RemoveLanguage);
		check(e.activeLanguageName() == "normal", "a removed language leaves normal text");
		check(!fs::exists(support + "/settings/userDefineLangs/Fresh.udl.xml"), "removing takes the file away");

		// Importing something else says so.
		f.host.answer = 0;
		f.host.filesToOpen = {writeFile(tmp, "notalang.xml", "<NotepadPlus></NotepadPlus>")};
		const std::string before = f.host.flashed;
		e.run(Cmd::ImportLanguage);
		check(f.host.flashed == before, "a file without languages imports nothing");

		// What ships can't be removed but works, in the variant for the theme.
		const std::string md = writeFile(tmp, "notes.md", "# Title\n\nSome **bold** text\n");
		check(e.openPath(md) && e.activeLanguageName() == "udl:Markdown (preinstalled)", "Markdown works out of the box");
		f.v().send(SCI_COLOURISE, 0, -1);
		check(f.v().send(SCI_GETSTYLEAT, 0) == SCE_USER_STYLE_COMMENTLINE, "a Markdown heading is coloured");
		const intptr_t lightColour = f.v().send(SCI_STYLEGETFORE, SCE_USER_STYLE_COMMENTLINE);
		e.run(Cmd::SetTheme, 0, "DarkModeDefault");
		check(f.v().send(SCI_STYLEGETFORE, SCE_USER_STYLE_COMMENTLINE) != lightColour, "a dark theme uses the dark variant");
		e.run(Cmd::SetTheme, 0, "Default");

		// Changing it makes a personal copy that wins, and removing that brings the original back.
		const auto colouredFor = [&](const char *word) {
			f.v().send(SCI_COLOURISE, 0, -1);
			return static_cast<int>(f.v().send(SCI_GETSTYLEAT, e.text().find(word)));
		};
		check(colouredFor("Some") == SCE_USER_STYLE_DEFAULT, "before overriding, the word is plain");
		MenuItem remove;
		remove.cmd = Cmd::RemoveLanguage;
		check(!e.stateOf(remove).enabled, "what ships can't be removed");
		f.host.formAnswer = [&](std::vector<FormField> &form) {
			for (FormField &field : form) {
				if (field.key == "language") field.value = "Markdown (preinstalled)";
				if (field.key == "list" + std::to_string(SCE_USER_KWLIST_KEYWORDS8)) field.value = "Some";
			}
			return true;
		};
		e.run(Cmd::DefineLanguage);
		check(e.activeLanguageName() == "udl:Markdown (preinstalled)", "the override keeps the name");
		check(colouredFor("Some") == SCE_USER_STYLE_KEYWORD8, "the override applies");
		check(e.stateOf(remove).enabled, "an override can be removed");
		f.host.answer = 1;
		f.host.formAnswer = [&](std::vector<FormField> &form) {
			for (FormField &field : form)
				if (field.key == "language") field.value = "Markdown (preinstalled)";
			return true;
		};
		e.run(Cmd::RemoveLanguage);
		check(e.activeLanguageName() == "udl:Markdown (preinstalled)", "removing an override goes back to the original");
		check(colouredFor("Some") == SCE_USER_STYLE_DEFAULT, "the original's keywords are back");
		check(!e.stateOf(remove).enabled, "nothing is left to remove");
	}
}

static void testPrinting(const std::string &tmp, const std::string &support) {
	Fixture f(support);
	Workspace &e = f.w;
	std::string many;
	for (int i = 0; i < 600; i++) many += "line number " + std::to_string(i) + "\n";
	const std::string path = writeFile(tmp, "print.txt", many);
	check(e.openPath(path), "open a long file");
	SciView &v = f.v();
	const intptr_t margin = v.send(SCI_GETMARGINWIDTHN, 0);
	const intptr_t editorColour = v.send(SCI_STYLEGETBACK, STYLE_DEFAULT);

	// While printing, the editor is left alone and a hidden view does the work.
	f.host.duringPrint = [&]() {
		check(f.host.printer->send(SCI_GETDOCPOINTER) == v.send(SCI_GETDOCPOINTER), "the print view shows the document");
		check(v.send(SCI_STYLEGETBACK, STYLE_DEFAULT) == editorColour, "the editor keeps its colours while printing");
		check(f.host.printer->send(SCI_GETMARGINWIDTHN, 0) > 0, "line numbers are on for the printout");
		check(f.host.printer->send(SCI_INDICGETSTYLE, 30) == INDIC_HIDDEN, "smart highlights don't print");
		check(f.host.printer->send(SCI_INDICGETSTYLE, 25) == v.send(SCI_INDICGETSTYLE, 25), "token styles print as they look");
	};
	e.run(Cmd::Print);
	f.host.duringPrint = nullptr;
	check(f.host.printer->send(SCI_GETDOCPOINTER) != v.send(SCI_GETDOCPOINTER), "the print view lets go of the document");
	check(f.host.printRuns == 1, "printing reaches the platform");
	check(f.host.lastPrintOutput.showDialog && f.host.lastPrintOutput.pdfPath.empty(), "Print shows the dialog");
	check(f.host.lastPrintStart == 0 && f.host.lastPrintEnd == v.send(SCI_GETLENGTH), "Print covers the document");
	check(f.host.lastPrintPageCount > 3, "a long file takes several pages");
	check(f.host.lastPrintHeader == "print.txt", "the header says which file");
	check(f.host.lastPrintFooter == "1 / " + std::to_string(f.host.lastPrintPageCount), "the footer numbers the page");
	check(v.send(SCI_GETMARGINWIDTHN, 0) == margin, "the editor's margin is unchanged");

	e.run(Cmd::PrintNow);
	check(!f.host.lastPrintOutput.showDialog, "Print Now skips the dialog");

	// A selection.
	MenuItem selection;
	selection.cmd = Cmd::PrintSelection;
	v.send(SCI_SETEMPTYSELECTION, 0);
	check(!e.stateOf(selection).enabled, "Print Selection needs a selection");
	const int runs = f.host.printRuns;
	e.run(Cmd::PrintSelection);
	check(f.host.printRuns == runs && f.host.flashed == "Nothing is selected", "an empty selection doesn't print");
	v.send(SCI_SETSEL, 10, 30);
	check(e.stateOf(selection).enabled, "Print Selection is on with a selection");
	e.run(Cmd::PrintSelection);
	check(f.host.lastPrintStart == 10 && f.host.lastPrintEnd == 30 && f.host.lastPrintPageCount == 1, "only the selection prints");
	v.send(SCI_SETEMPTYSELECTION, 0);

	// A PDF.
	f.host.savePath.clear();
	const int beforePdf = f.host.printRuns;
	e.run(Cmd::SaveAsPdf);
	check(f.host.printRuns == beforePdf, "cancelling the save doesn't print");
	check(f.host.savePathSuggested == "print.pdf", "the PDF is named after the file");
	f.host.savePath = tmp + "/out.pdf";
	e.run(Cmd::SaveAsPdf);
	check(f.host.lastPrintOutput.pdfPath == tmp + "/out.pdf" && !f.host.lastPrintOutput.showDialog, "Save as PDF asks for a file");
	if (f.host.pdfWritten) {
		std::ifstream pdf(tmp + "/out.pdf", std::ios::binary);
		std::string start(5, '\0');
		pdf.read(&start[0], 5);
		check(start == "%PDF-", "Save as PDF writes a PDF");
		// The same again from a dark theme, for looking at by hand.
		if (const char *keep = std::getenv("OMAPAD_KEEP_PDF")) {
			e.run(Cmd::SetTheme, 0, "DarkModeDefault");
			f.host.savePath = keep;
			e.run(Cmd::SaveAsPdf);
			e.run(Cmd::SetTheme, 0, "Default");
		}
	}
	f.host.savePath.clear();

	// Colours.
	e.run(Cmd::SetTheme, 0, "Default");
	e.run(Cmd::Print);
	check(f.host.lastPrintColourMode == SC_PRINT_COLOURONWHITE, "a light theme prints its colours on white");
	e.run(Cmd::SetTheme, 0, "DarkModeDefault");
	e.run(Cmd::Print);
	check(f.host.lastPrintColourMode == SC_PRINT_COLOURONWHITE, "a dark theme prints on white too");
	const intptr_t darkEditor = v.send(SCI_STYLEGETFORE, STYLE_DEFAULT);
	f.host.duringPrint = [&]() {
		check(f.host.printer->send(SCI_STYLEGETFORE, STYLE_DEFAULT) != darkEditor, "a dark theme's text isn't what's printed");
	};
	e.run(Cmd::Print);
	f.host.duringPrint = nullptr;
	check(v.send(SCI_STYLEGETFORE, STYLE_DEFAULT) == darkEditor, "the editor stays dark");
	e.run(Cmd::SetTheme, 0, "Default");

	// Preferences.
	f.host.formAnswer = [&](std::vector<FormField> &form) {
		for (FormField &field : form) {
			if (field.key == "printColours") field.value = "black";
			if (field.key == "printFooterMiddle") field.value = "Page $(CURRENT_PRINTING_PAGE)";
			if (field.key == "printHeaderLeft") field.value = "$(FULL_CURRENT_PATH)";
			if (field.key == "printLineNumbers") field.value = "0";
			if (field.key == "printMarginTop") field.value = "500";
		}
		return true;
	};
	e.run(Cmd::ShowPreferences);
	e.run(Cmd::Print);
	check(f.host.lastPrintColourMode == SC_PRINT_BLACKONWHITE, "the colour preference applies");
	check(f.host.lastPrintFooter == "Page 1", "footer preference applies");
	check(f.host.lastPrintHeader == path, "header preference applies");
	int marginShown = 0;
	f.host.formAnswer = [&](std::vector<FormField> &form) {
		for (const FormField &field : form)
			if (field.key == "printMarginTop") marginShown = std::atoi(field.value.c_str());
		return false;
	};
	e.run(Cmd::ShowPreferences);
	check(marginShown == 100, "a margin is limited");

	// A failure says so.
	f.host.printError = "No printer";
	e.run(Cmd::Print);
	check(f.host.lastTitle == "The document could not be printed.", "a failed print says so");
	f.host.printError.clear();

	// An empty document is one blank page rather than none.
	e.run(Cmd::New);
	e.run(Cmd::Print);
	check(f.host.lastPrintPageCount == 1, "an empty document prints one page");
}

static void testDocumentMapAndMisc(const std::string &tmp, const std::string &support) {
	Fixture f(support);
	Workspace &e = f.w;
	std::string many;
	for (int i = 0; i < 500; i++) many += "line " + std::to_string(i) + "\n";
	const std::string path = writeFile(tmp, "map.txt", many);
	check(e.openPath(path), "open for the map");
	e.run(Cmd::ToggleDocumentMap);
	check(f.host.mapVisible && e.documentMapVisible(), "the map shows");
	check(f.host.map->send(SCI_GETDOCPOINTER) == f.v().send(SCI_GETDOCPOINTER), "the map shows the same document");
	check(f.host.map->send(SCI_GETZOOM) == -10, "the map is zoomed out");
	check(f.host.map->send(SCI_INDICATORVALUEAT, 19, 0) != 0, "the map shades what's on screen");

	// Clicking the map moves the editor.
	const long target = f.host.map->send(SCI_POSITIONFROMLINE, 400);
	e.mapPressed(f.host.map->send(SCI_POINTXFROMPOSITION, 0, target), f.host.map->send(SCI_POINTYFROMPOSITION, 0, target));
	check(f.v().send(SCI_GETFIRSTVISIBLELINE) > 300, "a click on the map scrolls the editor", std::to_string(f.v().send(SCI_GETFIRSTVISIBLELINE)));
	const long scrolledTo = f.v().send(SCI_GETFIRSTVISIBLELINE);
	e.mapScrolled(-10);
	check(f.v().send(SCI_GETFIRSTVISIBLELINE) == scrolledTo - 10, "scrolling on the map scrolls the editor");
	check(f.v().send(SCI_LINEFROMPOSITION, f.v().send(SCI_GETCURRENTPOS)) == 0, "the caret stays put");

	// The map follows tab changes, and closing a document doesn't free it under the map.
	e.createUntitledDocument();
	e.setText("new");
	check(f.host.map->send(SCI_GETDOCPOINTER) == f.v().send(SCI_GETDOCPOINTER), "the map follows the tab");
	f.host.answer = 2;
	e.closeActive();
	check(f.host.map->send(SCI_GETDOCPOINTER) == f.v().send(SCI_GETDOCPOINTER), "the map follows a close");
	e.run(Cmd::ToggleDocumentMap);
	check(!f.host.mapVisible, "the map hides");

	// Next search result.
	const std::string a = writeFile(tmp, "result.txt", "needle one\nother\nneedle two\n");
	check(e.openPath(a), "open for results");
	FindOptions o = findOptions("needle", "", SearchMode::Normal);
	check(e.findAllInDocument(o) == 2, "find all for next result");
	e.run(Cmd::NextSearchResult);
	const long firstJump = f.v().send(SCI_LINEFROMPOSITION, f.v().send(SCI_GETCURRENTPOS));
	e.run(Cmd::NextSearchResult);
	const long secondJump = f.v().send(SCI_LINEFROMPOSITION, f.v().send(SCI_GETCURRENTPOS));
	check(firstJump != secondJump && (firstJump == 0 || firstJump == 2) && (secondJump == 0 || secondJump == 2), "next result walks them", std::to_string(firstJump) + " " + std::to_string(secondJump));
	e.run(Cmd::PreviousSearchResult);
	check(f.v().send(SCI_LINEFROMPOSITION, f.v().send(SCI_GETCURRENTPOS)) == firstJump, "previous result goes back");

	e.setText("call(one, [two])");
	e.setSelection(11, 11);
	e.run(Cmd::SelectInBraces);
	checkEq(e.selectedText(), "two", "selects inside the nearest bracket");
	e.setSelection(6, 6);
	e.run(Cmd::SelectInBraces);
	checkEq(e.selectedText(), "one, [two]", "skips finished brackets");

	e.setText("line");
	e.setSelection(0, 4);
	e.run(Cmd::DeleteSelection);
	checkEq(e.text(), "", "delete");
	e.run(Cmd::ToggleReadOnly);
	check(f.v().send(SCI_GETREADONLY) != 0, "read-only on");
	f.v().send(SCI_ADDTEXT, 1, reinterpret_cast<intptr_t>("x"));
	checkEq(e.text(), "", "read-only stops edits");
	e.run(Cmd::ToggleReadOnly);

	e.setText("1,5\n10,25\n2,75\n");
	e.setSelection(0, 0);
	e.run(Cmd::LineOperation, static_cast<int>(LineOp::SortDecimalCommaAsc));
	checkEq(e.text(), "1,5\n2,75\n10,25\n", "comma decimals sort");
	e.run(Cmd::LineOperation, static_cast<int>(LineOp::SortDecimalCommaDesc));
	checkEq(e.text(), "10,25\n2,75\n1,5\n", "comma decimals sort down");

	const std::string oldName = writeFile(tmp, "before-rename.txt", "x");
	check(e.openPath(oldName), "open for rename");
	f.host.promptAnswer = "after-rename.txt";
	f.host.promptAccepted = true;
	e.run(Cmd::Rename);
	check(e.activePath() == tmp + "/after-rename.txt" && fs::exists(tmp + "/after-rename.txt") && !fs::exists(oldName), "rename moves the file", e.activePath());

	// Close to the left and right.
	f.host.answer = 2;  // Don't Save.
	e.run(Cmd::CloseAll);
	e.createUntitledDocument();
	e.createUntitledDocument();
	const size_t before = e.tabs(e.focusedPane()).size();
	check(before >= 3, "three tabs", std::to_string(before));
	e.clickTab(e.focusedPane(), 1);
	e.run(Cmd::CloseLeft);
	check(e.tabs(e.focusedPane()).size() == before - 1, "close to the left");
	e.clickTab(e.focusedPane(), 0);
	e.run(Cmd::CloseRight);
	check(e.tabs(e.focusedPane()).size() == 1, "close to the right");
	e.run(Cmd::CloseAll);
}

int main() {
	setvbuf(stdout, nullptr, _IONBF, 0);
	initTestToolkit();
	setenv("OMAPAD_OMARCHY_THEME", "/nonexistent/colors.toml", 1);
	std::string tmp = (fs::temp_directory_path() / ("omapad-tests-" + std::to_string(getpid()))).string();
	fs::create_directories(tmp);
	const std::string support = tmp + "/support";

	printf("== testEncodings\n");
	testEncodings();
	printf("== testEol\n");
	testEol();
	printf("== testLineOps\n");
	testLineOps();
	printf("== testCase\n");
	testCase();
	printf("== testExtended\n");
	testExtended();
	printf("== testComments\n");
	testComments();
	printf("== testHash\n");
	testHash();
	printf("== testEditorFiles\n");
	testEditorFiles(tmp, support);
	printf("== testEditorSearch\n");
	testEditorSearch(tmp, support);
	printf("== testEncodingAndExternal\n");
	testEncodingAndExternal(tmp, support);
	printf("== testSplitView\n");
	testSplitView(tmp, support);
	printf("== testFolders\n");
	testFolders(tmp, support);
	printf("== testSession\n");
	testSession(tmp);
	printf("== testOmarchyTheme\n");
	testOmarchyTheme(tmp, support);
	printf("== testHighlighting\n");
	testHighlighting(tmp, support);

	printf("== testRegexEngine\n");
	testRegexEngine();
	printf("== testRegexInDocument\n");
	testRegexInDocument(support);
	printf("== testPreferences\n");
	testPreferences(tmp);
	printf("== testPanelsAndBackupInterval\n");
	testPanelsAndBackupInterval(tmp);
	printf("== testShortcuts\n");
	testShortcuts(tmp);
	printf("== testCompletion\n");
	testCompletion(tmp, support);
	printf("== testFunctionList\n");
	testFunctionList(tmp, support);
	printf("== testMultiSelectAndColumns\n");
	testMultiSelectAndColumns(support);
	printf("== testChangeHistoryAndStyles\n");
	testChangeHistoryAndStyles(tmp, support);
	printf("== testDocumentMapAndMisc\n");
	testDocumentMapAndMisc(tmp, support);
	printf("== testUserLanguageFormat\n");
	testUserLanguageFormat();
	printf("== testUserLanguageCommands\n");
	testUserLanguageCommands(tmp, support + "-udl");
	printf("== testPrinting\n");
	testPrinting(tmp, support + "-print");

	std::error_code error;
	chmod((tmp + "/locked").c_str(), 0700);
	fs::remove_all(tmp, error);
	printf("%d checks, %d failed\n", gChecks, gFailures);
	return gFailures ? 1 : 0;
}
