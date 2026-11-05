// Everything omapad does that isn't drawing a widget: documents, split view, folders,
// search, sessions and commands. Each platform supplies a Host and draws what this says.
#pragma once
#include "Completion.h"
#include "Notes.h"
#include "FunctionList.h"
#include "Languages.h"
#include "Print.h"
#include "Regex.h"
#include "TextCore.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <pthread.h>
#include <string>
#include <vector>

struct SCNotification;
struct OmapadPluginInfo;

namespace omapad {

enum class SearchMode { Normal = 0, Extended = 1, Regex = 2 };
enum class SearchResult { Found, NotFound, InvalidPattern };
enum class FindTab { Find = 0, Replace = 1, Files = 2, Mark = 3 };

struct FindOptions {
	std::string what, with;
	bool matchCase = false, wholeWord = false, wrap = true, backward = false, inSelection = false;
	bool dotMatchesNewline = false;
	SearchMode mode = SearchMode::Normal;
	std::string directory, filters = "*";
	bool recursive = true, includeHidden = false;
	std::vector<std::string> files;  // If set, search these rather than a folder.
	std::map<std::string, std::string> openDocuments;  // Text to use instead of what's on disk.
	bool bookmarkLine = false, purge = false;  // For Mark.
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
	ShowPreferences, ShowShortcutMapper,
	CompleteFunction, CompleteWord, ParameterHint, PreviousHint, NextHint, CompletePath,
	MultiSelectAll, MultiSelectNext, MultiSelectUndo, MultiSelectSkip, ColumnEditor,
	ToggleFunctionList, RefreshFunctionList, ToggleDocumentMap,
	NextChange, PreviousChange, ClearChangeHistory,
	StyleAllOccurrences, StyleOneToken, ClearStyle, JumpStyleUp, JumpStyleDown, CopyStyled,
	NextSearchResult, PreviousSearchResult, SelectInBraces, IndentLine, UnindentLine, DeleteSelection,
	ToggleReadOnly, Rename, CloseLeft, CloseRight,
	Print, PrintNow, PrintSelection, SaveAsPdf,
	DefineLanguage, ImportLanguage, ExportLanguage, RemoveLanguage, OpenLanguageFolder,
	OpenTerminal, OpenFolderOfFile, OpenDefaultViewer, CloseUnpinned, TabPin, PinActive, LoadSession, SaveSession,
	BeginEndSelect, CopyAllNames, BlockUncomment, TrimEolToSpace, PasteHtml, PasteRtf, CopyBinary, CutBinary, PasteBinary,
	OpenSelection, OpenSelectionFolder, RedactSelection, SearchInternet, ChangeSearchEngine, ColumnModeInfo,
	ReadOnlyAll, ReadOnlyClearAll, FileReadOnly, VolatileFind, GoToDialog, ShowMark, CutBookmarked, PasteBookmarked, FindCharsInRange,
	ViewInBrowser, ToggleNonPrinting, ToggleControlChars, ToggleZoomSync, MoveToNewInstance, OpenInNewInstance,
	FirstTab, LastTab, MoveTabStart, MoveTabEnd, TabColour, HideLines, TextDirection, ToggleMonitoring,
	WindowSort, ShowWindows, RecentWindow, DocumentSwitch,
	MacroSave, MacroRunSaved, MacroRunMultiple, MacroManage, RunDialog, RunSaved, RunManage, ValidateShortcuts,
	HashDialog, HelpOpen, HelpArguments, DebugInfo, CheckUpdates, SetUpdateProxy,
	TogglePanel, PanelCommand, IncrementalSearch, PostIt, DistractionFree, StyleConfigurator, ImportTheme, EditContextMenu,
	PluginsAdmin, ImportPlugin, OpenPluginsFolder, PluginCommand, TextTool, ConversionPanel, ExportDocument,
	ToggleOvertype, ResultsAction, TabAction, FindInProjects,
	RestoreClosed, FocusPanel, FolderExpandAll, FolderCollapseAll, FolderLocate, FolderOpen, FolderRunBySystem, FolderCopyName, FolderTerminal, FolderRemoveAll,
	FileAssociation,
	NotesMode, NotesNew, NotesFolder, NotesReveal, NotesRefresh, NotesImport, NotesTrash, NotesRetry,
	NotesView, NotesFocus, NotesFormat, NotesExportHtml,
};

enum PanelId { kPanelClipboard = 0, kPanelCharacters, kPanelDocuments, kPanelProject1, kPanelProject2, kPanelProject3, kPanelCount };
constexpr int kPanelFolders = 100, kPanelFunctions = 101;
const char *panelTitle(int panel);

// One line in a side panel.
struct PanelRow {
	std::string title, toolTip;
	int depth = 0;
	std::vector<std::string> columns;  // Set for panels with panelColumns.
};

// What's showing around the editor.
struct ChromeState {
	bool menu = true, toolbar = true, tabs = true, status = true, panels = true, frameless = false, largeToolbar = false;
	bool filledToolbar = false;
	std::string appearance = "system";
	std::string toolbarColour;  // RRGGBB, or empty for the default.
	int borderWidth = 0;
};

struct TabStyle {
	bool vertical = false, multiLine = false, locked = false, reduced = false;
	bool inactiveColour = true, activeBar = true, closeButton = true;
	bool pinButton = false, buttonsOnInactive = true, alternateIcons = false, rightShortcuts = true;
};
struct TabInfo;
// A tab's title with its pinned, unsaved, read-only and monitoring marks.
std::string tabDisplayTitle(const TabInfo &info, const TabStyle &style);

struct ToolItem {
	std::string title, icon, symbol;  // A freedesktop icon name, and an SF Symbol name.
	std::string customIcon;  // An image file from toolbarIcons, if the person set one up.
	std::string iconName;    // The name Notepad++'s icon sets use, like "new".
	Cmd cmd = Cmd::None;
	int arg = 0;
	bool separator = false;
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

struct TabInfo {
	std::string title, toolTip;
	bool dirty = false;
	bool readOnly = false, monitoring = false;
	bool pinned = false;
	int colour = 0;  // 1 to 5, or 0 for none.
};

// A table in a dialog, with buttons along the bottom.
struct ListDialog {
	std::string title;
	std::vector<std::string> columns;
	std::vector<std::vector<std::string>> rows;
	std::vector<std::string> buttons;
	bool multiSelect = false;
};

// What was on the command line.
struct CommandLine {
	std::vector<std::string> files, ignored;
	long line = -1, column = -1, position = -1;
	std::string language;
	bool readOnly = false, fullReadOnly = false, monitor = false, noSession = false, noTabBar = false, multiInstance = false;
	bool alwaysOnTop = false, openSession = false, foldersAsWorkspace = false, quickPrint = false;
	bool help = false, noPlugin = false, monitoringMode = false, savingForbidden = false, systemTray = false, loadingTime = false, recursive = false;
	long windowX = -1, windowY = -1;
	std::string localeCode, settingsDirectory, titleAdd, pluginMessage;
	std::string ghostName, ghostText, ghostFile;
	int ghostSpeed = -1;  // 0 slow, 1 fast, 2 fastest. Unset uses the quote's own.
};
// Things that have to be known before the Workspace exists.
void setLaunchOptions(const CommandLine &line);
const CommandLine &launchOptions();
// Where settings and state live, once doLocalConf.xml and -settingsDir have had their say.
void resolveConfigDirectories(std::string &settings, std::string &state);
// The folder holding the program, or next to the app bundle on a Mac.
std::string programDirectory();
CommandLine parseCommandLine(const std::vector<std::string> &arguments);
const char *commandLineHelp();
// MIME Tools and Converter. `ok` is false if the input isn't valid for the tool.
std::string applyTextTool(const std::string &tool, const std::string &input, bool &ok);
// Starts a program that carries on by itself, even if we quit.
bool spawnDetached(const std::vector<std::string> &arguments, const std::string &directory, std::string &error);
bool commandExists(const std::string &name);
std::string currentExecutablePath();

struct FolderEntry { std::string name, path; bool isDirectory = false; bool isSymlink = false; };

// Colours for the window around the editor, on platforms that draw their own.
// One setting in a generated form. Each platform draws the same list natively.
struct FormField {
	// Page starts a tab, and a form with pages shows them as tabs. LongText is a few lines tall.
	// StyleRow is colours and font style on one row. Flags is a grid of checkboxes.
	enum class Kind { Heading, Bool, Int, Text, Choice, Page, LongText, StyleRow, Flags };
	std::string key, label;
	Kind kind = Kind::Bool;
	// "1" or "0" for Bool, the number for Int, the choice's key for Choice. For StyleRow,
	// "RRGGBB,RRGGBB,N": foreground and background, empty for the theme's, then font style bits
	// (1 bold, 2 italic, 4 underline). For Flags, the sum of the chosen choices' keys.
	std::string value;
	int minimum = 0, maximum = 0;
	std::vector<std::pair<std::string, std::string>> choices;  // Key and label.
};

struct ShortcutEntry {
	std::string id;        // The menu path, like "Edit/Undo".
	std::string shortcut;  // Empty if there's none.
	std::string defaultShortcut;
	std::string category = "Main menu";  // Main menu, Macros, Run commands, Plugin commands or Scintilla commands.
};

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
	virtual SciView &printView() = 0;  // Never shown. It draws printouts, so the editor doesn't change.

	virtual int ask(const std::string &title, const std::string &detail, const std::vector<std::string> &buttons) = 0;
	virtual bool chooseFilesToOpen(const std::string &startDirectory, std::vector<std::string> &out) = 0;
	virtual bool chooseSavePath(const std::string &name, const std::string &startDirectory, std::string &out) = 0;
	virtual bool chooseFolder(const std::string &startDirectory, std::string &out) = 0;
	virtual bool chooseFont(std::string &family, double &size) = 0;
	virtual bool promptText(const std::string &title, const std::string &label, const std::string &initial, std::string &out) = 0;
	virtual void setClipboard(const std::string &text) = 0;
	// `format` is "text", "text/html" or "text/rtf". False if the clipboard has nothing in that form.
	virtual bool clipboardData(const std::string &format, std::string &out) = 0;
	virtual void setClipboardData(const std::string &format, const std::string &data) = 0;
	virtual void launch(const std::vector<std::string> &arguments, const std::string &directory) = 0;
	// Returns which button was pressed, or -1 if the window was closed.
	virtual int runList(const ListDialog &dialog, std::vector<int> &selection) = 0;
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
	virtual void notesChanged(const NotesState &state) {}
	virtual void menusChanged() = 0;   // Shortcuts changed, so menus need building again.
	virtual bool runForm(const std::string &title, std::vector<FormField> &fields) = 0;
	virtual void runShortcutMapper() = 0;
	// Returns false with a message if it couldn't be done. Cancelling isn't a failure.
	virtual bool print(PrintJob &job, const PrintOutput &output, std::string &error) = 0;
	virtual SciView &mapView() = 0;
	virtual void setDocumentMapVisible(bool visible) = 0;
	virtual void setFunctionListVisible(bool visible) = 0;
	virtual void functionListChanged() = 0;

	virtual void showFind(FindTab tab, const std::string &prefill, const std::string &directory) = 0;
	virtual void setFindText(const std::string &text) = 0;
	virtual FindOptions currentFindOptions() = 0;
	virtual void setFindStatus(const std::string &status, bool isError) = 0;
	virtual void setFindInFilesRunning(bool running) = 0;

	virtual void setPanelVisible(int panel, const std::string &title, bool visible) = 0;
	virtual void panelChanged(int panel) = 0;
	virtual void setIncrementalBar(bool visible) = 0;
	virtual void setChrome(const ChromeState &chrome) = 0;
	virtual void setTabStyle(const TabStyle &style) = 0;
	virtual void closeFind() = 0;  // After a search that fills the results, if the preference says so.
	virtual void setFindInSelection(bool on) = 0;
	virtual void beep() = 0;
	virtual void focusPanel(int panel) = 0;  // A PanelId, or kPanelFolders or kPanelFunctions.
	virtual void selectFolderPath(const std::string &path) = 0;
	virtual void setTray(bool visible) = 0;  // Does nothing where there's no tray to use.
	virtual void setWindowPosition(long x, long y) = 0;
	// Makes omapad the default for these extensions. False with a message if that failed.
	virtual bool registerFileTypes(const std::vector<std::string> &extensions, std::string &error) = 0;

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
	std::map<std::string, std::string> shortcuts;  // Menu path to shortcut. Empty means none.

	bool autoIndent = true, highlightCurrentLine = true, smartHighlight = true, matchBraces = true;
	bool lineNumbers = true, bookmarkMargin = true, changeHistory = true, multiEditing = true, scrollBeyondEnd = false;
	int edgeColumn = 0, caretWidth = 1;
	std::string defaultEol =
#ifdef _WIN32
		"crlf";
#else
		"lf";
#endif
	std::string defaultEncoding = "utf8";
	bool autoComplete = true, completeFunctions = true, completeWords = true, functionHints = true;
	int completeThreshold = 2;
	bool restoreSessionOnStart = true;
	int backupSeconds = 7;
	bool printLineNumbers = true;
	std::string printColours = "auto";  // auto (a light theme on white), invert, black, colour or screen.
	std::string printHeaderLeft = "$(FILE_NAME)", printHeaderMiddle, printHeaderRight = "$(SHORT_DATE)";
	std::string printFooterLeft, printFooterMiddle = "$(CURRENT_PRINTING_PAGE) / $(TOTAL_PAGES)", printFooterRight;
	int printMarginTop = 15, printMarginBottom = 15, printMarginLeft = 15, printMarginRight = 15;  // Millimetres.

	std::string dateTimeFormat = "yyyy-MM-dd HH:mm:ss";
	bool dateTimeReverse = false;
	std::string searchEngine = "https://duckduckgo.com/?q=$(CURRENT_WORD)";
	std::string updateProxy;
	std::string sessionExtension = ".session";
	bool showNonPrinting = false, showControlChars = false, npcCodepoint = false;
	bool syncZoom = false, switcherMru = true, folderDropOpensFiles = false, tabHide = false;

	// Everything Preferences.cpp describes in its table.
	std::map<std::string, std::string> prefs;
	std::string pref(const std::string &key) const;
	bool prefOn(const std::string &key) const;
	long prefInt(const std::string &key) const;
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
	std::vector<MenuItem> tabMenu(int pane, int index);
	std::vector<MenuItem> folderMenu(const std::string &path, bool isDirectory, bool isRoot) const;
	void runFolderCommand(Cmd cmd, const std::string &path);

	// Notifications from the widgets.
	void editorNotification(int pane, const SCNotification &notification);
	void resultsNotification(const SCNotification &notification);
	void mapPressed(long x, long y);  // A press or drag on the Document Map, in its own coordinates.
	void mapScrolled(long lines);
	void paneFocused(int pane);
	void applicationActivated();  // The app came to the front: check files and themes.
	bool tick();                  // Call about once a second. Writes backups and refreshes panels.
	bool prepareToQuit();         // False means don't quit.
	void openFiles(const std::vector<std::string> &paths);
	void afterStartup();          // Once the session is back and the window is ready.
	std::string windowTitle() const;
	std::string windowPath() const;
	void prepareCommandLine(const CommandLine &line);  // What has to happen before the session comes back.
	void applyCommandLine(const CommandLine &line);

	// Notes mode: callbacks take paths, never row indices, so filtering cannot retarget an action.
	bool notesMode() const { return notesEnabled; }
	NotesState notesState() const;
	bool setNotesMode(bool enabled);
	bool setNotesFolder(const std::string &path);
	void setNotesQuery(const std::string &query);
	void setNotesTag(const std::string &tag);
	bool openNote(std::string path);
	bool newNote(const std::string &initial = std::string());
	bool flushNotes();
	void refreshNotes();

	// Side panels.
	bool panelVisible(int panel) const;
	void togglePanel(int panel);
	std::vector<PanelRow> panelRows(int panel) const;
	std::string dialogDirectory() const;
	void updateMatchingTags(int pane);
	std::vector<std::string> panelColumns(int panel) const;  // Headings, if it has more than one column.
	void panelActivate(int panel, int row, int column = 0);
	void panelColumnClicked(int panel, int column);
	bool deleteShortcutCommand(const std::string &id);  // A macro or a saved run command.
	std::vector<MenuItem> panelMenu(int panel, int row) const;

	// Things the platforms ask for.
	std::string incrementalSearch(const std::string &text, bool backward, bool next, bool matchCase, bool highlightAll);
	void closeIncrementalSearch();
	std::vector<ToolItem> toolbarItems() const;
	std::vector<MenuItem> editorContextMenu() const;
	std::vector<MenuItem> statusMenu(bool rightSide) const;  // The status bar's two ends.
	std::vector<MenuItem> resultsMenu() const;
	void editorRightClicked(int pane, long position);
	void tabDoubleClicked(int pane, int index);
	void refreshChromeState() { applyChromeState(); }
	intptr_t sendToActiveEditor(unsigned message, uintptr_t w, intptr_t l);

	struct ProjectNode {
		enum class Kind { Root, Project, Folder, File } kind = Kind::Root;
		std::string name, path;
		bool expanded = false;
		std::vector<ProjectNode> children;
	};
	bool windowDirty() const;

	// Tabs.
	int paneCount() const { return 2; }
	int focusedPane() const { return focus; }
	bool splitVisible() const;
	bool splitStacked() const { return stacked; }
	std::vector<TabInfo> tabs(int pane) const;
	int activeTab(int pane) const;
	void tabHovered(int pane, int index);  // -1 when the pointer leaves.
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
	void findPanelFindAllOpen();
	void findPanelReplaceInFiles();

	bool convertActiveToEncoding(const std::string &identifier);  // False if the text can't be represented.
	bool reinterpretActiveAsEncoding(const std::string &identifier);
	void setActiveLanguage(const std::string &name);
	void setActiveLanguageForTests(const std::string &name) { setActiveLanguage(name); }
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

	// Preferences and shortcuts.
	std::vector<FormField> preferenceFields() const;
	void applyPreferenceFields(const std::vector<FormField> &fields);
	std::vector<ShortcutEntry> shortcutEntries() const;
	// Asks first if the shortcut is taken. False if it was refused or the person declined.
	bool assignShortcut(const std::string &id, const std::string &shortcut);
	void resetShortcuts();

	// Auto-completion.
	std::vector<std::string> completionCandidates(const std::string &prefix, bool functions, bool words, size_t excludeAt);
	bool showCompletion(bool functions, bool words, bool explicitRequest);
	bool showParameterHint(bool explicitRequest);
	bool hintActive() const { return hint.active; }
	size_t hintParameter() const { return hint.parameter; }
	std::string lastCompletionList;  // What was last offered, as the list widget doesn't say.

	// Function List and Document Map.
	bool functionListVisible() const { return functionPanel; }
	void refreshFunctionList();
	const std::vector<FunctionEntry> &functionEntries() const { return functions; }
	bool activeLanguageHasFunctionList() const;
	void goToFunction(size_t index);
	bool documentMapVisible() const { return mapVisible; }

	// Column Editor, as its form submits it.
	bool columnInsertText(const std::string &text);
	bool columnInsertNumbers(long initial, long increment, long repeat, int leading, int base);  // leading: 0 none, 1 zeros, 2 spaces.

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
	void afterCharacter(int pane, int character);
	void updateHintAfterCharacter(int character);
	void renderHint();
	std::string wordBeforeCaret(long &start) const;
	void showPathCompletion();
	void columnEditorDialog();
	void printActive(bool selectionOnly, const PrintOutput &output);
	std::string userLanguageFolder() const;
	// Picks from the user languages, with an optional "New language" entry that returns an empty name.
	bool chooseUserLanguage(const std::string &title, bool offerNew, bool personalOnly, std::string &outName);
	bool storeUserLanguage(const UserLanguage &language, const std::string &previousName, std::string &error);
	void removeUserLanguageFromFolder(const std::string &name, std::string &error);
	void userLanguagesChanged(const std::string &renamedFrom = std::string(), const std::string &renamedTo = std::string());
	void defineUserLanguage();
	void importUserLanguages();
	void exportUserLanguage();
	void removeUserLanguage();
	void columnInsert(const std::vector<std::string> &values);
	void multiSelectPrepare(int flags);
	long styleIndicator(int style) const;
	void styleOccurrences(int style);
	void styleSelection(int style);
	void clearStyle(int style);
	void jumpStyle(int style, bool forward);
	void copyStyled(int style);
	void goToChange(bool forward);
	void goToNextResult(bool forward);
	void selectInBraces();
	bool runFeature(Cmd cmd, int arg, const std::string &argument);
	void openExternally(const std::string &target);
	void openTerminalAt(const std::string &directory);
	void viewInBrowser(const std::string &browser);
	void openNewInstance(const std::string &path);
	std::string sessionFileText();
	bool loadSessionFile(const std::string &path);
	void beginEndSelect(bool column);
	void insertDate(int kind);
	void copyAllNames(bool fullPaths);
	void blockUncomment();
	void eolAndTrimToSpace();
	void pasteClipboardFormat(const char *format, const char *name);
	void copyBinary(bool cut);
	void pasteBinary();
	std::string selectionAsPath() const;
	void redactSelection(bool dots);
	void searchOnInternet();
	void changeSearchEngine();
	void setAllReadOnly(bool on);
	void toggleFileReadOnly();
	void goToDialog();
	void pasteToBookmarked();
	bool findCharacterInRange(long from, long to, bool backward, bool wrap);
	void findCharactersDialog();
	void setPrintableSymbols();
	void hideSelectedLines();
	bool showHiddenLinesAt(long line);
	void setTextDirection(bool rightToLeft);
	void toggleMonitoring();
	void updateMonitoredDocuments();
	void setTabColour(int pane, int index, int colour);
	void setTabPinned(int pane, int index, bool pinned);
	void sortTabs(int kind);
	void noteTabUsed();
	void activateDocument(const Document *document);
	void switchDocument(bool backward);
	void showWindowsDialog();
	std::string macroFile() const;
	void loadMacros();
	void saveMacros() const;
	void saveCurrentMacro();
	void runMacroMultiple();
	void manageMacros();
	std::string expandRunVariables(const std::string &command) const;
	void runCommandLine(const std::string &command);
	std::string runCommandFile() const;
	void loadRunCommands();
	void saveRunCommands() const;
	void runDialog();
	void manageRunCommands();
	void validateShortcuts();
	void hashDialog(HashKind kind);
	std::string debugInfo() const;
	void checkForUpdates();
	void applyChromeState();
	void appendPrefFields(std::vector<FormField> &fields) const;
	void applyPrefField(const FormField &field);
	void syncPrefsToFields();
	bool isLargeDocument() const;
	void applyExtraViewPreferences(int pane);
	void autoInsertAfter(int pane, int ch);
	void autoCloseTag(int pane);
	void delimiterSelect(int pane, long position);
	void updateLinks(int pane);
	bool openLinkAt(int pane, long position);
	void backupBeforeSave(const std::string &path);
	bool runPanelCommand(Cmd cmd, int arg, const std::string &argument);
	void panelCommand(int panel, int row, const std::string &action);
	void notePanelsChanged();
	void pollClipboard();
	ProjectNode *projectNodeAt(int panel, int row);
	ProjectNode *projectParentOf(int panel, ProjectNode *target);
	bool loadWorkspaceFile(int panel, const std::string &path);
	bool saveWorkspaceFile(int panel, const std::string &path, bool adopt);
	void findInProjects();
	void togglePostIt();
	void toggleDistractionFree();
	std::string contextMenuPath() const;
	void editContextMenu();
	void styleConfigurator();
	void applyGlobalOverride();
	void applyLanguageCustomisation();
	void importTheme();
	std::string pluginFolder() const;
	void loadPlugins();
	void pluginsAdmin();
	void importPlugin();
	void runTextTool(const std::string &tool);
	void conversionPanel();
	std::string exportHtml() const;
	std::string exportRtf() const;
	void runExport(const std::string &action);
	void loadMenuLanguage();
	void translateMenu(std::vector<MenuItem> &items) const;
	std::string catalogDirectory() const { return resourceDir; }
	std::vector<MenuItem> pluginMenu() const;
	void resultsAction(const std::string &action);
	long replaceInFileList(const FindOptions &options, const std::vector<std::string> &files);
	std::vector<std::string> filesMatching(const FindOptions &options) const;
	void searchInResults(const FindOptions &options, bool onlyFoundLines);
	bool findAllAppends = false;
	void tabAction(int pane, int index, const std::string &action);
	void appendScintillaShortcuts(std::vector<ShortcutEntry> &entries) const;
	void applyScintillaShortcuts(int pane);
	void applyChangeHistoryTo(SciView &target);
	void syncDocumentMap(bool documentChanged);
	void showDocumentMap(bool visible);
	void applyPreferencesEverywhere();
	std::vector<MenuItem> baseMenu() const;
	void updateBraceHighlight(int pane);
	void updateSmartHighlight(int pane);
	void syncScroll(int pane);

	std::string patternFrom(const FindOptions &options) const;
	int flagsFrom(const FindOptions &options) const;
	long searchIn(SciView &target, const std::string &pattern, long from, long to) const;
	const Regex *regexFor(const std::string &pattern) const;
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
	std::string findPrefill() const;
	void notifyPlugins(const char *event, const std::string &path);
	bool saveWithAdminRights(const std::string &bytes, const std::string &path, std::string &error);
	void ghostType(const std::string &text, int speed);
	void ghostTypeNext(std::shared_ptr<std::u32string> text, size_t at, int speed);
	bool aboutEasterEgg();
	void openWildcard(const std::string &pattern, bool recursive);
	void findInResultsDialog();
	std::vector<std::pair<int, int>> documentListOrder() const;  // Pane and index for each row, or -1 for a heading.
	std::vector<MenuItem> menuFromXml(const std::string &xml, const char *section, const char *fallback, int pane, int index) const;
	void markWorkspaceDirty(int panel);
	bool offerSavingWorkspaces();
	void fileAssociationDialog();
	void applyDocumentReadOnlyToOpen();
	std::vector<ToolItem> configuredToolbar(std::vector<ToolItem> items) const;

	bool runNotesCommand(Cmd cmd, int arg, const std::string &text);
	bool isNotePath(const std::string &path) const;
	bool isNoteLocation(const std::string &path) const;
	void notesTick();
	void notifyNotes();
	void formatNote(const std::string &action);
	bool saveNote(Document &document);
	void rememberNoteBaseline(const Document &document);
	void applyNotesPreferences(int pane);
	void restoreNotesMode();
	bool notesEnabled = false, notesFocus = false, notesNotifying = false;
	int notesViewMode = 0;
	std::string notesFolderPath, notesQuery, notesTag, notesError, notesLastPath;
	std::vector<NoteSummary> noteIndex;
	std::map<std::string, std::string> noteBaselines;
	std::map<std::string, unsigned long> noteSeenGeneration;
	std::weak_ptr<Document> notesPreviousDocument;
	int notesPreviousPane = 0;

	LanguageCatalog catalog;
	CompletionCatalog completion;
	FunctionListCatalog functionCatalog;
	struct HintState {
		bool active = false;
		std::string language, keyword;
		size_t overload = 0, parameter = 0;
		long open = 0;
	} hint;
	std::vector<FunctionEntry> functions;
	bool functionPanel = false;
	const void *functionsDocument = nullptr;
	unsigned long functionsGeneration = 0;
	long functionsPending = 0;
	bool mapVisible = false;
	intptr_t mapDocument = 0;
	bool mapSyncing = false;
	long lastBackupTick = 0, tickCount = 0;
	long nextResultLine = 0;
	mutable std::unique_ptr<Regex> cachedRegex;
	mutable std::string cachedRegexKey;
	mutable Regex::Options regexOptions;
	mutable Regex::Match lastMatch;
	std::string settingsDir, stateDir, homeSettingsDir;
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
	struct SavedMacro { std::string name; std::vector<MacroStep> steps; };
	std::vector<SavedMacro> savedMacros;
	void playSteps(const std::vector<MacroStep> &steps);
	std::vector<std::pair<std::string, std::string>> runCommands;  // Name and command.
	std::string lastRunCommand;
	long selectAnchor = -1;
	bool selectAnchorColumn = false;
	std::string binaryClipboard;
	struct { std::string mode = "high"; long from = 128, to = 255; bool backward = false, wrap = true; } charRange;
	std::vector<const Document *> mru;
	long switcherPosition = 0, switcherTick = -10;
	bool sessionDisabled = false, tabsHiddenByCommandLine = false;
	bool offerCreatingMissing = false, allFilesReadOnly = false, allFilesMonitored = false, savingForbidden = false;
	bool closingSeveral = false;
	int closeAllDecision = 0;  // 1 saves the rest, 2 discards the rest.
	std::vector<std::string> recentlyClosed;
	std::string lastDialogDirectory, titleSuffix;
	bool workspaceDirty[3] = {false, false, false};
	int documentSortColumn = -1;
	bool documentSortDescending = false;
	bool distractionFree = false, postIt = false, fullScreenBeforeDistraction = false;
	bool panelShown[kPanelCount] = {};
	std::vector<std::string> clipboardHistory;
	std::string lastClipboard;
	struct ProjectPanel {
		std::string file;
		ProjectNode root;
	};
	ProjectPanel projectPanels[3];
	struct LoadedPlugin {
		std::string name, folderName, path, description, error;
		bool enabled = true;
		void *handle = nullptr;
		const ::OmapadPluginInfo *info = nullptr;
		void (*notify)(const char *, const char *) = nullptr;
	};
	std::vector<LoadedPlugin> plugins;
	std::map<std::string, std::string> menuTranslations;
	std::string resourceDir;
	long lastAutoInserted = -1;
	int completionKeys = 3;

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
