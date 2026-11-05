// Every preference that's simple enough to describe in a table, and what each does to an editor.
#include "WorkspaceInternal.h"

#include "SciLexer.h"
#include "Scintilla.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace omapad {

namespace {

using Choices = std::vector<std::pair<std::string, std::string>>;

struct PrefDef {
	const char *page, *key, *label;
	char kind;  // b bool, i int, t text, l long text, c choice, h heading.
	const char *fallback;
	int minimum, maximum;
	Choices choices;
};

const std::vector<PrefDef> &prefTable() {
	static const std::vector<PrefDef> table = {
		{"General", "hideMenu", "Hide the menu bar (F10 or Alt shows it again, on Linux)", 'b', "0", 0, 0, {}},
		{"General", "hideStatus", "Hide the status bar", 'b', "0", 0, 0, {}},
		{"General", "appearance", "Window appearance", 'c', "system", 0, 0, {{"system", "Follow the system"}, {"light", "Light mode"}, {"dark", "Dark mode"}}},
		{"General", "docListExtColumn", "Document List: show the extension column", 'b', "1", 0, 0, {}},
		{"General", "docListPathColumn", "Document List: show the path column", 'b', "1", 0, 0, {}},
		{"General", "hideRightShortcuts", "Hide the new, list and close buttons beside the tabs", 'b', "0", 0, 0, {}},
		{"General", "darkTone", "Dark mode tone for the window (Linux)", 'c', "none", 0, 0, {{"none", "None"}, {"black", "Black"}, {"red", "Red"}, {"green", "Green"}, {"blue", "Blue"}, {"purple", "Purple"}, {"cyan", "Cyan"}, {"olive", "Olive"}, {"custom", "Custom"}}},
		{"General", "darkToneBackground", "Custom tone background (RRGGBB)", 't', "", 0, 0, {}},
		{"General", "darkToneText", "Custom tone text (RRGGBB)", 't', "", 0, 0, {}},
		{"General", "uiLanguage", "Menu language (a file from the nativeLang folder, empty for English)", 't', "", 0, 0, {}},

		{"Toolbar", "hideToolbar", "Hide the toolbar", 'b', "0", 0, 0, {}},
		{"Toolbar", "toolbarSize", "Icon size", 'c', "small", 0, 0, {{"small", "Small"}, {"large", "Large"}}},
		{"Toolbar", "toolbarFilled", "Filled icons", 'b', "0", 0, 0, {}},
		{"Toolbar", "toolbarColour", "Colorization", 'c', "default", 0, 0, {{"default", "Default"}, {"red", "Red"}, {"green", "Green"}, {"blue", "Blue"}, {"purple", "Purple"}, {"cyan", "Cyan"}, {"olive", "Olive"}, {"yellow", "Yellow"}, {"custom", "Custom"}}},
		{"Toolbar", "toolbarCustomColour", "Custom colour (RRGGBB)", 't', "", 0, 0, {}},

		{"Tab Bar", "tabHide", "Hide the tab bar", 'b', "0", 0, 0, {}},
		{"Tab Bar", "tabVertical", "Vertical tabs", 'b', "0", 0, 0, {}},
		{"Tab Bar", "tabMultiLine", "Multi-line tabs", 'b', "0", 0, 0, {}},
		{"Tab Bar", "tabLock", "Lock (no drag and drop)", 'b', "0", 0, 0, {}},
		{"Tab Bar", "tabDoubleClickClose", "Double click to close a document", 'b', "0", 0, 0, {}},
		{"Tab Bar", "tabExitOnLast", "Exit when the last tab closes", 'b', "0", 0, 0, {}},
		{"Tab Bar", "tabMaxLabel", "Maximum tab label length (0 for none)", 'i', "0", 0, 200, {}},
		{"Tab Bar", "tabReduce", "Reduce: smaller tabs", 'b', "0", 0, 0, {}},
		{"Tab Bar", "tabInactiveColour", "Change the inactive tab colour", 'b', "1", 0, 0, {}},
		{"Tab Bar", "tabActiveBar", "Draw a coloured bar on the active tab", 'b', "1", 0, 0, {}},
		{"Tab Bar", "tabCloseButton", "Show the close button", 'b', "1", 0, 0, {}},
		{"Tab Bar", "tabPinEnabled", "Enable the pin tab feature", 'b', "1", 0, 0, {}},
		{"Tab Bar", "tabPinButton", "Show a pin button on each tab", 'b', "0", 0, 0, {}},
		{"Tab Bar", "tabButtonsInactive", "Show buttons on inactive tabs", 'b', "1", 0, 0, {}},
		{"Tab Bar", "tabAlternateIcons", "Alternate icons: use symbols for unsaved and read-only", 'b', "0", 0, 0, {}},

		{"Editing", "currentLineFrame", "Show the current line as a frame instead of a background", 'b', "0", 0, 0, {}},
		{"Editing", "currentLineFrameWidth", "Frame width", 'i', "1", 1, 6, {}},
		{"Editing", "caretBlink", "Caret blink rate in milliseconds (0 for none)", 'i', "600", 0, 2500, {}},
		{"Editing", "wrapMode", "Line wrap", 'c', "default", 0, 0, {{"default", "Default"}, {"aligned", "Aligned"}, {"indent", "Indent"}}},
		{"Editing", "smoothFont", "Enable smooth font", 'b', "1", 0, 0, {}},
		{"Editing", "virtualSpace", "Enable virtual space", 'b', "0", 0, 0, {}},
		{"Editing", "foldToggle", "Make current level folding commands toggle", 'b', "0", 0, 0, {}},
		{"Editing", "rightClickKeepsSelection", "Keep the selection when right-clicking outside it", 'b', "0", 0, 0, {}},
		{"Editing", "copyLineNoSelection", "Copy or cut the line when nothing is selected", 'b', "0", 0, 0, {}},
		{"Editing", "selectedForeground", "Selected text colour (RRGGBB, empty for the theme's)", 't', "", 0, 0, {}},
		{"Editing", "disableAdvancedScroll", "Disable the advanced scrolling feature", 'b', "0", 0, 0, {}},
		{"Editing", "disableDragDrop", "Disable selected text drag and drop", 'b', "0", 0, 0, {}},
		{"Editing", "columnToMulti", "Turn a column selection into multi-editing", 'b', "1", 0, 0, {}},
		{"Editing", "eolAppearance", "End of line symbols", 'c', "default", 0, 0, {{"default", "Default"}, {"plain", "Plain text"}, {"custom", "Custom colour"}}},
		{"Editing", "eolColour", "End of line colour (RRGGBB)", 't', "808080", 0, 0, {}},
		{"Editing", "npcAppearance", "Non-printing characters show as", 'c', "abbreviation", 0, 0, {{"abbreviation", "Abbreviation"}, {"codepoint", "Codepoint"}}},
		{"Editing", "npcColour", "Non-printing character colour (RRGGBB, empty for the default)", 't', "", 0, 0, {}},
		{"Editing", "preventC0", "Prevent typing control characters", 'b', "1", 0, 0, {}},

		{"Margins", "foldStyle", "Fold margin style", 'c', "box", 0, 0, {{"simple", "Simple"}, {"arrow", "Arrow"}, {"circle", "Circle tree"}, {"box", "Box tree"}, {"none", "None"}}},
		{"Margins", "edgeMode", "Vertical edge", 'c', "line", 0, 0, {{"line", "Line mode"}, {"background", "Background mode"}}},
		{"Margins", "verticalEdges", "Extra vertical edge columns (like: 80 100)", 't', "", 0, 0, {}},
		{"Margins", "changeHistoryMode", "Change history", 'c', "margin", 0, 0, {{"margin", "Show in the margin"}, {"text", "Show in the text"}, {"both", "Both"}}},
		{"Margins", "lineNumberWidth", "Line number width", 'c', "dynamic", 0, 0, {{"dynamic", "Dynamic"}, {"constant", "Constant"}}},
		{"Margins", "borderWidth", "Border width around the editor", 'i', "0", 0, 30, {}},
		{"Margins", "paddingLeft", "Padding, left", 'i', "1", 0, 30, {}},
		{"Margins", "paddingRight", "Padding, right", 'i', "1", 0, 30, {}},
		{"Margins", "distractionPadding", "Distraction Free padding", 'i', "120", 0, 600, {}},

		{"New Document", "defaultLanguage", "Default language (a name from the Language menu, like: python)", 't', "normal", 0, 0, {}},
		{"New Document", "openNewAtStart", "Always open a new document at startup", 'b', "0", 0, 0, {}},
		{"New Document", "firstLineTitle", "Use the first line as the untitled tab's name", 'b', "0", 0, 0, {}},
		{"New Document", "applyToAnsi", "Open plain ASCII files as the default encoding", 'b', "0", 0, 0, {}},

		{"Default Folder", "defaultDirMode", "Open and save dialogs start in", 'c', "follow", 0, 0, {{"follow", "The current document's folder"}, {"remember", "The last used folder"}, {"custom", "This folder"}}},
		{"Default Folder", "defaultDir", "Folder", 't', "", 0, 0, {}},
		{"Default Folder", "folderDropOpensFiles", "Dropping a folder opens its files instead of Folder as Workspace", 'b', "0", 0, 0, {}},

		{"Recent Files", "recentCheck", "Check that recent files still exist at launch", 'b', "1", 0, 0, {}},
		{"Recent Files", "recentMax", "Maximum number of entries", 'i', "20", 0, 30, {}},
		{"Recent Files", "recentInSubmenu", "Show them in a submenu", 'b', "1", 0, 0, {}},
		{"Recent Files", "recentDisplay", "Display", 'c', "name", 0, 0, {{"name", "Only the file name"}, {"path", "The full path"}, {"custom", "The full path, up to this length"}}},
		{"Recent Files", "recentMaxLength", "Maximum length", 'i', "60", 1, 259, {}},

		{"Language", "languageCompact", "Make the Language menu compact", 'b', "1", 0, 0, {}},
		{"Language", "disabledLanguages", "Languages to hide from the menu (names, separated by spaces)", 'l', "", 0, 0, {}},
		{"Language", "languageTabs", "Per-language indent, like: python=4s html=2t (s spaces, t tabs)", 'l', "", 0, 0, {}},
		{"Language", "sqlBackslash", "Treat backslash as an escape character for SQL", 'b', "1", 0, 0, {}},

		{"Indentation", "backspaceUnindent", "The Backspace key unindents", 'b', "0", 0, 0, {}},
		{"Indentation", "autoIndentMode", "Auto-indent", 'c', "advanced", 0, 0, {{"none", "None"}, {"basic", "Basic"}, {"advanced", "Advanced"}}},

		{"Highlighting", "tokenMatchCase", "Style tokens: match case", 'b', "0", 0, 0, {}},
		{"Highlighting", "tokenWholeWord", "Style tokens: match whole word only", 'b', "1", 0, 0, {}},
		{"Highlighting", "matchTags", "Highlight matching tags", 'b', "1", 0, 0, {}},
		{"Highlighting", "matchTagAttributes", "Highlight tag attributes", 'b', "1", 0, 0, {}},
		{"Highlighting", "matchTagComment", "Highlight comment, PHP and ASP zones", 'b', "1", 0, 0, {}},
		{"Highlighting", "smartOtherView", "Smart highlighting also in the other view", 'b', "0", 0, 0, {}},
		{"Highlighting", "smartCase", "Smart highlighting: match case", 'b', "0", 0, 0, {}},
		{"Highlighting", "smartWord", "Smart highlighting: match whole word only", 'b', "1", 0, 0, {}},
		{"Highlighting", "smartUseFind", "Smart highlighting: use the Find dialog's settings", 'b', "0", 0, 0, {}},

		{"Print", "printFormFeed", "Print a form feed as a page break", 'b', "1", 0, 0, {}},
		{"Print", "printHeaderBold", "Header in bold", 'b', "0", 0, 0, {}},
		{"Print", "printHeaderItalic", "Header in italic", 'b', "0", 0, 0, {}},
		{"Print", "printFooterBold", "Footer in bold", 'b', "0", 0, 0, {}},
		{"Print", "printFooterItalic", "Footer in italic", 'b', "0", 0, 0, {}},

		{"Searching", "fillFindFromSelection", "Fill the Find field with the selected text", 'b', "1", 0, 0, {}},
		{"Searching", "fillMaxChars", "Maximum characters to fill from the selection", 'i', "1024", 1, 16384, {}},
		{"Searching", "fillSelectWord", "Use the word under the caret when nothing is selected", 'b', "1", 0, 0, {}},
		{"Searching", "fillInSelection", "Check In Selection when the selection is at least this long (0 for never)", 'i', "1024", 0, 1000000, {}},
		{"Searching", "fillFilesDirectory", "Fill the Find in Files folder from the active document", 'b', "1", 0, 0, {}},
		{"Searching", "findMonospace", "Use a monospaced font in the Find window", 'b', "0", 0, 0, {}},
		{"Searching", "findTransparency", "Find window transparency", 'c', "none", 0, 0, {{"none", "None"}, {"lostFocus", "On losing focus"}, {"always", "Always"}}},
		{"Searching", "findOpacity", "Find window opacity in percent", 'i', "70", 20, 100, {}},
		{"Searching", "resultsPurge", "Search results: clear them for each search", 'b', "1", 0, 0, {}},
		{"Searching", "findStaysOpen", "The Find window stays open after a search that fills the results", 'b', "1", 0, 0, {}},
		{"Searching", "confirmReplaceAllOpen", "Confirm Replace All in All Opened Documents", 'b', "1", 0, 0, {}},
		{"Searching", "replaceNoMove", "Replace: don't move to the following occurrence", 'b', "0", 0, 0, {}},
		{"Searching", "resultsOnePerLine", "Search results: show one entry per found line", 'b', "0", 0, 0, {}},
		{"Searching", "preferDisk", "Find in Files: prefer the file on disk over open documents", 'b', "1", 0, 0, {}},

		{"Backup", "rememberSession", "Remember the current session for the next launch", 'b', "1", 0, 0, {}},
		{"Backup", "snapshot", "Enable session snapshot and periodic backup", 'b', "1", 0, 0, {}},
		{"Backup", "backupPath", "Snapshot folder (empty for the default)", 't', "", 0, 0, {}},
		{"Backup", "rememberInaccessible", "Remember files that can't be reached from the last session", 'b', "0", 0, 0, {}},
		{"Backup", "backupOnSave", "Backup on save", 'c', "none", 0, 0, {{"none", "None"}, {"simple", "Simple backup"}, {"verbose", "Verbose backup"}}},
		{"Backup", "backupDir", "Backup folder (empty for next to the file)", 't', "", 0, 0, {}},

		{"Auto-Completion", "completeBrief", "Make the list brief: only matches of the typed word", 'b', "1", 0, 0, {}},
		{"Auto-Completion", "completeInsertTab", "Insert the selection with Tab", 'b', "1", 0, 0, {}},
		{"Auto-Completion", "completeInsertEnter", "Insert the selection with Enter", 'b', "1", 0, 0, {}},
		{"Auto-Completion", "completeIgnoreNumbers", "Ignore numbers", 'b', "1", 0, 0, {}},
		{"Auto-Completion", "autoCloseTag", "Auto-insert the HTML or XML close tag", 'b', "0", 0, 0, {}},
		{"Auto-Completion", "pairParen", "Auto-insert ( )", 'b', "0", 0, 0, {}},
		{"Auto-Completion", "pairBracket", "Auto-insert [ ]", 'b', "0", 0, 0, {}},
		{"Auto-Completion", "pairBrace", "Auto-insert { }", 'b', "0", 0, 0, {}},
		{"Auto-Completion", "pairDouble", "Auto-insert \" \"", 'b', "0", 0, 0, {}},
		{"Auto-Completion", "pairSingle", "Auto-insert ' '", 'b', "0", 0, 0, {}},
		{"Auto-Completion", "pair1", "Matched pair 1 (two characters, like: <>)", 't', "", 0, 0, {}},
		{"Auto-Completion", "pair2", "Matched pair 2", 't', "", 0, 0, {}},
		{"Auto-Completion", "pair3", "Matched pair 3", 't', "", 0, 0, {}},

		{"Instances and Dates", "instanceMode", "Instances", 'c', "mono", 0, 0, {{"mono", "One instance"}, {"multi", "Always a new instance"}, {"session", "Open a session in a new instance"}}},
		{"Instances and Dates", "dateTimeFormatPref", "Custom date and time format", 't', "yyyy-MM-dd HH:mm:ss", 0, 0, {}},
		{"Instances and Dates", "dateTimeReversePref", "Reverse the default date and time order", 'b', "0", 0, 0, {}},
		{"Instances and Dates", "rememberPanels", "Remember which panels are open with -nosession", 'b', "1", 0, 0, {}},
		{"Instances and Dates", "rememberPanelClipboard", "Remember Clipboard History", 'b', "1", 0, 0, {}},
		{"Instances and Dates", "rememberPanelDocuments", "Remember Document List", 'b', "1", 0, 0, {}},
		{"Instances and Dates", "rememberPanelCharacters", "Remember Character Panel", 'b', "1", 0, 0, {}},
		{"Instances and Dates", "rememberPanelFolders", "Remember Folder as Workspace", 'b', "1", 0, 0, {}},
		{"Instances and Dates", "rememberPanelProjects", "Remember Project Panels", 'b', "1", 0, 0, {}},
		{"Instances and Dates", "rememberPanelMap", "Remember Document Map", 'b', "1", 0, 0, {}},
		{"Instances and Dates", "rememberPanelFunctions", "Remember Function List", 'b', "1", 0, 0, {}},

		{"Delimiters", "wordChars", "Add these characters to the word characters", 't', "", 0, 0, {}},
		{"Delimiters", "delimOpen", "Delimiter selection (Ctrl + double click): open", 't', "(", 0, 0, {}},
		{"Delimiters", "delimClose", "Delimiter selection: close", 't', ")", 0, 0, {}},
		{"Delimiters", "delimMultiline", "Allow it on several lines", 'b', "0", 0, 0, {}},

		{"Cloud and Links", "cloudPath", "Keep settings in this folder (empty for no cloud, needs a restart)", 't', "", 0, 0, {}},
		{"Cloud and Links", "links", "Clickable links: double click to open", 'b', "1", 0, 0, {}},
		{"Cloud and Links", "linkNoUnderline", "No underline", 'b', "0", 0, 0, {}},
		{"Cloud and Links", "linkFullBox", "Full box mode", 'b', "0", 0, 0, {}},
		{"Cloud and Links", "uriSchemes", "Link schemes", 'l', "http:// https:// ftp:// ftps:// file:// mailto: svn:// git:// ssh:// sftp:// irc:// ldap:// telnet:// news: gopher:// smb://", 0, 0, {}},

		{"Performance", "largeFile", "Enable the large file restriction: no syntax highlighting", 'b', "1", 0, 0, {}},
		{"Performance", "largeFileMb", "Large file size in MB", 'i', "200", 1, 2046, {}},
		{"Performance", "largeNoWrap", "Turn word wrap off for large files", 'b', "1", 0, 0, {}},
		{"Performance", "largeComplete", "Allow auto-completion in large files", 'b', "0", 0, 0, {}},
		{"Performance", "largeSmart", "Allow smart highlighting in large files", 'b', "0", 0, 0, {}},
		{"Performance", "largeBrace", "Allow brace match in large files", 'b', "0", 0, 0, {}},
		{"Performance", "largeLinks", "Allow clickable links in large files", 'b', "0", 0, 0, {}},

		{"Misc", "searchEnginePref", "Search engine address, with $(CURRENT_WORD)", 't', "https://duckduckgo.com/?q=$(CURRENT_WORD)", 0, 0, {}},
		{"Misc", "systemTray", "System tray (Mac only)", 'c', "none", 0, 0, {{"none", "No tray icon"}, {"always", "Always show"}, {"minimize", "Minimize to the tray"}, {"close", "Close to the tray"}}},
		{"Misc", "functionListSorted", "Function List: start sorted by name", 'b', "0", 0, 0, {}},
		{"Misc", "fileDetection", "Check for changes on disk", 'c', "on", 0, 0, {{"on", "Enabled"}, {"off", "Disabled"}}},
		{"Misc", "updateSilently", "Update silently", 'b', "0", 0, 0, {}},
		{"Misc", "scrollToEnd", "Scroll to the last line after an update", 'b', "0", 0, 0, {}},
		{"Misc", "switcherEnabled", "Enable the document switcher (Ctrl+Tab)", 'b', "1", 0, 0, {}},
		{"Misc", "switcherMruPref", "Most recently used order", 'b', "1", 0, 0, {}},
		{"Misc", "peekTab", "Peek at a document when hovering on its tab", 'b', "0", 0, 0, {}},
		{"Misc", "peekMap", "Peek on the Document Map when hovering on a tab", 'b', "0", 0, 0, {}},
		{"Misc", "muteSounds", "Mute all sounds", 'b', "0", 0, 0, {}},
		{"Misc", "autoDetectEncoding", "Autodetect the character encoding", 'b', "1", 0, 0, {}},
		{"Misc", "titleFileOnly", "Show only the file name in the title bar", 'b', "0", 0, 0, {}},
		{"Misc", "saveAllConfirm", "Enable the Save All confirm dialog", 'b', "0", 0, 0, {}},
		{"Misc", "folderSymlinks", "Allow symlinks in Folder as Workspace", 'b', "1", 0, 0, {}},
		{"Misc", "sessionExtensionPref", "Session file extension", 't', ".session", 0, 0, {}},
		{"Misc", "workspaceExtension", "Workspace file extension", 't', ".workspace", 0, 0, {}},
	};
	return table;
}

}  // namespace

std::string Settings::pref(const std::string &key) const {
	const auto found = prefs.find(key);
	if (found != prefs.end()) return found->second;
	for (const PrefDef &def : prefTable())
		if (key == def.key) return def.fallback;
	return std::string();
}

bool Settings::prefOn(const std::string &key) const { return pref(key) == "1"; }
long Settings::prefInt(const std::string &key) const { return std::strtol(pref(key).c_str(), nullptr, 10); }

void Workspace::appendPrefFields(std::vector<FormField> &fields) const {
	std::string page;
	for (const PrefDef &def : prefTable()) {
		if (page != def.page) {
			page = def.page;
			FormField heading;
			heading.kind = FormField::Kind::Page;
			heading.label = page;
			fields.push_back(heading);
		}
		FormField field;
		field.key = def.key;
		field.label = def.label;
		field.value = settings.pref(def.key);
		field.minimum = def.minimum;
		field.maximum = def.maximum;
		field.choices = def.choices;
		field.kind = def.kind == 'b' ? FormField::Kind::Bool : def.kind == 'i' ? FormField::Kind::Int
			: def.kind == 'l' ? FormField::Kind::LongText : def.kind == 'c' ? FormField::Kind::Choice : FormField::Kind::Text;
		fields.push_back(field);
	}
}

void Workspace::applyPrefField(const FormField &field) {
	for (const PrefDef &def : prefTable()) {
		if (field.key != def.key) continue;
		std::string value = field.value;
		if (def.kind == 'i') {
			const long number = std::strtol(value.c_str(), nullptr, 10);
			value = std::to_string(std::min<long>(std::max<long>(number, def.minimum), def.maximum));
		}
		settings.prefs[def.key] = value;
		return;
	}
}

void Workspace::syncPrefsToFields() {
	settings.dateTimeFormat = settings.pref("dateTimeFormatPref");
	settings.dateTimeReverse = settings.prefOn("dateTimeReversePref");
	settings.sessionExtension = settings.pref("sessionExtensionPref");
	settings.switcherMru = settings.prefOn("switcherMruPref");
	settings.folderDropOpensFiles = settings.prefOn("folderDropOpensFiles");
	settings.tabHide = settings.prefOn("tabHide");
	settings.npcCodepoint = settings.pref("npcAppearance") == "codepoint";
	settings.searchEngine = settings.pref("searchEnginePref");
}

bool Workspace::isLargeDocument() const {
	if (!settings.prefOn("largeFile")) return false;
	return s(SCI_GETLENGTH) >= settings.prefInt("largeFileMb") * 1024 * 1024;
}

void Workspace::applyExtraViewPreferences(int pane) {
	SciView &v = view(pane);
	const auto colourOf = [](const std::string &hex, long &out) {
		if (hex.size() != 6) return false;
		char *end = nullptr;
		const unsigned long rgb = std::strtoul(hex.c_str(), &end, 16);
		if (*end) return false;
		out = static_cast<long>(((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF) | 0xFF000000UL);
		return true;
	};
	v.send(SCI_SETCARETLINEFRAME, settings.prefOn("currentLineFrame") ? settings.prefInt("currentLineFrameWidth") : 0);
	v.send(SCI_SETCARETPERIOD, settings.prefInt("caretBlink"));
	const std::string wrap = settings.pref("wrapMode");
	v.send(SCI_SETWRAPINDENTMODE, wrap == "aligned" ? SC_WRAPINDENT_SAME : wrap == "indent" ? SC_WRAPINDENT_INDENT : SC_WRAPINDENT_FIXED);
	v.send(SCI_SETFONTQUALITY, settings.prefOn("smoothFont") ? SC_EFF_QUALITY_DEFAULT : SC_EFF_QUALITY_NON_ANTIALIASED);
	v.send(SCI_SETVIRTUALSPACEOPTIONS, SCVS_RECTANGULARSELECTION | (settings.prefOn("virtualSpace") ? SCVS_USERACCESSIBLE | SCVS_NOWRAPLINESTART : 0));
	v.send(SCI_SETBACKSPACEUNINDENTS, settings.prefOn("backspaceUnindent"));
	v.send(SCI_SETMOUSEWHEELCAPTURES, !settings.prefOn("disableAdvancedScroll"));
	v.send(SCI_SETDRAGDROPENABLED, !settings.prefOn("disableDragDrop"));
	v.send(SCI_SETADDITIONALSELECTIONTYPING, settings.multiEditing || settings.prefOn("columnToMulti"));
	long colour = 0;
	if (colourOf(settings.pref("selectedForeground"), colour)) v.send(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_TEXT, colour);
	else v.send(SCI_RESETELEMENTCOLOUR, SC_ELEMENT_SELECTION_TEXT);

	// Word characters: Scintilla's default, plus the person's.
	std::string words = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_" + settings.pref("wordChars");
	v.send(SCI_SETWORDCHARS, 0, reinterpret_cast<intptr_t>(words.c_str()));

	// Edges.
	const std::string edgeList = settings.pref("verticalEdges");
	v.send(SCI_MULTIEDGECLEARALL);
	if (!edgeList.empty()) {
		v.send(SCI_SETEDGEMODE, EDGE_MULTILINE);
		std::istringstream columns(edgeList);
		long column = 0;
		while (columns >> column) v.send(SCI_MULTIEDGEADDLINE, column, 0xC0C0C0);
		if (settings.edgeColumn > 0) v.send(SCI_MULTIEDGEADDLINE, settings.edgeColumn, 0xC0C0C0);
	} else if (settings.edgeColumn > 0) {
		v.send(SCI_SETEDGEMODE, settings.pref("edgeMode") == "background" ? EDGE_BACKGROUND : EDGE_LINE);
		v.send(SCI_SETEDGECOLUMN, settings.edgeColumn);
	}

	// Padding: also what gives Distraction Free its wide margins.
	v.send(SCI_SETMARGINLEFT, 0, distractionFree ? settings.prefInt("distractionPadding") : settings.prefInt("paddingLeft"));
	v.send(SCI_SETMARGINRIGHT, 0, distractionFree ? settings.prefInt("distractionPadding") : settings.prefInt("paddingRight"));

	// Fold margin.
	const std::string fold = settings.pref("foldStyle");
	v.send(SCI_SETMARGINWIDTHN, kMarginFold, fold == "none" ? 0 : 14);
	struct Markers { int folder, open, sub, tail, end, openMid, midTail; };
	const Markers box = {SC_MARK_BOXPLUS, SC_MARK_BOXMINUS, SC_MARK_VLINE, SC_MARK_LCORNER, SC_MARK_BOXPLUSCONNECTED, SC_MARK_BOXMINUSCONNECTED, SC_MARK_TCORNER};
	const Markers circle = {SC_MARK_CIRCLEPLUS, SC_MARK_CIRCLEMINUS, SC_MARK_VLINE, SC_MARK_LCORNERCURVE, SC_MARK_CIRCLEPLUSCONNECTED, SC_MARK_CIRCLEMINUSCONNECTED, SC_MARK_TCORNERCURVE};
	const Markers arrow = {SC_MARK_ARROW, SC_MARK_ARROWDOWN, SC_MARK_EMPTY, SC_MARK_EMPTY, SC_MARK_EMPTY, SC_MARK_EMPTY, SC_MARK_EMPTY};
	const Markers simple = {SC_MARK_PLUS, SC_MARK_MINUS, SC_MARK_EMPTY, SC_MARK_EMPTY, SC_MARK_EMPTY, SC_MARK_EMPTY, SC_MARK_EMPTY};
	const Markers &m = fold == "circle" ? circle : fold == "arrow" ? arrow : fold == "simple" ? simple : box;
	v.send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDER, m.folder);
	v.send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPEN, m.open);
	v.send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERSUB, m.sub);
	v.send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERTAIL, m.tail);
	v.send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEREND, m.end);
	v.send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPENMID, m.openMid);
	v.send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERMIDTAIL, m.midTail);

	// Change history.
	if (settings.changeHistory) {
		const std::string history = settings.pref("changeHistoryMode");
		int flags = SC_CHANGE_HISTORY_ENABLED;
		if (history != "text") flags |= SC_CHANGE_HISTORY_MARKERS;
		if (history != "margin") flags |= SC_CHANGE_HISTORY_INDICATORS;
		v.send(SCI_SETCHANGEHISTORY, flags);
		v.send(SCI_SETMARGINWIDTHN, kMarginHistory, history == "text" ? 0 : 4);
	}

	// Completion keys.
	const bool tab = settings.prefOn("completeInsertTab"), enter = settings.prefOn("completeInsertEnter");
	completionKeys = (tab ? 1 : 0) | (enter ? 2 : 0);

	// Links.
	v.send(SCI_INDICSETSTYLE, kIndicatorLink, settings.prefOn("linkFullBox") ? INDIC_FULLBOX : settings.prefOn("linkNoUnderline") ? INDIC_TEXTFORE : INDIC_PLAIN);
	v.send(SCI_INDICSETFORE, kIndicatorLink, 0xC06000);
	v.send(SCI_INDICSETHOVERSTYLE, kIndicatorLink, INDIC_FULLBOX);
	v.send(SCI_INDICSETALPHA, kIndicatorLink, 40);
	for (const int tag : {kIndicatorTag, kIndicatorTagAttribute}) {
		v.send(SCI_INDICSETSTYLE, tag, INDIC_ROUNDBOX);
		v.send(SCI_INDICSETFORE, tag, tag == kIndicatorTag ? 0xFF0080 : 0x00C000);
		v.send(SCI_INDICSETALPHA, tag, 70);
		v.send(SCI_INDICSETUNDER, tag, 1);
	}
	setPrintableSymbols();
	const long eolColour = [&] {
		long out = 0;
		return colourOf(settings.pref("eolColour"), out) ? out : 0xFF808080L;
	}();
	// Scintilla draws line endings as representations too, once they have one.
	static const char *const eols[] = {"\r\n", "\r", "\n"};
	static const char *const eolNames[] = {"CRLF", "CR", "LF"};
	const std::string appearance = settings.pref("eolAppearance");
	for (int i = 0; i < 3; i++) {
		if (appearance == "default") break;
		v.send(SCI_SETREPRESENTATION, reinterpret_cast<uintptr_t>(eols[i]), reinterpret_cast<intptr_t>(eolNames[i]));
		v.send(SCI_SETREPRESENTATIONAPPEARANCE, reinterpret_cast<uintptr_t>(eols[i]), appearance == "custom" ? SC_REPRESENTATION_COLOUR : SC_REPRESENTATION_PLAIN);
		if (appearance == "custom") v.send(SCI_SETREPRESENTATIONCOLOUR, reinterpret_cast<uintptr_t>(eols[i]), eolColour);
	}
}

#pragma mark - Typing helpers

void Workspace::autoInsertAfter(int pane, int ch) {
	SciView &v = view(pane);
	const long position = v.send(SCI_GETCURRENTPOS);
	const auto insert = [&](const std::string &text) { v.send(SCI_INSERTTEXT, position, reinterpret_cast<intptr_t>(text.c_str())); };
	const int next = position < v.send(SCI_GETLENGTH) ? static_cast<int>(v.send(SCI_GETCHARAT, position)) : 0;
	const int previous = position >= 2 ? static_cast<int>(v.send(SCI_GETCHARAT, position - 2)) : 0;
	// Typing a closer that was auto-inserted just steps over it.
	const auto stepOver = [&]() {
		if (next == ch && lastAutoInserted == position) {
			v.send(SCI_DELETERANGE, position, 1);
			return true;
		}
		return false;
	};
	const auto wordChar = [](int c) { return std::isalnum(c & 0xFF) || c == '_'; };
	switch (ch) {
	case ')': case ']': case '}':
		stepOver();
		return;
	case '(':
		if (settings.prefOn("pairParen") && !wordChar(next)) { insert(")"); lastAutoInserted = position; }
		return;
	case '[':
		if (settings.prefOn("pairBracket") && !wordChar(next)) { insert("]"); lastAutoInserted = position; }
		return;
	case '{':
		if (settings.prefOn("pairBrace") && !wordChar(next)) { insert("}"); lastAutoInserted = position; }
		return;
	case '"':
		if (stepOver()) return;
		if (settings.prefOn("pairDouble") && !wordChar(next) && !wordChar(previous)) { insert("\""); lastAutoInserted = position; }
		return;
	case '\'':
		if (stepOver()) return;
		if (settings.prefOn("pairSingle") && !wordChar(next) && !wordChar(previous)) { insert("'"); lastAutoInserted = position; }
		return;
	case '>':
		if (settings.prefOn("autoCloseTag")) autoCloseTag(pane);
		break;
	default:
		break;
	}
	for (const char *key : {"pair1", "pair2", "pair3"}) {
		const std::string pair = settings.pref(key);
		if (pair.size() == 2 && pair[0] == static_cast<char>(ch)) {
			insert(std::string(1, pair[1]));
			lastAutoInserted = position;
			return;
		}
	}
}

void Workspace::autoCloseTag(int pane) {
	SciView &v = view(pane);
	const Document *document = activeDocument();
	if (!document) return;
	const std::string &language = document->language->name;
	if (language != "html" && language != "xml" && language != "php" && language != "asp" && language != "jsp") return;
	const long position = v.send(SCI_GETCURRENTPOS);
	const long start = std::max(0L, position - 1024);
	const char *pointer = reinterpret_cast<const char *>(v.send(SCI_GETRANGEPOINTER, start, position - start));
	if (!pointer) return;
	const std::string before(pointer, static_cast<size_t>(position - start));
	const size_t open = before.rfind('<');
	if (open == std::string::npos || open + 1 >= before.size()) return;
	const char first = before[open + 1];
	if (first == '/' || first == '!' || first == '?' || (before.size() >= 2 && before[before.size() - 2] == '/')) return;
	size_t end = open + 1;
	while (end < before.size() && (std::isalnum(static_cast<unsigned char>(before[end])) || before[end] == '-' || before[end] == ':' || before[end] == '_')) end++;
	const std::string name = before.substr(open + 1, end - open - 1);
	static const char *const voidTags[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"};
	if (name.empty()) return;
	if (language != "xml")
		for (const char *tag : voidTags)
			if (name == tag) return;
	const std::string close = "</" + name + ">";
	v.send(SCI_INSERTTEXT, position, reinterpret_cast<intptr_t>(close.c_str()));
}

void Workspace::delimiterSelect(int pane, long position) {
	SciView &v = view(pane);
	const std::string open = settings.pref("delimOpen"), close = settings.pref("delimClose");
	if (open.empty() || close.empty()) return;
	const bool multiline = settings.prefOn("delimMultiline");
	const long line = v.send(SCI_LINEFROMPOSITION, position);
	const long from = multiline ? 0 : v.send(SCI_POSITIONFROMLINE, line);
	const long to = multiline ? v.send(SCI_GETLENGTH) : v.send(SCI_GETLINEENDPOSITION, line);
	const char *pointer = reinterpret_cast<const char *>(v.send(SCI_GETRANGEPOINTER, from, to - from));
	if (!pointer) return;
	const std::string text(pointer, static_cast<size_t>(to - from));
	const size_t caret = static_cast<size_t>(position - from);
	// Nested pairs count, when the two markers differ.
	size_t start = std::string::npos;
	int depth = 0;
	for (size_t i = caret; i-- > 0;) {
		if (open != close && text.compare(i, close.size(), close) == 0) depth++;
		if (text.compare(i, open.size(), open) == 0) {
			if (depth == 0) {
				start = i + open.size();
				break;
			}
			depth--;
		}
	}
	if (start == std::string::npos) return;
	depth = 0;
	for (size_t i = caret; i + close.size() <= text.size(); i++) {
		if (open != close && text.compare(i, open.size(), open) == 0) depth++;
		if (text.compare(i, close.size(), close) == 0) {
			if (depth == 0) {
				v.send(SCI_SETSEL, from + static_cast<long>(start), from + static_cast<long>(i));
				return;
			}
			depth--;
		}
	}
}

void Workspace::updateLinks(int pane) {
	SciView &v = view(pane);
	const bool enabled = settings.prefOn("links") && (!isLargeDocument() || settings.prefOn("largeLinks"));
	const long first = v.send(SCI_DOCLINEFROMVISIBLE, v.send(SCI_GETFIRSTVISIBLELINE));
	const long last = std::min<long>(first + v.send(SCI_LINESONSCREEN) + 1, v.send(SCI_GETLINECOUNT) - 1);
	const long start = v.send(SCI_POSITIONFROMLINE, first);
	const long end = v.send(SCI_GETLINEENDPOSITION, last);
	v.send(SCI_SETINDICATORCURRENT, kIndicatorLink);
	v.send(SCI_INDICATORCLEARRANGE, start, end - start);
	if (!enabled || end <= start) return;
	const char *pointer = reinterpret_cast<const char *>(v.send(SCI_GETRANGEPOINTER, start, end - start));
	if (!pointer) return;
	const std::string text(pointer, static_cast<size_t>(end - start));
	std::vector<std::string> schemes;
	std::istringstream words(settings.pref("uriSchemes"));
	std::string word;
	while (words >> word) schemes.push_back(word);
	const auto endOfLink = [](const std::string &value, size_t from) {
		size_t at = from;
		while (at < value.size()) {
			const unsigned char c = static_cast<unsigned char>(value[at]);
			if (c <= ' ' || c == '"' || c == '<' || c == '>' || c == '`' || c == '\\' || c == '{' || c == '}' || c == '|' || c == '^') break;
			at++;
		}
		while (at > from && std::string(".,;:!?)]'").find(value[at - 1]) != std::string::npos) at--;
		return at;
	};
	for (size_t at = 0; at < text.size(); at++) {
		if (at > 0 && (std::isalnum(static_cast<unsigned char>(text[at - 1])) || text[at - 1] == '/')) continue;
		for (const std::string &scheme : schemes) {
			if (text.compare(at, scheme.size(), scheme) != 0) continue;
			const size_t linkEnd = endOfLink(text, at + scheme.size());
			if (linkEnd > at + scheme.size()) {
				v.send(SCI_INDICATORFILLRANGE, start + static_cast<long>(at), static_cast<long>(linkEnd - at));
				at = linkEnd;
			}
			break;
		}
	}
}

bool Workspace::openLinkAt(int pane, long position) {
	SciView &v = view(pane);
	if (!v.send(SCI_INDICATORVALUEAT, kIndicatorLink, position)) return false;
	const long start = v.send(SCI_INDICATORSTART, kIndicatorLink, position);
	const long end = v.send(SCI_INDICATOREND, kIndicatorLink, position);
	const char *pointer = reinterpret_cast<const char *>(v.send(SCI_GETRANGEPOINTER, start, end - start));
	if (!pointer || end <= start) return false;
	openExternally(std::string(pointer, static_cast<size_t>(end - start)));
	return true;
}

void Workspace::updateMatchingTags(int pane) {
	SciView &v = view(pane);
	const long length = v.send(SCI_GETLENGTH);
	v.send(SCI_SETINDICATORCURRENT, kIndicatorTag);
	v.send(SCI_INDICATORCLEARRANGE, 0, length);
	v.send(SCI_SETINDICATORCURRENT, kIndicatorTagAttribute);
	v.send(SCI_INDICATORCLEARRANGE, 0, length);
	const Document *document = paneAt(pane).active >= 0 ? paneAt(pane).tabs[paneAt(pane).active].document.get() : nullptr;
	if (!document || !settings.prefOn("matchTags") || isLargeDocument()) return;
	const std::string &language = document->language->name;
	if (language != "html" && language != "xml" && language != "php" && language != "asp" && language != "jsp") return;
	const bool paintZones = settings.prefOn("matchTagComment");
	// Looks at most this far each way, so a huge file doesn't slow typing.
	const long caret = v.send(SCI_GETCURRENTPOS);
	const long windowStart = std::max(0L, caret - 256 * 1024), windowEnd = std::min(length, caret + 256 * 1024);
	const char *pointer = reinterpret_cast<const char *>(v.send(SCI_GETRANGEPOINTER, windowStart, windowEnd - windowStart));
	if (!pointer) return;
	const std::string text(pointer, static_cast<size_t>(windowEnd - windowStart));
	const size_t at = static_cast<size_t>(caret - windowStart);

	// Inside a comment, a PHP block or an ASP block, the zone's own markers are the pair to show.
	if (paintZones) {
		static const struct { const char *open, *close; } zones[] = {{"<!--", "-->"}, {"<?", "?>"}, {"<%", "%>"}};
		for (const auto &zone : zones) {
			const size_t lookFrom = at + std::strlen(zone.open) - 1 < text.size() ? at + std::strlen(zone.open) - 1 : text.size();
			const size_t open = text.rfind(zone.open, lookFrom);
			if (open == std::string::npos) continue;
			const size_t close = text.find(zone.close, open + std::strlen(zone.open));
			if (close == std::string::npos || close + std::strlen(zone.close) < at) continue;
			v.send(SCI_SETINDICATORCURRENT, kIndicatorTag);
			v.send(SCI_INDICATORFILLRANGE, windowStart + static_cast<long>(open), static_cast<long>(std::strlen(zone.open)));
			v.send(SCI_INDICATORFILLRANGE, windowStart + static_cast<long>(close), static_cast<long>(std::strlen(zone.close)));
			return;
		}
	}

	struct TagSpan { size_t start, end; std::string name; bool closing, selfClosing; };
	std::vector<TagSpan> tags;
	for (size_t i = 0; i < text.size();) {
		const size_t open = text.find('<', i);
		if (open == std::string::npos) break;
		if (text.compare(open, 4, "<!--") == 0) {
			const size_t comment = text.find("-->", open);
			i = comment == std::string::npos ? text.size() : comment + 3;
			continue;
		}
		size_t nameStart = open + 1;
		const bool closing = nameStart < text.size() && text[nameStart] == '/';
		if (closing) nameStart++;
		size_t nameEnd = nameStart;
		while (nameEnd < text.size() && (std::isalnum(static_cast<unsigned char>(text[nameEnd])) || text[nameEnd] == '-' || text[nameEnd] == ':' || text[nameEnd] == '_')) nameEnd++;
		if (nameEnd == nameStart) {
			i = open + 1;
			continue;
		}
		char quote = 0;
		size_t close = nameEnd;
		for (; close < text.size(); close++) {
			const char c = text[close];
			if (quote) {
				if (c == quote) quote = 0;
			} else if (c == '"' || c == '\'') {
				quote = c;
			} else if (c == '>') {
				break;
			}
		}
		if (close >= text.size()) break;
		tags.push_back({open, close + 1, text.substr(nameStart, nameEnd - nameStart), closing, text[close - 1] == '/'});
		i = close + 1;
	}
	size_t current = tags.size();
	for (size_t i = 0; i < tags.size(); i++)
		if (at >= tags[i].start && at <= tags[i].end) current = i;
	if (current == tags.size()) {
		// In a tag's content: the nearest enclosing open tag counts.
		int depth = 0;
		for (size_t i = tags.size(); i-- > 0;) {
			if (tags[i].end > at || tags[i].selfClosing) continue;
			if (tags[i].closing) {
				depth++;
			} else if (depth == 0) {
				current = i;
				break;
			} else {
				depth--;
			}
		}
		if (current == tags.size()) return;
	}
	const TagSpan &tag = tags[current];
	if (tag.selfClosing) return;
	size_t match = tags.size();
	int depth = 0;
	if (tag.closing) {
		for (size_t i = current; i-- > 0;) {
			if (tags[i].name != tag.name || tags[i].selfClosing) continue;
			if (tags[i].closing) depth++;
			else if (depth == 0) { match = i; break; }
			else depth--;
		}
	} else {
		for (size_t i = current + 1; i < tags.size(); i++) {
			if (tags[i].name != tag.name || tags[i].selfClosing) continue;
			if (!tags[i].closing) depth++;
			else if (depth == 0) { match = i; break; }
			else depth--;
		}
	}
	if (match == tags.size()) return;
	const auto paint = [&](const TagSpan &span) {
		v.send(SCI_SETINDICATORCURRENT, kIndicatorTag);
		const size_t nameAt = span.start + (span.closing ? 2 : 1);
		v.send(SCI_INDICATORFILLRANGE, windowStart + static_cast<long>(span.start), static_cast<long>(nameAt + span.name.size() - span.start));
		v.send(SCI_INDICATORFILLRANGE, windowStart + static_cast<long>(span.end) - 1, 1);
		if (settings.prefOn("matchTagAttributes") && span.end > nameAt + span.name.size() + 1) {
			v.send(SCI_SETINDICATORCURRENT, kIndicatorTagAttribute);
			v.send(SCI_INDICATORFILLRANGE, windowStart + static_cast<long>(nameAt + span.name.size()), static_cast<long>(span.end - 1 - nameAt - span.name.size()));
		}
	};
	paint(tag);
	paint(tags[match]);
}

void Workspace::backupBeforeSave(const std::string &path) {
	const std::string mode = settings.pref("backupOnSave");
	if (mode == "none") return;
	std::error_code error;
	if (!fs::exists(path, error)) return;
	std::string directory = settings.pref("backupDir");
	if (directory.empty()) directory = mode == "verbose" ? directoryOf(path) + "/nppBackup" : directoryOf(path);
	fs::create_directories(directory, error);
	std::string target = (fs::path(directory) / baseName(path)).string();
	if (mode == "verbose") target += "." + formatDateTime("yyyy-MM-dd_HHmmss", time(nullptr));
	target += ".bak";
	fs::copy_file(path, target, fs::copy_options::overwrite_existing, error);
	if (error) host.flash("The backup could not be written: " + error.message());
}

}  // namespace omapad
