// Everything omapad does that isn't drawing a widget: documents, split view, folders,
// search, sessions and commands. Each platform supplies a Host and draws what this says.
#pragma once
#include "Languages.h"
#include "TextCore.h"

#include <atomic>
#include <functional>
#include <memory>
#include <pthread.h>
#include <string>
#include <vector>

struct SCNotification;

namespace omapad {

enum class SearchMode { Normal = 0, Extended = 1, Regex = 2 };
enum class SearchResult { Found, NotFound, InvalidPattern };
enum class FindTab { Find = 0, Replace = 1, Files = 2 };

struct FindOptions {
	std::string what, with;
	bool matchCase = false, wholeWord = false, wrap = true, backward = false, inSelection = false;
	SearchMode mode = SearchMode::Normal;
	std::string directory, filters = "*";
	bool recursive = true, includeHidden = false;
};

enum class Cmd {
	None,
	New, Open, OpenRecent, ClearRecent, OpenFolder, Reload, Save, SaveAs, SaveCopy, SaveAll,
	Close, CloseAll, CloseOthers, CloseUnchanged, Reveal, Trash, Quit,
	Undo, Redo, Cut, Copy, Paste, SelectAll, InsertDate, CopyPath, Case, LineOperation,
	DuplicateLine, JoinLines, SplitLines, MoveLineUp, MoveLineDown, InsertBlankAbove, InsertBlankBelow,
	Comment, BlockComment, EolConvert, EolToSpace, TabsToSpaces, SpacesToTabs,
	ShowFind, ShowReplace, ShowFindInFiles, FindNext, FindPrevious, SelectFindNext, SelectFindPrevious,
	ToggleResults, GoToLine, MatchingBrace,
	ToggleBookmark, NextBookmark, PreviousBookmark, ClearBookmarks, CopyBookmarked,
	RemoveBookmarked, RemoveUnbookmarked, InvertBookmarks,
	AlwaysOnTop, FullScreen, ToggleSpace, ToggleEol, ToggleAllCharacters, ToggleGuides, ToggleWrapSymbol, ToggleWrap,
	ZoomIn, ZoomOut, ZoomReset, ActivateTab, NextTab, PreviousTab, MoveTabForward, MoveTabBackward,
	MoveToOtherView, CloneToOtherView, FocusOtherView, RotateSplit, SyncVertical, SyncHorizontal,
	ToggleFolderPanel, ToggleHiddenInFolders, RefreshFolders, CloseAllFolders,
	FolderNewFile, FolderNewFolder, FolderRename, FolderTrash, FolderCopyPath, FolderReveal, FolderFindInFiles, FolderRemoveRoot,
	FoldAll, UnfoldAll, FoldCurrent, UnfoldCurrent, FoldLevel, UnfoldLevel, Summary,
	ReopenEncoding, ConvertEncoding, SetLanguage, SetTheme, ChooseFont, SetTabWidth, ToggleInsertSpaces,
	HashSelection, HashFiles, MacroStart, MacroStop, MacroPlay, MacroPlayToEnd, About, SelectDocument,
	TabClose, TabCloseOthers, TabCloseLeft, TabCloseRight, TabReveal, TabCopyPath, TabMoveToOther, TabCloneToOther,
};

// A menu described once and drawn natively by each platform.
struct MenuItem {
	enum class Dynamic { None, Recent, Documents };
	std::string title;
	Cmd cmd = Cmd::None;
	int arg = 0;
	std::string text;      // A string argument, such as a language name or path.
	std::string shortcut;  // "Mod+Shift+F". Mod is Command on a Mac and Control elsewhere.
	std::string macAction; // An AppKit responder action, for standard text editing.
	bool separator = false;
	Dynamic dynamic = Dynamic::None;
	std::vector<MenuItem> children;
};

struct MenuState { bool enabled = true; bool checked = false; };

struct TabInfo { std::string title, toolTip; bool dirty = false; };

struct FolderEntry { std::string name, path; bool isDirectory = false; bool isSymlink = false; };

// Colours for the window around the editor, on platforms that draw their own.
struct ChromeColors {
	bool valid = false;
	std::string background, foreground, dim, accent, selection, darkBackground, lighterBackground;
};

// What each platform must provide.
struct Host {
	virtual ~Host() = default;
	virtual SciView &paneView(int pane) = 0;
	virtual SciView &resultsView() = 0;
	virtual SciView &backgroundView() = 0;

	virtual int ask(const std::string &title, const std::string &detail, const std::vector<std::string> &buttons) = 0;
	virtual bool chooseFilesToOpen(const std::string &startDirectory, std::vector<std::string> &out) = 0;
	virtual bool chooseSavePath(const std::string &name, const std::string &startDirectory, std::string &out) = 0;
	virtual bool chooseFolder(const std::string &startDirectory, std::string &out) = 0;
	virtual bool chooseFont(std::string &family, double &size) = 0;
	virtual bool promptText(const std::string &title, const std::string &label, const std::string &initial, std::string &out) = 0;
	virtual void setClipboard(const std::string &text) = 0;
	virtual void reveal(const std::string &path) = 0;
	virtual bool moveToTrash(const std::string &path, std::string &error) = 0;

	virtual void tabsChanged() = 0;   // Tabs, focus or window title changed.
	virtual void statusChanged(const std::string &left, const std::string &right) = 0;
	virtual void flash(const std::string &message) = 0;
	virtual void splitChanged(bool visible, bool stacked) = 0;
	virtual void setResultsVisible(bool visible) = 0;
	virtual void setFolderPanelVisible(bool visible) = 0;
	virtual void foldersChanged() = 0;
	virtual void focusPane(int pane) = 0;
	virtual void themeChanged() = 0;

	virtual void showFind(FindTab tab, const std::string &prefill, const std::string &directory) = 0;
	virtual void setFindText(const std::string &text) = 0;
	virtual FindOptions currentFindOptions() = 0;
	virtual void setFindStatus(const std::string &status, bool isError) = 0;
	virtual void setFindInFilesRunning(bool running) = 0;

	virtual void setFullScreen(bool on) = 0;
	virtual void setAlwaysOnTop(bool on) = 0;
	virtual bool systemIsDark() = 0;
	virtual void post(std::function<void()> work) = 0;  // Runs on the UI thread later.
	virtual void quit() = 0;
};

struct Settings {
	std::string theme = "Auto";
#ifdef __APPLE__
	std::string fontName = "Menlo";
	double fontSize = 13;
#else
	std::string fontName = "monospace";
	double fontSize = 11;
#endif
	bool wordWrap = false, showSpace = false, showEol = false, showGuides = true, showWrapSymbol = false;
	int tabWidth = 4;
	bool tabsToSpaces = false;
	bool showHiddenInFolders = false;
	std::vector<std::string> recent;
};

struct Document;
struct Tab;
struct Pane;

class Workspace {
public:
	// `settingsDirectory` holds preferences. `stateDirectory` holds the session and backups.
	Workspace(Host &host, const std::string &resourceDirectory, const std::string &settingsDirectory, const std::string &stateDirectory);
	~Workspace();

	// Call once the host's views exist.
	void start();

	// Commands, as menus and shortcuts invoke them.
	void run(Cmd cmd, int arg = 0, const std::string &text = std::string());
	MenuState stateOf(const MenuItem &item);
	std::vector<MenuItem> mainMenu() const;
	std::vector<MenuItem> dynamicMenu(MenuItem::Dynamic kind) const;
	std::vector<MenuItem> tabMenu(int pane, int index) const;
	std::vector<MenuItem> folderMenu(const std::string &path, bool isDirectory, bool isRoot) const;
	void runFolderCommand(Cmd cmd, const std::string &path);

	// Notifications from the widgets.
	void editorNotification(int pane, const SCNotification &notification);
	void resultsNotification(const SCNotification &notification);
	void paneFocused(int pane);
	void applicationActivated();  // The app came to the front: check files and themes.
	bool tick();                  // Call every few seconds. Writes backups.
	bool prepareToQuit();         // False means don't quit.
	void openFiles(const std::vector<std::string> &paths);
	std::string windowTitle() const;
	std::string windowPath() const;
	bool windowDirty() const;

	// Tabs.
	int paneCount() const { return 2; }
	int focusedPane() const { return focus; }
	bool splitVisible() const;
	bool splitStacked() const { return stacked; }
	std::vector<TabInfo> tabs(int pane) const;
	int activeTab(int pane) const;
	void clickTab(int pane, int index);
	bool closeTab(int pane, int index);
	void moveTab(int pane, int from, int to);
	void createUntitledDocument();
	bool moveTabToOtherView(int pane, int index);
	bool cloneTabToOtherView(int pane, int index);

	// Documents, on the focused pane.
	bool openPath(const std::string &path);
	bool saveActiveToPath(const std::string &path);
	bool saveActive();
	bool closeActive();
	int documentCount() const;
	std::string activeTitle() const;
	std::string activePath() const;
	std::string activeEncodingIdentifier() const;
	std::string activeLanguageName() const;
	bool activeIsDirty() const;
	std::string activeEolName() const;
	std::string text() const;
	void setText(const std::string &text);
	void setSelection(long start, long end);
	std::string selectedText() const;

	// Search.
	SearchResult findNext(const FindOptions &options);
	long countMatches(const FindOptions &options);  // -1 for an invalid pattern.
	long replaceAll(const FindOptions &options);    // -1 for an invalid pattern.
	SearchResult replaceOne(const FindOptions &options);
	long replaceAllOpenDocuments(const FindOptions &options);
	long markAll(const FindOptions &options);
	void clearMarks();
	long findAllInDocument(const FindOptions &options);
	void findInFiles(const FindOptions &options, std::function<void(long matches, long files)> completion);
	void stopFindInFiles();
	std::string resultsText() const;
	bool jumpToResultLine(long line);

	// The Find panel's buttons.
	void findPanelFindNext();
	void findPanelCount();
	void findPanelFindAll();
	void findPanelReplace();
	void findPanelReplaceAll();
	void findPanelReplaceAllOpen();
	void findPanelMarkAll();
	void findPanelClearMarks();
	void findPanelFindInFiles();

	bool convertActiveToEncoding(const std::string &identifier);  // False if the text can't be represented.
	bool reinterpretActiveAsEncoding(const std::string &identifier);
	void setActiveLanguage(const std::string &name);
	void checkExternalChanges();
	bool writeSession();  // False if anything could not be written.
	bool restoreSession();

	// Folder as Workspace.
	const std::vector<std::string> &folders() const { return folderRoots; }
	bool addFolder(const std::string &path);
	void removeFolder(const std::string &path);
	std::vector<FolderEntry> listFolder(const std::string &path) const;
	bool folderPanelVisible() const { return folderPanel; }
	void setFolderPanelVisible(bool visible);
	bool createFileIn(const std::string &directory, const std::string &name, std::string &error);
	bool createFolderIn(const std::string &directory, const std::string &name, std::string &error);
	bool renamePath(const std::string &path, const std::string &newName, std::string &error);
	bool isFolderExpanded(const std::string &path) const;
	void setFolderExpanded(const std::string &path, bool expanded);

	// Themes.
	std::string effectiveTheme() const;
	bool themeIsDark() const;
	ChromeColors chromeColors() const;
	bool usesAutomaticTheme() const { return settings.theme == "Auto"; }
	bool reloadOmarchyTheme();  // True if the colours changed.
	void systemAppearanceChanged();
	std::string omarchyThemePath() const;

	Settings settings;

private:
	friend struct BackgroundDocument;
	Host &host;
	SciView &view() const;
	SciView &view(int pane) const;
	intptr_t s(unsigned message, uintptr_t w = 0, intptr_t l = 0) const;
	Pane &paneAt(int pane) const;
	Tab *activeTabPointer() const;
	Document *activeDocument() const;

	std::shared_ptr<Document> addDocument();
	std::string nextUntitledName() const;
	void configureView(int pane);
	void applyViewPreferences(int pane);
	void applyLanguageTo(int pane);
	void applyThemeEverywhere();
	void updateLineNumberMargin(int pane);
	void saveViewState(int pane);
	void activate(int pane, int index);
	void addTab(int pane, std::shared_ptr<Document> document);
	void detachPane(int pane);
	void releaseIfUnused(const std::shared_ptr<Document> &document);
	bool documentOpenElsewhere(const Document *document, int pane, int index) const;
	void findOpen(const std::string &path, int &pane, int &index) const;
	void refreshChrome();
	void updateStatus();
	void loadInto(Document &document, const std::string &utf8);
	std::string textOf(const Document &document) const;
	bool readFile(const std::string &path, std::string &out, bool report);
	bool writeFileAtomically(const std::string &bytes, const std::string &path, std::string &error);
	bool saveDocument(Document &document, const std::string &path);
	bool saveAsDialog(Document &document);
	const Language *detectLanguage(const std::string &path, const std::string &utf8) const;
	bool reloadActive();
	void addRecent(const std::string &path);
	void removeBackup(Document &document);
	std::string backupPath(const Document &document) const;
	void closeTabs(std::vector<std::pair<int, std::shared_ptr<Document>>> targets);
	void loadSettings();
	void saveSettings() const;
	void afterSplitChange();

	void autoIndent(int pane, int character);
	void updateBraceHighlight(int pane);
	void updateSmartHighlight(int pane);
	void syncScroll(int pane);

	std::string patternFrom(const FindOptions &options) const;
	int flagsFrom(const FindOptions &options) const;
	long searchIn(SciView &target, const std::string &pattern, long from, long to) const;
	long replaceTarget(SciView &target, const FindOptions &options) const;
	long replaceAllIn(SciView &target, const FindOptions &options) const;
	void searchRange(const FindOptions &options, SciView &target, long &start, long &end) const;
	void setResults(const std::string &text, std::vector<struct ResultEntry> entries, bool append);
	void showResults(bool visible);

	void selectedLines(long &start, long &end, bool wholeDocumentIfEmpty) const;
	std::string rangeOf(long start, long end) const;
	void replaceRange(long start, long end, const std::string &text, bool select);
	void lineOperation(LineOp op);
	void comment(CommentMode mode);
	void blockComment();
	void toggleBookmark(long line);
	long currentLine() const;
	void goToBookmark(bool forward);
	void removeLines(bool keepBookmarked);
	void foldLevel(int level, bool expand);
	void summary();
	void goToLine();
	void hashFiles(HashKind kind);
	void hashSelection(HashKind kind);
	void macroRecord(unsigned message, uintptr_t w, intptr_t l);
	void macroPlayOnce();
	void runFromMenu(bool backward, bool useSelection);
	std::string selectionOrWord() const;

	LanguageCatalog catalog;
	std::string settingsDir, stateDir;
	std::vector<std::shared_ptr<Document>> documents;
	std::unique_ptr<Pane> panes[2];
	int focus = 0;
	bool stacked = false;
	bool syncVertical = false, syncHorizontal = false;
	bool syncing = false;
	long syncLine = 0, syncColumn = 0;
	bool resultsVisible = false;
	bool checkingExternal = false;
	bool started = false;
	std::vector<std::string> folderRoots;
	std::vector<std::string> expandedFolders;
	bool folderPanel = false;
	bool fullScreen = false, alwaysOnTop = false;
	std::string lastSession;
	std::string lastPaletteText;
	Palette palette;
	std::vector<struct ResultEntry> resultEntries;
	bool recording = false;
	struct MacroStep { unsigned message; uintptr_t w; intptr_t l; std::string text; };
	std::vector<MacroStep> macro;

	// Find in Files runs on its own thread with a large stack, because the C++ regex
	// library recurses per character and an ordinary stack can't hold a long line.
	std::shared_ptr<std::atomic_bool> searchCancel;
	pthread_t searchThread{};
	bool searchThreadRunning = false;
	long searchGeneration = 0;
	std::shared_ptr<bool> alive;
	void joinSearchThread();
};

}  // namespace omapad
