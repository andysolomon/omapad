#include <cstring>
// Tests pure logic directly, then drives the real Workspace with real Scintilla views,
// against real files in a temporary folder. Runs the same on macOS and Linux.
#include "TestViews.h"
#include "Scintilla.h"
#include "TextCore.h"
#include "Workspace.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
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
	std::unique_ptr<SciView> panes[2], results, background;
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

	TestHost() {
		for (int i = 0; i < 2; i++)
			panes[i] = makeHiddenSciView([this, i](const SCNotification &n) { if (workspace) workspace->editorNotification(i, n); });
		results = makeHiddenSciView([this](const SCNotification &n) { if (workspace) workspace->resultsNotification(n); });
		background = makeHiddenSciView(nullptr);
	}
	SciView &paneView(int pane) override { return *panes[pane]; }
	SciView &resultsView() override { return *results; }
	SciView &backgroundView() override { return *background; }
	int ask(const std::string &title, const std::string &, const std::vector<std::string> &) override {
		prompts++;
		lastTitle = title;
		return answer;
	}
	bool chooseFilesToOpen(const std::string &, std::vector<std::string> &) override { return false; }
	bool chooseSavePath(const std::string &, const std::string &, std::string &) override { return false; }
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

	std::error_code error;
	chmod((tmp + "/locked").c_str(), 0700);
	fs::remove_all(tmp, error);
	printf("%d checks, %d failed\n", gChecks, gFailures);
	return gFailures ? 1 : 0;
}
