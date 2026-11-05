// The Linux window around the shared Workspace, using GTK3 (Scintilla's GTK port).
// Built for Omarchy on Hyprland, and works on any GTK3 desktop.
#include <gtk/gtk.h>

#include "Scintilla.h"
#include "ScintillaWidget.h"
#include "Workspace.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace omapad;
namespace fs = std::filesystem;

namespace {

const char *const kAppId = "local.omapad.editor";
const char *const kItemKey = "omapad-item";

// Connects a signal to a C++ function. The swapped connection passes only our data, so one
// handler fits any signal whose return value doesn't matter.
void onSignal(gpointer object, const char *signal, std::function<void()> handler) {
	auto *heap = new std::function<void()>(std::move(handler));
	g_signal_connect_data(object, signal,
		G_CALLBACK(+[](gpointer data) { (*static_cast<std::function<void()> *>(data))(); }), heap,
		+[](gpointer data, GClosure *) { delete static_cast<std::function<void()> *>(data); },
		G_CONNECT_SWAPPED);
}

struct GtkSciView : SciView {
	GtkWidget *widget = nullptr;
	intptr_t send(unsigned message, uintptr_t w, intptr_t l) override {
		return scintilla_send_message(SCINTILLA(widget), message, w, l);
	}
};

std::string pathFromFile(GFile *file) {
	char *path = g_file_get_path(file);
	std::string out = path ? path : "";
	g_free(path);
	return out;
}

std::string homeSubdir(const char *variable, const char *fallback) {
	const char *value = g_getenv(variable);
	if (value && value[0] == '/') return std::string(value) + "/omapad";
	return std::string(g_get_home_dir()) + "/" + fallback + "/omapad";
}

std::string findResources() {
	const char *overridePath = g_getenv("OMAPAD_RESOURCES");
	if (overridePath) return overridePath;
	std::error_code error;
	const fs::path exe = fs::read_symlink("/proc/self/exe", error);
	if (!error) {
		for (const fs::path &candidate : {exe.parent_path().parent_path() / "share" / "omapad", exe.parent_path() / "resources", exe.parent_path().parent_path() / "resources"})
			if (fs::exists(candidate / "langs.model.xml", error)) return candidate.string();
	}
	return "/usr/share/omapad";
}

enum FolderColumn { ColName, ColPath, ColIsDir, ColIsRoot, ColIcon, ColPopulated, ColCount };
enum FunctionColumn { FnName, FnIndex, FnCount };
enum ShortcutColumn { ScCommand, ScShortcut, ScId, ScWeight, ScCount };

// The keyval for the key part of a shortcut like "Mod+Shift+F", or 0.
guint keyvalFromShortcutKey(const std::string &name) {
	static const std::map<std::string, guint> named = {
		{"Space", GDK_KEY_space}, {"PageUp", GDK_KEY_Page_Up}, {"PageDown", GDK_KEY_Page_Down},
		{"/", GDK_KEY_slash}, {"=", GDK_KEY_equal}, {"-", GDK_KEY_minus}, {"+", GDK_KEY_plus},
		{"[", GDK_KEY_bracketleft}, {"]", GDK_KEY_bracketright}, {",", GDK_KEY_comma}, {".", GDK_KEY_period},
		{";", GDK_KEY_semicolon}, {"'", GDK_KEY_apostrophe}, {"\\", GDK_KEY_backslash}, {"`", GDK_KEY_grave},
	};
	const auto found = named.find(name);
	if (found != named.end()) return found->second;
	if (name.size() == 1) {
		const char lower = g_ascii_tolower(name[0]);
		return gdk_unicode_to_keyval(static_cast<guchar>(lower));
	}
	const guint key = gdk_keyval_from_name(name.c_str());
	return key == GDK_KEY_VoidSymbol ? 0 : key;
}

// The event as a shortcut, or empty if it isn't usable as one. Reads the key as if Shift
// weren't held, so Control+Shift+1 is that and not Control+!.
std::string shortcutFromEvent(const GdkEventKey *event) {
	if (event->is_modifier) return {};
	guint keyval = event->keyval;
	gdk_keymap_translate_keyboard_state(gdk_keymap_get_for_display(gdk_display_get_default()), event->hardware_keycode,
	                                    GdkModifierType(0), event->group, &keyval, nullptr, nullptr, nullptr);
	static const std::map<guint, const char *> named = {
		{GDK_KEY_space, "Space"}, {GDK_KEY_Return, "Return"}, {GDK_KEY_Tab, "Tab"}, {GDK_KEY_BackSpace, "BackSpace"},
		{GDK_KEY_Delete, "Delete"}, {GDK_KEY_Home, "Home"}, {GDK_KEY_End, "End"}, {GDK_KEY_Up, "Up"}, {GDK_KEY_Down, "Down"},
		{GDK_KEY_Left, "Left"}, {GDK_KEY_Right, "Right"}, {GDK_KEY_Page_Up, "PageUp"}, {GDK_KEY_Page_Down, "PageDown"},
	};
	std::string key;
	const auto found = named.find(keyval);
	const bool functionKey = keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F35;
	if (found != named.end()) {
		key = found->second;
	} else if (functionKey) {
		key = "F" + std::to_string(keyval - GDK_KEY_F1 + 1);
	} else {
		const guint32 character = gdk_keyval_to_unicode(keyval);
		if (character < 0x21 || character > 0x7E) return {};  // Only keys every layout can name.
		key = std::string(1, g_ascii_toupper(static_cast<char>(character)));
	}
	std::string out;
	if (event->state & GDK_CONTROL_MASK) out += "Mod+";
	if (event->state & GDK_MOD1_MASK) out += "Alt+";
	// Without Control or Alt, a key types text, unless it's a function key.
	if (out.empty() && !functionKey) return {};
	if (event->state & GDK_SHIFT_MASK) out += "Shift+";
	return out + key;
}

bool isCheckable(Cmd cmd) {
	static const std::set<Cmd> checkable = {
		Cmd::AlwaysOnTop, Cmd::FullScreen, Cmd::ToggleSpace, Cmd::ToggleEol, Cmd::ToggleAllCharacters, Cmd::ToggleGuides,
		Cmd::ToggleWrapSymbol, Cmd::ToggleWrap, Cmd::ToggleResults, Cmd::RotateSplit, Cmd::SyncVertical, Cmd::SyncHorizontal,
		Cmd::ToggleFolderPanel, Cmd::ToggleHiddenInFolders, Cmd::EolConvert, Cmd::ReopenEncoding, Cmd::ConvertEncoding,
		Cmd::SetLanguage, Cmd::SetTheme, Cmd::SetTabWidth, Cmd::ToggleInsertSpaces, Cmd::SelectDocument,
		Cmd::ToggleFunctionList, Cmd::ToggleDocumentMap, Cmd::ToggleReadOnly,
		Cmd::TogglePanel, Cmd::PostIt, Cmd::DistractionFree, Cmd::ToggleNonPrinting, Cmd::ToggleControlChars, Cmd::ToggleZoomSync,
		Cmd::ToggleMonitoring, Cmd::TextDirection, Cmd::TabColour, Cmd::PinActive, Cmd::FileReadOnly,
		Cmd::NotesMode, Cmd::NotesFocus, Cmd::NotesView,
	};
	return checkable.count(cmd) != 0;
}

}  // namespace

class App : public Host {
public:
	explicit App(GtkApplication *application) : application(application) {}

	void build();
	void openPaths(const std::vector<std::string> &paths);
	void present();

	// Host.
	SciView &paneView(int pane) override { return panes[pane].sci; }
	SciView &resultsView() override { return results; }
	SciView &backgroundView() override { return background; }
	SciView &printView() override { return printer; }
	SciView &mapView() override { return map; }
	int ask(const std::string &title, const std::string &detail, const std::vector<std::string> &buttons) override;
	bool chooseFilesToOpen(const std::string &start, std::vector<std::string> &out) override;
	bool chooseSavePath(const std::string &name, const std::string &start, std::string &out) override;
	bool chooseFolder(const std::string &start, std::string &out) override;
	bool chooseFont(std::string &family, double &size) override;
	bool promptText(const std::string &title, const std::string &label, const std::string &initial, std::string &out) override;
	bool clipboardData(const std::string &format, std::string &out) override;
	void setClipboardData(const std::string &format, const std::string &data) override;
	void launch(const std::vector<std::string> &arguments, const std::string &directory) override {
		std::string error;
		if (!spawnDetached(arguments, directory, error)) flash(error);
	}
	int runList(const ListDialog &dialog, std::vector<int> &selection) override;
	void setPanelVisible(int panel, const std::string &title, bool visible) override;
	void panelChanged(int panel) override;
	void setIncrementalBar(bool visible) override;
	void setChrome(const ChromeState &chrome) override;
	void notesChanged(const NotesState &state) override;
	void setTabStyle(const TabStyle &style) override;
	void closeFind() override { if (findWindow) gtk_widget_hide(findWindow); }
	void setFindInSelection(bool on) override { if (inSelection) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(inSelection), on); }
	void beep() override { gdk_display_beep(gdk_display_get_default()); }
	void focusPanel(int panel) override;
	void selectFolderPath(const std::string &path) override;
	void setTray(bool) override {}  // GTK3 has no tray that works on Wayland without a separate library.
	void setWindowPosition(long x, long y) override;
	bool registerFileTypes(const std::vector<std::string> &extensions, std::string &error) override;
	void applyToolbarIcons();
	void buildTabShortcuts(int pane);
	void setClipboard(const std::string &text) override {
		gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), text.c_str(), static_cast<gint>(text.size()));
	}
	void reveal(const std::string &path) override;
	bool moveToTrash(const std::string &path, std::string &error) override;
	void tabsChanged() override;
	void statusChanged(const std::string &left, const std::string &right) override {
		gtk_label_set_text(GTK_LABEL(statusLeft), left.c_str());
		gtk_label_set_text(GTK_LABEL(statusRight), right.c_str());
	}
	void flash(const std::string &message) override;
	void splitChanged(bool visible, bool stacked) override;
	void setResultsVisible(bool visible) override;
	void setFolderPanelVisible(bool visible) override;
	void foldersChanged() override;
	void focusPane(int pane) override {
		if (notesPreview && notesUiState.enabled && notesUiState.viewMode == 2) gtk_widget_grab_focus(notesPreview);
		else if (panes[pane].sci.widget) gtk_widget_grab_focus(panes[pane].sci.widget);
	}
	void themeChanged() override;
	void menusChanged() override;
	bool runForm(const std::string &title, std::vector<FormField> &fields) override;
	bool print(PrintJob &job, const PrintOutput &output, std::string &error) override;
	void runShortcutMapper() override;
	void setDocumentMapVisible(bool visible) override;
	void setFunctionListVisible(bool visible) override;
	void functionListChanged() override;
	void showFind(FindTab tab, const std::string &prefill, const std::string &directory) override;
	void setFindText(const std::string &text) override { gtk_entry_set_text(whatEntry(), text.c_str()); }
	FindOptions currentFindOptions() override;
	void setFindStatus(const std::string &status, bool isError) override;
	void setFindInFilesRunning(bool running) override {
		gtk_widget_set_sensitive(findInFilesButton, !running);
		gtk_widget_set_sensitive(stopButton, running);
	}
	void setFullScreen(bool on) override {
		if (on) gtk_window_fullscreen(GTK_WINDOW(window));
		else gtk_window_unfullscreen(GTK_WINDOW(window));
	}
	void setAlwaysOnTop(bool on) override;
	bool systemIsDark() override;
	void post(std::function<void()> work) override {
		auto *heap = new std::function<void()>(std::move(work));
		g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, +[](gpointer data) -> gboolean {
			(*static_cast<std::function<void()> *>(data))();
			return G_SOURCE_REMOVE;
		}, heap, +[](gpointer data) { delete static_cast<std::function<void()> *>(data); });
	}
	void quit() override { tryQuit(); }

	bool tryQuit();
	std::unique_ptr<Workspace> workspace;

private:
	struct PaneWidgets {
		GtkSciView sci;
		GtkWidget *box = nullptr;
		GtkWidget *notebook = nullptr;
		bool syncing = false;
	};

	GtkWidget *makeScintilla(int pane);
	void buildPane(int pane);
	void syncNotebook(int pane);
	void addNotebookPage(int pane);
	void setTabLabel(int pane, int index, const TabInfo &info);
	GtkWidget *nativeItem(const MenuItem &item, GtkAccelGroup *accel, bool topLevel);
	GtkWidget *createMenuWidget(const MenuItem &item, GtkAccelGroup *accel, bool topLevel);
	GtkWidget *nativeMenu(const std::vector<MenuItem> &items, GtkAccelGroup *accel);
	void refreshMenu(GtkWidget *menu);
	void popup(const std::vector<MenuItem> &items);
	void buildMenuBar();
	void buildFolderPanel();
	void buildFunctionPanel();
	void buildDocumentMap();
	void buildToolbar();
	void buildNotes();
	void applyNotesLayout();
	void renderNotesPreview(const NotesState &state);
	void buildIncrementalBar();
	void runIncremental(bool backward, bool next);
	void syncFlowTabs(int pane);
	void arrangeSidePanels();
	void addFolderChildren(GtkTreeIter *parent, const std::string &path);
	void populateRow(GtkTreeIter *iter);
	void restoreExpansion(GtkTreeIter *iter);
	void buildFindWindow();
	void applyFindTab(FindTab tab);
	GtkEntry *whatEntry() const { return GTK_ENTRY(gtk_bin_get_child(GTK_BIN(what))); }
	void rememberHistory();
	void watchTheme();
	void watchFile(const std::string &path);
	void scheduleActivationCheck();

	GtkApplication *application;
	GtkWidget *window = nullptr;
	PaneWidgets panes[2];
	GtkSciView results, background, map, printer;
	GtkPrintSettings *printSettings = nullptr;
	GtkWidget *toolbar = nullptr, *statusBar = nullptr, *panelsBox = nullptr;
	std::vector<std::pair<GtkToolItem *, ToolItem>> toolButtons;
	GtkWidget *tabShortcuts[2] = {nullptr, nullptr};
	struct PanelWidgets { std::string signature; int columnCount = -1; GtkWidget *box = nullptr, *view = nullptr; GtkListStore *store = nullptr; } panelWidgets[kPanelCount];
	GtkWidget *incrementalBar = nullptr, *incrementalEntry = nullptr, *incrementalCase = nullptr, *incrementalAll = nullptr, *incrementalStatus = nullptr;
	GtkWidget *markButtons = nullptr, *markOptions = nullptr, *bookmarkLine = nullptr, *purgeMarks = nullptr;
	GtkWidget *flowTabs[2] = {nullptr, nullptr};
	NotesState notesUiState;
	bool syncingNotes = false, notesSavedResults = false, notesSavedIncremental = false;
	std::vector<int> notesSavedPositions;
	std::string notesTagSignature, notesListSignature, notesPreviewSignature, notesPreviewPath;
	GtkWidget *notesBar = nullptr, *notesSidebar = nullptr, *notesRootPaned = nullptr, *notesContentPaned = nullptr;
	GtkWidget *notesSearch = nullptr, *notesTagView = nullptr, *notesList = nullptr, *notesListScroll = nullptr;
	GtkListStore *notesTagStore = nullptr, *notesStore = nullptr;
	GtkWidget *notesTitle = nullptr, *notesFolderLabel = nullptr, *notesEmpty = nullptr, *notesSaveBox = nullptr;
	GtkWidget *notesSaveStatus = nullptr, *notesRetry = nullptr, *notesFocusButton = nullptr, *notesFormatButton = nullptr;
	GtkWidget *notesBarStatus = nullptr, *notesBarRetry = nullptr;
	GtkWidget *notesPreview = nullptr, *notesPreviewScroll = nullptr, *notesViewButtons[3] = {nullptr, nullptr, nullptr};
	ChromeState chromeState;
	TabStyle tabStyle;
	std::string heldClipboard[2];
	bool menuTemporarilyShown = false;
	GtkWidget *editorPaned = nullptr, *rightPaned = nullptr, *outerPaned = nullptr, *sidePaned = nullptr, *sideBox = nullptr;
	GtkWidget *menubar = nullptr;
	GtkAccelGroup *accelGroup = nullptr;
	GtkWidget *statusLeft = nullptr, *statusRight = nullptr, *statusMessage = nullptr;
	guint flashTimer = 0, activationTimer = 0, themeTimer = 0;
	int dialogDepth = 0;
	bool settingMenus = false;
	bool splitPositioned = false;
	bool reloadingFolders = false;

	GtkWidget *folderBox = nullptr, *folderView = nullptr;
	GtkTreeStore *folderStore = nullptr;

	GtkWidget *functionBox = nullptr, *functionView = nullptr, *functionFilter = nullptr, *functionEmpty = nullptr, *functionSort = nullptr;
	GtkTreeStore *functionStore = nullptr;
	std::set<std::string> collapsedClasses;
	bool reloadingFunctions = false;

	GtkWidget *findWindow = nullptr, *findTabs[4] = {nullptr, nullptr, nullptr, nullptr};
	GtkWidget *what = nullptr, *with = nullptr, *filters = nullptr, *directory = nullptr;
	GtkWidget *matchCase = nullptr, *wholeWord = nullptr, *wrap = nullptr, *backward = nullptr, *inSelection = nullptr;
	GtkWidget *recursive = nullptr, *hidden = nullptr, *mode = nullptr, *findStatus = nullptr, *dotNewline = nullptr;
	GtkWidget *withRow = nullptr, *filtersRow = nullptr, *directoryRow = nullptr, *filesOptions = nullptr, *selectionRow = nullptr;
	GtkWidget *findButtons = nullptr, *replaceButtons = nullptr, *filesButtons = nullptr;
	GtkWidget *findInFilesButton = nullptr, *stopButton = nullptr;
	std::vector<std::string> whatHistory, withHistory;
	FindTab findTab = FindTab::Find;

	GtkCssProvider *themeCss = nullptr;
	std::vector<GFileMonitor *> themeMonitors;
};

// The options that came with this launch, like -n12.
static CommandLine gCommandLine;

#pragma mark - Editors and tabs

GtkWidget *App::makeScintilla(int pane) {
	GtkWidget *widget = scintilla_new();
	struct Binding { App *app; int pane; };
	g_signal_connect_data(widget, SCINTILLA_NOTIFY,
		G_CALLBACK(+[](GtkWidget *, gint, SCNotification *n, gpointer data) {
			auto *binding = static_cast<Binding *>(data);
			if (binding->app->workspace) binding->app->workspace->editorNotification(binding->pane, *n);
		}),
		new Binding{this, pane}, +[](gpointer data, GClosure *) { delete static_cast<Binding *>(data); }, GConnectFlags(0));

	// The context menu comes from the core, so it can be edited.
	struct MenuBinding { App *app; int pane; };
	g_signal_connect_data(widget, "button-press-event",
		G_CALLBACK(+[](GtkWidget *, GdkEventButton *event, gpointer data) -> gboolean {
			auto *b = static_cast<MenuBinding *>(data);
			if (event->button != GDK_BUTTON_SECONDARY || !b->app->workspace) return FALSE;
			const long position = b->app->panes[b->pane].sci.send(SCI_POSITIONFROMPOINT, static_cast<uintptr_t>(event->x), static_cast<intptr_t>(event->y));
			b->app->workspace->editorRightClicked(b->pane, position);
			b->app->popup(b->app->workspace->editorContextMenu());
			return TRUE;
		}),
		new MenuBinding{this, pane}, +[](gpointer data, GClosure *) { delete static_cast<MenuBinding *>(data); }, GConnectFlags(0));

	// Dropping files on the editor opens them. Dropping text still inserts it.
	g_signal_connect(widget, "drag-data-received",
		G_CALLBACK(+[](GtkWidget *w, GdkDragContext *context, gint, gint, GtkSelectionData *data, guint, guint time, gpointer self) {
			gchar **uris = gtk_selection_data_get_uris(data);
			if (!uris) return;
			std::vector<std::string> paths;
			for (gchar **uri = uris; *uri; uri++) {
				char *path = g_filename_from_uri(*uri, nullptr, nullptr);
				if (path) paths.emplace_back(path);
				g_free(path);
			}
			g_strfreev(uris);
			if (paths.empty()) return;
			g_signal_stop_emission_by_name(w, "drag-data-received");
			gtk_drag_finish(context, TRUE, FALSE, time);
			static_cast<App *>(self)->openPaths(paths);
		}), this);
	return widget;
}

void App::buildPane(int pane) {
	PaneWidgets &p = panes[pane];
	p.box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	p.notebook = gtk_notebook_new();
	GtkNotebook *notebook = GTK_NOTEBOOK(p.notebook);
	gtk_notebook_set_scrollable(notebook, TRUE);
	gtk_notebook_set_show_border(notebook, FALSE);
	gtk_widget_set_can_focus(p.notebook, FALSE);
	gtk_box_pack_start(GTK_BOX(p.box), p.notebook, FALSE, FALSE, 0);
	p.sci.widget = makeScintilla(pane);
	gtk_box_pack_start(GTK_BOX(p.box), p.sci.widget, TRUE, TRUE, 0);

	struct Binding { App *app; int pane; };
	auto *binding = new Binding{this, pane};
	g_signal_connect(p.notebook, "switch-page",
		G_CALLBACK(+[](GtkNotebook *, GtkWidget *, guint number, gpointer data) {
			auto *b = static_cast<Binding *>(data);
			if (!b->app->panes[b->pane].syncing && b->app->workspace) b->app->workspace->clickTab(b->pane, static_cast<int>(number));
		}), binding);
	g_signal_connect(p.notebook, "page-reordered",
		G_CALLBACK(+[](GtkNotebook *nb, GtkWidget *, guint number, gpointer data) {
			auto *b = static_cast<Binding *>(data);
			if (b->app->panes[b->pane].syncing || !b->app->workspace) return;
			// GTK has moved its page already. Work out where it came from by the active tab.
			const int from = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(nb), "omapad-drag-from"));
			b->app->workspace->moveTab(b->pane, from, static_cast<int>(number));
		}), binding);
	buildTabShortcuts(pane);
}

// Notepad++'s new, list and close buttons, at the end of the tab bar.
void App::buildTabShortcuts(int pane) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	const auto add = [&](const char *icon, const char *tip, std::function<void()> action) {
		GtkWidget *button = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_MENU);
		gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
		gtk_widget_set_can_focus(button, FALSE);
		gtk_widget_set_tooltip_text(button, tip);
		onSignal(button, "clicked", std::move(action));
		gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
	};
	add("list-add-symbolic", "New document", [this]() { post([this]() { workspace->run(Cmd::New); }); });
	add("pan-down-symbolic", "List the open documents", [this]() { popup(workspace->dynamicMenu(MenuItem::Dynamic::Documents)); });
	add("window-close-symbolic", "Close this document", [this]() { post([this]() { workspace->run(Cmd::Close); }); });
	gtk_widget_show_all(box);
	gtk_notebook_set_action_widget(GTK_NOTEBOOK(panes[pane].notebook), box, GTK_PACK_END);
	tabShortcuts[pane] = box;
}

void App::addNotebookPage(int pane) {
	PaneWidgets &p = panes[pane];
	GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_size_request(page, -1, 0);

	GtkWidget *events = gtk_event_box_new();
	gtk_event_box_set_visible_window(GTK_EVENT_BOX(events), FALSE);
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *label = gtk_label_new("");
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
	gtk_label_set_max_width_chars(GTK_LABEL(label), 28);
	GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
	gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
	gtk_widget_set_can_focus(close, FALSE);
	gtk_style_context_add_class(gtk_widget_get_style_context(close), "omapad-tab-close");
	GtkWidget *pin = gtk_button_new_with_label("⚲");
	gtk_button_set_relief(GTK_BUTTON(pin), GTK_RELIEF_NONE);
	gtk_widget_set_can_focus(pin, FALSE);
	gtk_widget_set_tooltip_text(pin, "Pin or unpin this tab");
	gtk_style_context_add_class(gtk_widget_get_style_context(pin), "omapad-tab-close");
	gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(row), pin, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), close, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(events), row);
	gtk_widget_show_all(events);
	g_object_set_data(G_OBJECT(events), "omapad-pin", pin);
	onSignal(pin, "clicked", [this, pane, page]() {
		const int index = gtk_notebook_page_num(GTK_NOTEBOOK(panes[pane].notebook), page);
		post([this, pane, index]() { if (index >= 0) workspace->run(Cmd::TabPin, pane * 10000 + index); });
	});
	g_object_set_data(G_OBJECT(events), "omapad-label", label);
	g_object_set_data(G_OBJECT(events), "omapad-close", close);
	g_object_set_data(G_OBJECT(page), "omapad-pane", GINT_TO_POINTER(pane));

	onSignal(close, "clicked", [this, pane, page]() {
		const int index = gtk_notebook_page_num(GTK_NOTEBOOK(panes[pane].notebook), page);
		// Closing rebuilds the tabs, so it can't happen inside this button's own click.
		post([this, pane, page, index]() {
			(void)page;
			if (index >= 0) workspace->closeTab(pane, index);
		});
	});
	struct Binding { App *app; int pane; GtkWidget *page; };
	g_signal_connect_data(events, "enter-notify-event", G_CALLBACK(+[](GtkWidget *, GdkEventCrossing *, gpointer data) -> gboolean {
		auto *b = static_cast<Binding *>(data);
		b->app->workspace->tabHovered(b->pane, gtk_notebook_page_num(GTK_NOTEBOOK(b->app->panes[b->pane].notebook), b->page));
		return FALSE;
	}), new Binding{this, pane, page}, +[](gpointer data, GClosure *) { delete static_cast<Binding *>(data); }, GConnectFlags(0));
	g_signal_connect_data(events, "leave-notify-event", G_CALLBACK(+[](GtkWidget *, GdkEventCrossing *, gpointer data) -> gboolean {
		auto *b = static_cast<Binding *>(data);
		b->app->workspace->tabHovered(b->pane, -1);
		return FALSE;
	}), new Binding{this, pane, page}, +[](gpointer data, GClosure *) { delete static_cast<Binding *>(data); }, GConnectFlags(0));
	g_signal_connect_data(events, "button-press-event",
		G_CALLBACK(+[](GtkWidget *, GdkEventButton *event, gpointer data) -> gboolean {
			auto *b = static_cast<Binding *>(data);
			GtkNotebook *nb = GTK_NOTEBOOK(b->app->panes[b->pane].notebook);
			const int index = gtk_notebook_page_num(nb, b->page);
			if (index < 0) return FALSE;
			if (event->button == GDK_BUTTON_SECONDARY) {
				b->app->popup(b->app->workspace->tabMenu(b->pane, index));
				return TRUE;
			}
			if (event->type == GDK_2BUTTON_PRESS && event->button == GDK_BUTTON_PRIMARY) {
				App *app = b->app;
				const int pane = b->pane;
				app->post([app, pane, index]() { app->workspace->tabDoubleClicked(pane, index); });
				return TRUE;
			}
			if (event->button == GDK_BUTTON_MIDDLE) {
				App *app = b->app;
				const int pane = b->pane;
				app->post([app, pane, index]() { app->workspace->closeTab(pane, index); });
				return TRUE;
			}
			g_object_set_data(G_OBJECT(nb), "omapad-drag-from", GINT_TO_POINTER(index));
			// A click on a tab in the other view moves focus there, even if it's already showing.
			b->app->workspace->paneFocused(b->pane);
			b->app->focusPane(b->pane);
			return FALSE;
		}),
		new Binding{this, pane, page}, +[](gpointer data, GClosure *) { delete static_cast<Binding *>(data); }, GConnectFlags(0));

	gtk_widget_show(page);
	const gboolean wasSyncing = p.syncing;
	p.syncing = true;
	gtk_notebook_append_page(GTK_NOTEBOOK(p.notebook), page, events);
	gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(p.notebook), page, TRUE);
	p.syncing = wasSyncing;
}

void App::setTabLabel(int pane, int index, const TabInfo &info) {
	GtkNotebook *notebook = GTK_NOTEBOOK(panes[pane].notebook);
	GtkWidget *page = gtk_notebook_get_nth_page(notebook, index);
	GtkWidget *events = gtk_notebook_get_tab_label(notebook, page);
	GtkWidget *label = GTK_WIDGET(g_object_get_data(G_OBJECT(events), "omapad-label"));
	const std::string title = tabDisplayTitle(info, tabStyle);
	gtk_label_set_text(GTK_LABEL(label), title.c_str());
	const bool buttonsShown = tabStyle.buttonsOnInactive || workspace->activeTab(pane) == index;
	GtkWidget *closeButton = GTK_WIDGET(g_object_get_data(G_OBJECT(events), "omapad-close"));
	if (closeButton) gtk_widget_set_visible(closeButton, tabStyle.closeButton && !info.pinned && buttonsShown);
	GtkWidget *pinButton = GTK_WIDGET(g_object_get_data(G_OBJECT(events), "omapad-pin"));
	if (pinButton) gtk_widget_set_visible(pinButton, tabStyle.pinButton && buttonsShown);
	GtkStyleContext *eventsStyle = gtk_widget_get_style_context(gtk_bin_get_child(GTK_BIN(events)));
	for (int colour = 1; colour <= 5; colour++) {
		const std::string name = "omapad-tab-colour-" + std::to_string(colour);
		if (colour == info.colour) gtk_style_context_add_class(eventsStyle, name.c_str());
		else gtk_style_context_remove_class(eventsStyle, name.c_str());
	}
	gtk_notebook_set_tab_reorderable(notebook, page, !tabStyle.locked);
	// An ellipsized label asks for almost no width, and a scrolling notebook gives it just that.
	// Marks and emoji are wider than the average character it counts in.
	gtk_label_set_width_chars(GTK_LABEL(label), static_cast<gint>(std::min<glong>(g_utf8_strlen(title.c_str(), -1) + 2, 30)));
	gtk_widget_set_tooltip_text(events, info.toolTip.c_str());
}

void App::syncNotebook(int pane) {
	PaneWidgets &p = panes[pane];
	GtkNotebook *notebook = GTK_NOTEBOOK(p.notebook);
	const std::vector<TabInfo> tabs = workspace->tabs(pane);
	p.syncing = true;
	while (gtk_notebook_get_n_pages(notebook) > static_cast<int>(tabs.size())) gtk_notebook_remove_page(notebook, -1);
	while (gtk_notebook_get_n_pages(notebook) < static_cast<int>(tabs.size())) addNotebookPage(pane);
	for (int i = 0; i < static_cast<int>(tabs.size()); i++) setTabLabel(pane, i, tabs[i]);
	const int active = workspace->activeTab(pane);
	if (active >= 0 && gtk_notebook_get_current_page(notebook) != active) gtk_notebook_set_current_page(notebook, active);
	p.syncing = false;
	syncFlowTabs(pane);
	GtkStyleContext *style = gtk_widget_get_style_context(p.notebook);
	if (workspace->focusedPane() == pane) gtk_style_context_remove_class(style, "omapad-unfocused");
	else gtk_style_context_add_class(style, "omapad-unfocused");
}

void App::tabsChanged() {
	if (!window) return;
	syncNotebook(0);
	syncNotebook(1);
	const std::string title = workspace->windowTitle();
	const std::string shown = (workspace->windowDirty() ? "● " : "") + title + (title.empty() ? "omapad" : " - omapad");
	gtk_window_set_title(GTK_WINDOW(window), shown.c_str());
	applyNotesLayout();
}

void App::splitChanged(bool visible, bool stacked) {
	gtk_orientable_set_orientation(GTK_ORIENTABLE(editorPaned), stacked ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL);
	gtk_widget_set_visible(panes[1].box, visible);
	if (visible) gtk_widget_show_all(panes[1].box);
	GtkAllocation allocation;
	gtk_widget_get_allocation(editorPaned, &allocation);
	const int size = stacked ? allocation.height : allocation.width;
	if (visible && size > 1) gtk_paned_set_position(GTK_PANED(editorPaned), size / 2);
	splitPositioned = visible && size > 1;
	applyNotesLayout();
}

void App::setResultsVisible(bool visible) {
	if (notesUiState.enabled) visible = false;
	gtk_widget_set_visible(results.widget, visible);
	if (visible) {
		GtkAllocation allocation;
		gtk_widget_get_allocation(rightPaned, &allocation);
		if (allocation.height > 300) gtk_paned_set_position(GTK_PANED(rightPaned), allocation.height - 200);
	}
}

#pragma mark - Menus

GtkWidget *App::nativeMenu(const std::vector<MenuItem> &items, GtkAccelGroup *accel) {
	GtkWidget *menu = gtk_menu_new();
	if (accel) gtk_menu_set_accel_group(GTK_MENU(menu), accel);
	for (const MenuItem &item : items) gtk_menu_shell_append(GTK_MENU_SHELL(menu), nativeItem(item, accel, false));
	onSignal(menu, "show", [this, menu]() { refreshMenu(menu); });
	return menu;
}

GtkWidget *App::nativeItem(const MenuItem &item, GtkAccelGroup *accel, bool topLevel) {
	if (item.separator) return gtk_separator_menu_item_new();
	GtkWidget *widget = createMenuWidget(item, accel, topLevel);
	// Shortcuts have to keep working when the menu bar is hidden.
	g_signal_connect(widget, "can-activate-accel", G_CALLBACK(+[](GtkWidget *w, guint, gpointer) -> gboolean { return gtk_widget_is_sensitive(w); }), nullptr);
	return widget;
}

GtkWidget *App::createMenuWidget(const MenuItem &item, GtkAccelGroup *accel, bool topLevel) {
	GtkWidget *widget;
	if (topLevel) {
		const std::string title = "_" + item.title;
		widget = gtk_menu_item_new_with_mnemonic(title.c_str());
	} else if (isCheckable(item.cmd)) {
		widget = gtk_check_menu_item_new_with_label(item.title.c_str());
		if (item.cmd == Cmd::SetLanguage || item.cmd == Cmd::SetTheme || item.cmd == Cmd::EolConvert || item.cmd == Cmd::SetTabWidth ||
		    item.cmd == Cmd::ReopenEncoding || item.cmd == Cmd::ConvertEncoding || item.cmd == Cmd::SelectDocument || item.cmd == Cmd::NotesView)
			gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(widget), TRUE);
	} else {
		widget = gtk_menu_item_new_with_label(item.title.c_str());
	}
	if (item.dynamic != MenuItem::Dynamic::None) {
		GtkWidget *menu = gtk_menu_new();
		gtk_menu_item_set_submenu(GTK_MENU_ITEM(widget), menu);
		const MenuItem::Dynamic kind = item.dynamic;
		onSignal(menu, "show", [this, menu, kind]() {
			// Rebuilt each time it opens, since tabs and recent files change.
			GList *children = gtk_container_get_children(GTK_CONTAINER(menu));
			for (GList *child = children; child; child = child->next) gtk_widget_destroy(GTK_WIDGET(child->data));
			g_list_free(children);
			for (const MenuItem &entry : workspace->dynamicMenu(kind)) gtk_menu_shell_append(GTK_MENU_SHELL(menu), nativeItem(entry, nullptr, false));
			gtk_widget_show_all(menu);
			refreshMenu(menu);
		});
		// GTK won't open a submenu that has no items yet.
		GtkWidget *placeholder = gtk_menu_item_new_with_label("");
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), placeholder);
		return widget;
	}
	if (!item.children.empty()) {
		gtk_menu_item_set_submenu(GTK_MENU_ITEM(widget), nativeMenu(item.children, accel));
		return widget;
	}
	auto *copy = new MenuItem(item);
	g_object_set_data_full(G_OBJECT(widget), kItemKey, copy, +[](gpointer data) { delete static_cast<MenuItem *>(data); });
	onSignal(widget, "activate", [this, copy]() {
		if (settingMenus) return;
		// A menu's own click finishes first, because the command may rebuild tabs or menus.
		const MenuItem item = *copy;
		post([this, item]() { workspace->run(item.cmd, item.arg, item.text); });
	});

	if (!item.shortcut.empty()) {
		guint key = 0;
		GdkModifierType mods = GdkModifierType(0);
		std::string shortcut = item.shortcut;
		// "Mod++" means the plus key, which splitting on + would lose.
		if (shortcut.size() >= 2 && shortcut.compare(shortcut.size() - 2, 2, "++") == 0) {
			key = GDK_KEY_plus;
			shortcut.resize(shortcut.size() - 2);
		}
		gchar **parts = g_strsplit(shortcut.c_str(), "+", -1);
		for (gchar **part = parts; *part; part++) {
			const std::string name = *part;
			if (name == "Mod" || name == "Ctrl") mods = GdkModifierType(mods | GDK_CONTROL_MASK);
			else if (name == "Alt") mods = GdkModifierType(mods | GDK_MOD1_MASK);
			else if (name == "Shift") mods = GdkModifierType(mods | GDK_SHIFT_MASK);
			else if (!name.empty()) key = keyvalFromShortcutKey(name);
		}
		g_strfreev(parts);
		if (key != 0) {
			if (!item.macAction.empty() || !accel) {
				// Standard editing keys stay with whichever view has focus, so they work in the
				// results pane too. The menu only shows them. Omarchy's Super+C and Super+V arrive
				// as Ctrl+Insert and Shift+Insert, which Scintilla handles the same way.
				GtkWidget *child = gtk_bin_get_child(GTK_BIN(widget));
				if (GTK_IS_ACCEL_LABEL(child)) gtk_accel_label_set_accel(GTK_ACCEL_LABEL(child), key, mods);
			} else {
				gtk_widget_add_accelerator(widget, "activate", accel, key, mods, GTK_ACCEL_VISIBLE);
				// Menus only update when opened, so a greyed item mustn't block its shortcut.
				// Commands check for themselves whether they apply.
				g_signal_connect(widget, "can-activate-accel", G_CALLBACK(+[](GtkWidget *, guint, gpointer) -> gboolean { return TRUE; }), nullptr);
			}
		}
	}
	return widget;
}

void App::refreshMenu(GtkWidget *menu) {
	settingMenus = true;
	GList *children = gtk_container_get_children(GTK_CONTAINER(menu));
	for (GList *child = children; child; child = child->next) {
		GtkWidget *widget = GTK_WIDGET(child->data);
		auto *item = static_cast<MenuItem *>(g_object_get_data(G_OBJECT(widget), kItemKey));
		if (!item) continue;
		const MenuState state = workspace->stateOf(*item);
		gtk_widget_set_sensitive(widget, state.enabled && item->cmd != Cmd::None);
		if (GTK_IS_CHECK_MENU_ITEM(widget)) gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(widget), state.checked);
	}
	g_list_free(children);
	settingMenus = false;
}

void App::popup(const std::vector<MenuItem> &items) {
	GtkWidget *menu = nativeMenu(items, nullptr);
	gtk_widget_show_all(menu);
	g_signal_connect(menu, "selection-done", G_CALLBACK(+[](GtkWidget *m, gpointer) {
		g_idle_add(+[](gpointer data) -> gboolean { gtk_widget_destroy(GTK_WIDGET(data)); return G_SOURCE_REMOVE; }, m);
	}), nullptr);
	gtk_menu_popup_at_pointer(GTK_MENU(menu), nullptr);
}

void App::buildMenuBar() {
	if (accelGroup) gtk_window_remove_accel_group(GTK_WINDOW(window), accelGroup);
	accelGroup = gtk_accel_group_new();
	gtk_window_add_accel_group(GTK_WINDOW(window), accelGroup);
	GList *children = gtk_container_get_children(GTK_CONTAINER(menubar));
	for (GList *child = children; child; child = child->next) gtk_widget_destroy(GTK_WIDGET(child->data));
	g_list_free(children);
	for (const MenuItem &top : workspace->mainMenu()) gtk_menu_shell_append(GTK_MENU_SHELL(menubar), nativeItem(top, accelGroup, true));
	gtk_widget_show_all(menubar);
}

void App::menusChanged() {
	if (window && menubar) buildMenuBar();
}

#pragma mark - Folder as Workspace

void App::buildFolderPanel() {
	folderBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(folderBox), "omapad-folders");
	GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	gtk_widget_set_margin_start(header, 10);
	gtk_widget_set_margin_end(header, 4);
	GtkWidget *title = gtk_label_new("WORKSPACE");
	gtk_style_context_add_class(gtk_widget_get_style_context(title), "omapad-folder-title");
	gtk_label_set_xalign(GTK_LABEL(title), 0);
	GtkWidget *refresh = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_MENU);
	GtkWidget *add = gtk_button_new_from_icon_name("list-add-symbolic", GTK_ICON_SIZE_MENU);
	gtk_widget_set_tooltip_text(refresh, "Refresh");
	gtk_widget_set_tooltip_text(add, "Add a folder");
	for (GtkWidget *button : {refresh, add}) {
		gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
		gtk_widget_set_can_focus(button, FALSE);
	}
	gtk_box_pack_start(GTK_BOX(header), title, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(header), refresh, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(header), add, FALSE, FALSE, 0);
	onSignal(refresh, "clicked", [this]() { foldersChanged(); });
	onSignal(add, "clicked", [this]() { workspace->run(Cmd::OpenFolder); });

	folderStore = gtk_tree_store_new(ColCount, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_BOOLEAN);
	folderView = gtk_tree_view_new_with_model(GTK_TREE_MODEL(folderStore));
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(folderView), FALSE);
	gtk_tree_view_set_enable_search(GTK_TREE_VIEW(folderView), TRUE);
	gtk_tree_view_set_search_column(GTK_TREE_VIEW(folderView), ColName);
	gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(folderView), ColPath);
	GtkTreeViewColumn *column = gtk_tree_view_column_new();
	GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
	GtkCellRenderer *text = gtk_cell_renderer_text_new();
	g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, nullptr);
	gtk_tree_view_column_pack_start(column, icon, FALSE);
	gtk_tree_view_column_pack_start(column, text, TRUE);
	gtk_tree_view_column_add_attribute(column, icon, "icon-name", ColIcon);
	gtk_tree_view_column_add_attribute(column, text, "text", ColName);
	gtk_tree_view_append_column(GTK_TREE_VIEW(folderView), column);

	g_signal_connect(folderView, "row-expanded", G_CALLBACK(+[](GtkTreeView *, GtkTreeIter *iter, GtkTreePath *, gpointer self) {
		App *app = static_cast<App *>(self);
		app->populateRow(iter);
		if (app->reloadingFolders) return;
		gchar *path = nullptr;
		gtk_tree_model_get(GTK_TREE_MODEL(app->folderStore), iter, ColPath, &path, -1);
		if (path) app->workspace->setFolderExpanded(path, true);
		g_free(path);
	}), this);
	g_signal_connect(folderView, "row-collapsed", G_CALLBACK(+[](GtkTreeView *, GtkTreeIter *iter, GtkTreePath *, gpointer self) {
		App *app = static_cast<App *>(self);
		if (app->reloadingFolders) return;
		gchar *path = nullptr;
		gtk_tree_model_get(GTK_TREE_MODEL(app->folderStore), iter, ColPath, &path, -1);
		if (path) app->workspace->setFolderExpanded(path, false);
		g_free(path);
	}), this);
	g_signal_connect(folderView, "row-activated", G_CALLBACK(+[](GtkTreeView *view, GtkTreePath *treePath, GtkTreeViewColumn *, gpointer self) {
		App *app = static_cast<App *>(self);
		GtkTreeIter iter;
		if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(app->folderStore), &iter, treePath)) return;
		gchar *path = nullptr;
		gboolean isDirectory = FALSE;
		gtk_tree_model_get(GTK_TREE_MODEL(app->folderStore), &iter, ColPath, &path, ColIsDir, &isDirectory, -1);
		if (isDirectory) {
			if (gtk_tree_view_row_expanded(view, treePath)) gtk_tree_view_collapse_row(view, treePath);
			else gtk_tree_view_expand_row(view, treePath, FALSE);
		} else if (path) {
			app->workspace->openPath(path);
		}
		g_free(path);
	}), this);
	g_signal_connect(folderView, "button-press-event", G_CALLBACK(+[](GtkWidget *widget, GdkEventButton *event, gpointer self) -> gboolean {
		if (event->button != GDK_BUTTON_SECONDARY) return FALSE;
		App *app = static_cast<App *>(self);
		GtkTreePath *treePath = nullptr;
		if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), static_cast<gint>(event->x), static_cast<gint>(event->y), &treePath, nullptr, nullptr, nullptr)) return FALSE;
		gtk_tree_view_set_cursor(GTK_TREE_VIEW(widget), treePath, nullptr, FALSE);
		GtkTreeIter iter;
		if (gtk_tree_model_get_iter(GTK_TREE_MODEL(app->folderStore), &iter, treePath)) {
			gchar *path = nullptr;
			gboolean isDirectory = FALSE;
			gboolean isRoot = FALSE;
			gtk_tree_model_get(GTK_TREE_MODEL(app->folderStore), &iter, ColPath, &path, ColIsDir, &isDirectory, ColIsRoot, &isRoot, -1);
			if (path) app->popup(app->workspace->folderMenu(path, isDirectory, isRoot));
			g_free(path);
		}
		gtk_tree_path_free(treePath);
		return TRUE;
	}), this);

	GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(scroll), folderView);
	gtk_box_pack_start(GTK_BOX(folderBox), header, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(folderBox), scroll, TRUE, TRUE, 0);
}

void App::addFolderChildren(GtkTreeIter *parent, const std::string &path) {
	for (const FolderEntry &entry : workspace->listFolder(path)) {
		GtkTreeIter iter;
		gtk_tree_store_append(folderStore, &iter, parent);
		gtk_tree_store_set(folderStore, &iter, ColName, entry.name.c_str(), ColPath, entry.path.c_str(), ColIsDir, entry.isDirectory, ColIsRoot, FALSE,
		                   ColIcon, entry.isDirectory ? "folder-symbolic" : "text-x-generic-symbolic", ColPopulated, FALSE, -1);
		if (entry.isDirectory) {
			// An empty child gives the row an expander without reading the folder yet.
			GtkTreeIter placeholder;
			gtk_tree_store_append(folderStore, &placeholder, &iter);
		}
	}
}

void App::populateRow(GtkTreeIter *iter) {
	GtkTreeModel *model = GTK_TREE_MODEL(folderStore);
	gboolean populated = FALSE;
	gchar *path = nullptr;
	gtk_tree_model_get(model, iter, ColPopulated, &populated, ColPath, &path, -1);
	if (!populated && path) {
		gtk_tree_store_set(folderStore, iter, ColPopulated, TRUE, -1);
		addFolderChildren(iter, path);
		// The placeholder was the first child.
		GtkTreeIter first;
		if (gtk_tree_model_iter_children(model, &first, iter)) gtk_tree_store_remove(folderStore, &first);
	}
	g_free(path);
}

void App::restoreExpansion(GtkTreeIter *iter) {
	GtkTreeModel *model = GTK_TREE_MODEL(folderStore);
	gchar *path = nullptr;
	gboolean isDirectory = FALSE;
	gtk_tree_model_get(model, iter, ColPath, &path, ColIsDir, &isDirectory, -1);
	const bool expand = path && isDirectory && workspace->isFolderExpanded(path);
	g_free(path);
	if (!expand) return;
	populateRow(iter);
	GtkTreePath *treePath = gtk_tree_model_get_path(model, iter);
	gtk_tree_view_expand_row(GTK_TREE_VIEW(folderView), treePath, FALSE);
	gtk_tree_path_free(treePath);
	GtkTreeIter child;
	if (gtk_tree_model_iter_children(model, &child, iter)) {
		do {
			restoreExpansion(&child);
		} while (gtk_tree_model_iter_next(model, &child));
	}
}

void App::foldersChanged() {
	if (!folderStore) return;
	reloadingFolders = true;
	std::string selected;
	GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(folderView));
	GtkTreeIter selectedIter;
	if (gtk_tree_selection_get_selected(selection, nullptr, &selectedIter)) {
		gchar *path = nullptr;
		gtk_tree_model_get(GTK_TREE_MODEL(folderStore), &selectedIter, ColPath, &path, -1);
		if (path) selected = path;
		g_free(path);
	}
	gtk_tree_store_clear(folderStore);
	for (const std::string &root : workspace->folders()) {
		GtkTreeIter iter;
		gtk_tree_store_append(folderStore, &iter, nullptr);
		const std::string name = fs::path(root).filename().string();
		gtk_tree_store_set(folderStore, &iter, ColName, name.empty() ? root.c_str() : name.c_str(), ColPath, root.c_str(), ColIsDir, TRUE, ColIsRoot, TRUE,
		                   ColIcon, "folder-symbolic", ColPopulated, FALSE, -1);
		GtkTreeIter placeholder;
		gtk_tree_store_append(folderStore, &placeholder, &iter);
		restoreExpansion(&iter);
	}
	// Reselect what was selected, if it's still there.
	if (!selected.empty()) {
		std::function<bool(GtkTreeIter *)> find = [&](GtkTreeIter *iter) -> bool {
			do {
				gchar *path = nullptr;
				gtk_tree_model_get(GTK_TREE_MODEL(folderStore), iter, ColPath, &path, -1);
				const bool match = path && selected == path;
				g_free(path);
				if (match) {
					gtk_tree_selection_select_iter(selection, iter);
					return true;
				}
				GtkTreeIter child;
				if (gtk_tree_model_iter_children(GTK_TREE_MODEL(folderStore), &child, iter) && find(&child)) return true;
			} while (gtk_tree_model_iter_next(GTK_TREE_MODEL(folderStore), iter));
			return false;
		};
		GtkTreeIter first;
		if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(folderStore), &first)) find(&first);
	}
	reloadingFolders = false;
}

void App::setFolderPanelVisible(bool visible) {
	if (notesUiState.enabled) visible = false;
	gtk_widget_set_visible(folderBox, visible);
	if (visible) {
		gtk_widget_show_all(folderBox);
		gtk_paned_set_position(GTK_PANED(outerPaned), 240);
		foldersChanged();
	}
}

#pragma mark - Function List and Document Map

void App::buildFunctionPanel() {
	functionBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(functionBox), "omapad-folders");
	gtk_widget_set_size_request(functionBox, 160, -1);
	GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	gtk_widget_set_margin_start(header, 10);
	gtk_widget_set_margin_end(header, 4);
	GtkWidget *title = gtk_label_new("FUNCTIONS");
	gtk_style_context_add_class(gtk_widget_get_style_context(title), "omapad-folder-title");
	gtk_label_set_xalign(GTK_LABEL(title), 0);
	functionSort = gtk_toggle_button_new();
	gtk_button_set_image(GTK_BUTTON(functionSort), gtk_image_new_from_icon_name("view-sort-ascending-symbolic", GTK_ICON_SIZE_MENU));
	gtk_widget_set_tooltip_text(functionSort, "Sort by name");
	GtkWidget *refresh = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_MENU);
	gtk_widget_set_tooltip_text(refresh, "Refresh");
	for (GtkWidget *button : {functionSort, refresh}) {
		gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
		gtk_widget_set_can_focus(button, FALSE);
	}
	gtk_box_pack_start(GTK_BOX(header), title, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(header), functionSort, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(header), refresh, FALSE, FALSE, 0);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(functionSort), workspace->settings.prefOn("functionListSorted"));
	onSignal(functionSort, "toggled", [this]() { functionListChanged(); });
	onSignal(refresh, "clicked", [this]() { workspace->refreshFunctionList(); });

	functionFilter = gtk_search_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(functionFilter), "Filter");
	gtk_widget_set_margin_start(functionFilter, 6);
	gtk_widget_set_margin_end(functionFilter, 6);
	gtk_widget_set_margin_bottom(functionFilter, 4);
	onSignal(functionFilter, "search-changed", [this]() { functionListChanged(); });

	functionEmpty = gtk_label_new("");
	gtk_label_set_line_wrap(GTK_LABEL(functionEmpty), TRUE);
	gtk_widget_set_margin_top(functionEmpty, 12);
	gtk_widget_set_no_show_all(functionEmpty, TRUE);

	functionStore = gtk_tree_store_new(FnCount, G_TYPE_STRING, G_TYPE_INT);
	functionView = gtk_tree_view_new_with_model(GTK_TREE_MODEL(functionStore));
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(functionView), FALSE);
	gtk_tree_view_set_enable_search(GTK_TREE_VIEW(functionView), FALSE);
	gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(functionView), FnName);
	GtkCellRenderer *text = gtk_cell_renderer_text_new();
	g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
	gtk_tree_view_append_column(GTK_TREE_VIEW(functionView), gtk_tree_view_column_new_with_attributes("", text, "text", FnName, nullptr));
	g_signal_connect(functionView, "row-activated", G_CALLBACK(+[](GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *, gpointer self) {
		App *app = static_cast<App *>(self);
		GtkTreeIter iter;
		if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(app->functionStore), &iter, path)) return;
		gint index = -1;
		gtk_tree_model_get(GTK_TREE_MODEL(app->functionStore), &iter, FnIndex, &index, -1);
		if (index >= 0) {
			app->workspace->goToFunction(static_cast<size_t>(index));
		} else if (gtk_tree_view_row_expanded(view, path)) {
			gtk_tree_view_collapse_row(view, path);
		} else {
			gtk_tree_view_expand_row(view, path, FALSE);
		}
	}), this);
	const auto expansion = +[](GtkTreeView *, GtkTreeIter *iter, GtkTreePath *, gpointer self, bool collapsed) {
		App *app = static_cast<App *>(self);
		if (app->reloadingFunctions) return;
		gchar *name = nullptr;
		gtk_tree_model_get(GTK_TREE_MODEL(app->functionStore), iter, FnName, &name, -1);
		if (name && collapsed) app->collapsedClasses.insert(name);
		else if (name) app->collapsedClasses.erase(name);
		g_free(name);
	};
	static const auto recordExpansion = expansion;
	g_signal_connect(functionView, "row-collapsed", G_CALLBACK(+[](GtkTreeView *v, GtkTreeIter *i, GtkTreePath *p, gpointer self) { recordExpansion(v, i, p, self, true); }), this);
	g_signal_connect(functionView, "row-expanded", G_CALLBACK(+[](GtkTreeView *v, GtkTreeIter *i, GtkTreePath *p, gpointer self) { recordExpansion(v, i, p, self, false); }), this);

	GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(scroll), functionView);
	gtk_box_pack_start(GTK_BOX(functionBox), header, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(functionBox), functionFilter, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(functionBox), functionEmpty, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(functionBox), scroll, TRUE, TRUE, 0);
}

void App::functionListChanged() {
	if (!functionStore || !gtk_widget_get_visible(functionBox)) return;
	const auto lower = [](std::string value) {
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(g_ascii_tolower(c)); });
		return value;
	};
	const std::string filter = lower(gtk_entry_get_text(GTK_ENTRY(functionFilter)));
	const std::vector<FunctionEntry> &entries = workspace->functionEntries();
	std::vector<size_t> order;
	for (size_t i = 0; i < entries.size(); i++) {
		if (filter.empty() || lower(entries[i].name).find(filter) != std::string::npos || lower(entries[i].className).find(filter) != std::string::npos)
			order.push_back(i);
	}
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(functionSort)))
		std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return lower(entries[a].name) < lower(entries[b].name); });

	reloadingFunctions = true;
	gtk_tree_store_clear(functionStore);
	std::map<std::string, GtkTreeIter> classes;
	for (const size_t i : order) {
		const FunctionEntry &entry = entries[i];
		GtkTreeIter *parent = nullptr;
		if (!entry.className.empty()) {
			auto found = classes.find(entry.className);
			if (found == classes.end()) {
				GtkTreeIter classIter;
				gtk_tree_store_append(functionStore, &classIter, nullptr);
				gtk_tree_store_set(functionStore, &classIter, FnName, entry.className.c_str(), FnIndex, -1, -1);
				found = classes.emplace(entry.className, classIter).first;
			}
			parent = &found->second;
		}
		GtkTreeIter iter;
		gtk_tree_store_append(functionStore, &iter, parent);
		gtk_tree_store_set(functionStore, &iter, FnName, entry.name.c_str(), FnIndex, static_cast<gint>(i), -1);
	}
	for (auto &entry : classes) {
		if (collapsedClasses.count(entry.first)) continue;
		GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(functionStore), &entry.second);
		gtk_tree_view_expand_row(GTK_TREE_VIEW(functionView), path, FALSE);
		gtk_tree_path_free(path);
	}
	reloadingFunctions = false;

	const bool nothing = order.empty();
	gtk_widget_set_visible(functionEmpty, nothing);
	if (nothing) {
		const char *message = !filter.empty() ? "Nothing matches the filter."
			: !workspace->activeLanguageHasFunctionList() ? "This language has no function list."
			: "No functions found.";
		gtk_label_set_text(GTK_LABEL(functionEmpty), message);
	}
}

void App::setFunctionListVisible(bool visible) {
	gtk_widget_set_visible(functionBox, visible);
	if (visible) {
		gtk_widget_show_all(functionBox);
		functionListChanged();
	}
	arrangeSidePanels();
}

// The map takes no input of its own: presses jump the editor there, and scrolling scrolls the
// editor. Stopping these signals here also keeps it from ever taking the keyboard focus.
void App::buildDocumentMap() {
	map.widget = scintilla_new();
	gtk_widget_set_can_focus(map.widget, FALSE);
	gtk_widget_set_size_request(map.widget, 120, -1);
	g_signal_connect(map.widget, "button-press-event", G_CALLBACK(+[](GtkWidget *, GdkEventButton *event, gpointer self) -> gboolean {
		if (event->button == GDK_BUTTON_PRIMARY) static_cast<App *>(self)->workspace->mapPressed(static_cast<long>(event->x), static_cast<long>(event->y));
		return TRUE;
	}), this);
	g_signal_connect(map.widget, "button-release-event", G_CALLBACK(+[](GtkWidget *, GdkEventButton *, gpointer) -> gboolean { return TRUE; }), nullptr);
	g_signal_connect(map.widget, "motion-notify-event", G_CALLBACK(+[](GtkWidget *, GdkEventMotion *event, gpointer self) -> gboolean {
		if (event->state & GDK_BUTTON1_MASK) static_cast<App *>(self)->workspace->mapPressed(static_cast<long>(event->x), static_cast<long>(event->y));
		return TRUE;
	}), this);
	g_signal_connect(map.widget, "scroll-event", G_CALLBACK(+[](GtkWidget *, GdkEventScroll *event, gpointer self) -> gboolean {
		App *app = static_cast<App *>(self);
		static double pending = 0;
		if (event->direction == GDK_SCROLL_UP) pending -= 3;
		else if (event->direction == GDK_SCROLL_DOWN) pending += 3;
		else if (event->direction == GDK_SCROLL_SMOOTH) pending += event->delta_y * 3;
		// Trackpads send fractions of a line, so they're saved up until there's a whole one.
		const long whole = static_cast<long>(pending);
		pending -= static_cast<double>(whole);
		if (whole != 0) app->workspace->mapScrolled(whole);
		return TRUE;
	}), this);
}

void App::setDocumentMapVisible(bool visible) {
	gtk_widget_set_visible(map.widget, visible);
	arrangeSidePanels();
}

// Gives the side panels their room, now and whenever one is shown or hidden.
void App::arrangeSidePanels() {
	if (!sidePaned) return;
	if (notesUiState.enabled) { gtk_widget_hide(sideBox); return; }
	bool anyPanel = false;
	for (const PanelWidgets &panel : panelWidgets) anyPanel = anyPanel || (panel.box && gtk_widget_get_visible(panel.box));
	if (panelsBox) gtk_widget_set_visible(panelsBox, anyPanel);
	const int wanted = (anyPanel ? 260 : 0) + (gtk_widget_get_visible(functionBox) ? 250 : 0) + (gtk_widget_get_visible(map.widget) ? 120 : 0);
	gtk_widget_set_visible(sideBox, wanted > 0);
	GtkAllocation allocation;
	gtk_widget_get_allocation(sidePaned, &allocation);
	if (wanted > 0 && allocation.width > wanted + 100) gtk_paned_set_position(GTK_PANED(sidePaned), allocation.width - wanted);
}

#pragma mark - Find window

void App::buildFindWindow() {
	findWindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(findWindow), "Find");
	gtk_window_set_transient_for(GTK_WINDOW(findWindow), GTK_WINDOW(window));
	gtk_window_set_type_hint(GTK_WINDOW(findWindow), GDK_WINDOW_TYPE_HINT_DIALOG);
	gtk_window_set_resizable(GTK_WINDOW(findWindow), TRUE);
	gtk_window_set_default_size(GTK_WINDOW(findWindow), 620, -1);
	g_signal_connect(findWindow, "notify::is-active", G_CALLBACK(+[](GObject *object, GParamSpec *, gpointer self) {
		App *app = static_cast<App *>(self);
		const std::string transparency = app->workspace->settings.pref("findTransparency");
		if (transparency != "lostFocus") return;
		gtk_widget_set_opacity(GTK_WIDGET(object), gtk_window_is_active(GTK_WINDOW(object)) ? 1.0 : app->workspace->settings.prefInt("findOpacity") / 100.0);
	}), this);
	g_signal_connect(findWindow, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), nullptr);
	g_signal_connect(findWindow, "key-press-event", G_CALLBACK(+[](GtkWidget *w, GdkEventKey *event, gpointer self) -> gboolean {
		App *app = static_cast<App *>(self);
		// Shift+Enter searches the other way, once.
		if ((event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) && (event->state & GDK_SHIFT_MASK)) {
			GtkToggleButton *backward = GTK_TOGGLE_BUTTON(app->backward);
			const gboolean was = gtk_toggle_button_get_active(backward);
			gtk_toggle_button_set_active(backward, !was);
			app->workspace->findPanelFindNext();
			gtk_toggle_button_set_active(backward, was);
			return TRUE;
		}
		if (event->keyval != GDK_KEY_Escape) return FALSE;
		gtk_widget_hide(w);
		return TRUE;
	}), this);

	GtkWidget *stack = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_container_set_border_width(GTK_CONTAINER(stack), 12);
	gtk_container_add(GTK_CONTAINER(findWindow), stack);

	GtkWidget *tabs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(tabs), "linked");
	const char *titles[] = {"Find", "Replace", "Find in Files", "Mark"};
	for (int i = 0; i < 4; i++) {
		findTabs[i] = i == 0 ? gtk_radio_button_new_with_label(nullptr, titles[i]) : gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(findTabs[0]), titles[i]);
		gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(findTabs[i]), FALSE);
		gtk_box_pack_start(GTK_BOX(tabs), findTabs[i], FALSE, FALSE, 0);
		onSignal(findTabs[i], "toggled", [this, i]() {
			if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(findTabs[i]))) applyFindTab(static_cast<FindTab>(i));
		});
	}
	gtk_box_pack_start(GTK_BOX(stack), tabs, FALSE, FALSE, 0);

	const auto labelled = [&](const char *text, GtkWidget *control) {
		GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		GtkWidget *label = gtk_label_new(text);
		gtk_label_set_xalign(GTK_LABEL(label), 1);
		gtk_widget_set_size_request(label, 64, -1);
		gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(row), control, TRUE, TRUE, 0);
		gtk_box_pack_start(GTK_BOX(stack), row, FALSE, FALSE, 0);
		return row;
	};
	what = gtk_combo_box_text_new_with_entry();
	with = gtk_combo_box_text_new_with_entry();
	filters = gtk_entry_new();
	gtk_entry_set_text(GTK_ENTRY(filters), "*");
	gtk_entry_set_placeholder_text(GTK_ENTRY(filters), "*.txt *.py (space separated, * for all)");
	directory = gtk_entry_new();
	gtk_entry_set_text(GTK_ENTRY(directory), g_get_home_dir());
	mode = gtk_combo_box_text_new();
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(mode), "Normal");
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(mode), "Extended (\\n, \\r, \\t, \\x…)");
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(mode), "Regular expression");
	gtk_combo_box_set_active(GTK_COMBO_BOX(mode), 0);

	labelled("Find:", what);
	withRow = labelled("Replace:", with);
	filtersRow = labelled("Filters:", filters);
	GtkWidget *directoryControl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *browse = gtk_button_new_with_label("Choose…");
	GtkWidget *current = gtk_button_new_with_label("<<");
	gtk_widget_set_tooltip_text(current, "Use the current document's folder");
	onSignal(current, "clicked", [this]() {
		const std::string path = workspace->activePath();
		if (!path.empty()) gtk_entry_set_text(GTK_ENTRY(directory), fs::path(path).parent_path().c_str());
	});
	gtk_box_pack_start(GTK_BOX(directoryControl), directory, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(directoryControl), current, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(directoryControl), browse, FALSE, FALSE, 0);
	directoryRow = labelled("Folder:", directoryControl);
	onSignal(browse, "clicked", [this]() {
		std::string path;
		if (chooseFolder(gtk_entry_get_text(GTK_ENTRY(directory)), path)) gtk_entry_set_text(GTK_ENTRY(directory), path.c_str());
	});
	labelled("Mode:", mode);

	const auto checkRow = [&](std::initializer_list<GtkWidget *> boxes) {
		GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
		gtk_widget_set_margin_start(row, 72);
		for (GtkWidget *box : boxes) gtk_box_pack_start(GTK_BOX(row), box, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(stack), row, FALSE, FALSE, 0);
		return row;
	};
	matchCase = gtk_check_button_new_with_label("Match case");
	wholeWord = gtk_check_button_new_with_label("Whole word");
	wrap = gtk_check_button_new_with_label("Wrap around");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(wrap), TRUE);
	backward = gtk_check_button_new_with_label("Backward");
	inSelection = gtk_check_button_new_with_label("In selection");
	recursive = gtk_check_button_new_with_label("In subfolders");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(recursive), TRUE);
	hidden = gtk_check_button_new_with_label("In hidden folders");
	checkRow({matchCase, wholeWord, wrap, backward});
	dotNewline = gtk_check_button_new_with_label(". matches newline");
	gtk_widget_set_sensitive(dotNewline, FALSE);
	onSignal(mode, "changed", [this]() { gtk_widget_set_sensitive(dotNewline, gtk_combo_box_get_active(GTK_COMBO_BOX(mode)) == 2); });
	selectionRow = checkRow({inSelection, dotNewline});
	filesOptions = checkRow({recursive, hidden});
	bookmarkLine = gtk_check_button_new_with_label("Bookmark line");
	purgeMarks = gtk_check_button_new_with_label("Purge for each search");
	markOptions = checkRow({bookmarkLine, purgeMarks});

	const auto buttonRow = [&](std::initializer_list<std::pair<const char *, std::function<void()>>> buttons) {
		GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
		for (const auto &entry : buttons) {
			GtkWidget *button = gtk_button_new_with_label(entry.first);
			auto action = entry.second;
			onSignal(button, "clicked", [this, action]() {
				rememberHistory();
				action();
			});
			gtk_box_pack_start(GTK_BOX(row), button, FALSE, FALSE, 0);
		}
		gtk_box_pack_start(GTK_BOX(stack), row, FALSE, FALSE, 0);
		return row;
	};
	Workspace *w = workspace.get();
	findButtons = buttonRow({{"Find Next", [w] { w->findPanelFindNext(); }}, {"Count", [w] { w->findPanelCount(); }}, {"Find All", [w] { w->findPanelFindAll(); }}, {"In All Open Documents", [w] { w->findPanelFindAllOpen(); }},
	                         {"Mark All", [w] { w->findPanelMarkAll(); }}, {"Clear Marks", [w] { w->findPanelClearMarks(); }}});
	replaceButtons = buttonRow({{"Find Next", [w] { w->findPanelFindNext(); }}, {"Replace", [w] { w->findPanelReplace(); }}, {"Replace All", [w] { w->findPanelReplaceAll(); }},
	                            {"In All Open Documents", [w] { w->findPanelReplaceAllOpen(); }}});
	markButtons = buttonRow({{"Mark All", [w] { w->findPanelMarkAll(); }}, {"Clear All Marks", [w] { w->findPanelClearMarks(); }},
	                         {"Copy Marked Text", [w] { w->run(Cmd::CopyStyled, 0); }}});
	filesButtons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	findInFilesButton = gtk_button_new_with_label("Find All");
	stopButton = gtk_button_new_with_label("Stop");
	gtk_widget_set_sensitive(stopButton, FALSE);
	gtk_box_pack_start(GTK_BOX(filesButtons), findInFilesButton, FALSE, FALSE, 0);
	GtkWidget *replaceInFiles = gtk_button_new_with_label("Replace in Files");
	gtk_box_pack_start(GTK_BOX(filesButtons), replaceInFiles, FALSE, FALSE, 0);
	onSignal(replaceInFiles, "clicked", [this, w]() {
		rememberHistory();
		w->findPanelReplaceInFiles();
	});
	gtk_box_pack_start(GTK_BOX(filesButtons), stopButton, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(stack), filesButtons, FALSE, FALSE, 0);
	onSignal(findInFilesButton, "clicked", [this, w]() {
		rememberHistory();
		w->findPanelFindInFiles();
	});
	onSignal(stopButton, "clicked", [w]() { w->stopFindInFiles(); });

	findStatus = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(findStatus), 0);
	gtk_label_set_line_wrap(GTK_LABEL(findStatus), TRUE);
	gtk_box_pack_start(GTK_BOX(stack), findStatus, FALSE, FALSE, 0);

	// Return in the Find field runs whatever the current tab is for.
	onSignal(whatEntry(), "activate", [this, w]() {
		rememberHistory();
		if (findTab == FindTab::Files) w->findPanelFindInFiles();
		else w->findPanelFindNext();
	});
	gtk_widget_show_all(stack);
	gtk_widget_set_no_show_all(findWindow, FALSE);
}

void App::applyFindTab(FindTab tab) {
	findTab = tab;
	const bool isFiles = tab == FindTab::Files;
	gtk_widget_set_visible(withRow, tab == FindTab::Replace || tab == FindTab::Files);
	gtk_widget_set_visible(filtersRow, isFiles);
	gtk_widget_set_visible(directoryRow, isFiles);
	gtk_widget_set_visible(filesOptions, isFiles);
	gtk_widget_set_visible(selectionRow, !isFiles);
	gtk_widget_set_visible(findButtons, tab == FindTab::Find);
	gtk_widget_set_visible(replaceButtons, tab == FindTab::Replace);
	gtk_widget_set_visible(filesButtons, isFiles);
	gtk_widget_set_visible(markButtons, tab == FindTab::Mark);
	gtk_widget_set_visible(markOptions, tab == FindTab::Mark);
	gtk_widget_set_visible(backward, !isFiles);
	gtk_widget_set_visible(wrap, !isFiles);
	const char *titles[] = {"Find", "Replace", "Find in Files", "Mark"};
	gtk_window_set_title(GTK_WINDOW(findWindow), titles[static_cast<int>(tab)]);
	static GtkCssProvider *monospace = nullptr;
	if (!monospace) {
		monospace = gtk_css_provider_new();
		gtk_css_provider_load_from_data(monospace, "entry { font-family: monospace; }", -1, nullptr);
	}
	GtkStyleContext *whatStyle = gtk_widget_get_style_context(GTK_WIDGET(whatEntry()));
	if (workspace->settings.prefOn("findMonospace")) gtk_style_context_add_provider(whatStyle, GTK_STYLE_PROVIDER(monospace), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	else gtk_style_context_remove_provider(whatStyle, GTK_STYLE_PROVIDER(monospace));
	gtk_label_set_text(GTK_LABEL(findStatus), "");
	gtk_window_resize(GTK_WINDOW(findWindow), 620, 1);
}

void App::showFind(FindTab tab, const std::string &prefill, const std::string &folder) {
	const std::string transparency = workspace->settings.pref("findTransparency");
	const double opacity = workspace->settings.prefInt("findOpacity") / 100.0;
	gtk_widget_set_opacity(findWindow, transparency == "always" ? opacity : 1.0);
	if (!folder.empty()) gtk_entry_set_text(GTK_ENTRY(directory), folder.c_str());
	if (!prefill.empty() && prefill.find_first_of("\r\n") == std::string::npos) gtk_entry_set_text(whatEntry(), prefill.c_str());
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(findTabs[static_cast<int>(tab)]), TRUE);
	applyFindTab(tab);
	gtk_window_present(GTK_WINDOW(findWindow));
	gtk_widget_grab_focus(GTK_WIDGET(whatEntry()));
}

FindOptions App::currentFindOptions() {
	const auto on = [](GtkWidget *w) { return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)) != FALSE; };
	FindOptions o;
	o.what = gtk_entry_get_text(whatEntry());
	o.with = gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(with))));
	o.matchCase = on(matchCase);
	o.wholeWord = on(wholeWord);
	o.wrap = on(wrap);
	o.backward = on(backward);
	o.inSelection = on(inSelection);
	o.mode = static_cast<SearchMode>(gtk_combo_box_get_active(GTK_COMBO_BOX(mode)));
	o.dotMatchesNewline = on(dotNewline);
	std::string folder = gtk_entry_get_text(GTK_ENTRY(directory));
	if (folder.rfind("~/", 0) == 0 || folder == "~") folder = std::string(g_get_home_dir()) + folder.substr(1);
	o.directory = folder;
	o.filters = gtk_entry_get_text(GTK_ENTRY(filters));
	o.recursive = on(recursive);
	o.includeHidden = on(hidden);
	o.bookmarkLine = findTab == FindTab::Mark && on(bookmarkLine);
	o.purge = findTab == FindTab::Mark && on(purgeMarks);
	return o;
}

void App::rememberHistory() {
	const auto remember = [](GtkWidget *box, std::vector<std::string> &history) {
		const std::string value = gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(box))));
		if (value.empty() || (!history.empty() && history.front() == value)) return;
		history.erase(std::remove(history.begin(), history.end(), value), history.end());
		history.insert(history.begin(), value);
		if (history.size() > 20) history.resize(20);
		gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(box));
		for (const std::string &entry : history) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(box), entry.c_str());
		gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(box))), value.c_str());
	};
	remember(what, whatHistory);
	remember(with, withHistory);
}

void App::setFindStatus(const std::string &status, bool isError) {
	gtk_label_set_text(GTK_LABEL(findStatus), status.c_str());
	GtkStyleContext *style = gtk_widget_get_style_context(findStatus);
	if (isError) gtk_style_context_add_class(style, "error");
	else gtk_style_context_remove_class(style, "error");
}

#pragma mark - Panels, toolbar and window modes

bool App::clipboardData(const std::string &format, std::string &out) {
	GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
	if (format == "text") {
		gchar *text = gtk_clipboard_wait_for_text(clipboard);
		if (!text) return false;
		out = text;
		g_free(text);
		return true;
	}
	GtkSelectionData *data = gtk_clipboard_wait_for_contents(clipboard, gdk_atom_intern(format.c_str(), FALSE));
	if (!data) return false;
	gint length = 0;
	const guchar *bytes = gtk_selection_data_get_data_with_length(data, &length);
	if (bytes && length > 0) out.assign(reinterpret_cast<const char *>(bytes), static_cast<size_t>(length));
	gtk_selection_data_free(data);
	return !out.empty();
}

void App::setClipboardData(const std::string &format, const std::string &data) {
	// For "text/html+rtf", the two parts are separated by a zero byte.
	const size_t split = data.find('\0');
	heldClipboard[0] = format == "text/rtf" ? std::string() : data.substr(0, split);
	heldClipboard[1] = format == "text/rtf" ? data : format == "text/html+rtf" && split != std::string::npos ? data.substr(split + 1) : std::string();
	std::vector<GtkTargetEntry> targets;
	static char htmlName[] = "text/html", rtfName[] = "text/rtf", plainName[] = "UTF8_STRING";
	if (!heldClipboard[0].empty()) {
		targets.push_back({htmlName, 0, 0});
		targets.push_back({plainName, 0, 0});
	}
	if (!heldClipboard[1].empty()) targets.push_back({rtfName, 0, 1});
	gtk_clipboard_set_with_data(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), targets.data(), static_cast<guint>(targets.size()),
		+[](GtkClipboard *, GtkSelectionData *selection, guint info, gpointer self) {
			const std::string &held = static_cast<App *>(self)->heldClipboard[info];
			gtk_selection_data_set(selection, gtk_selection_data_get_target(selection), 8, reinterpret_cast<const guchar *>(held.data()), static_cast<gint>(held.size()));
		}, nullptr, this);
}

int App::runList(const ListDialog &list, std::vector<int> &selection) {
	GtkWidget *dialog = gtk_dialog_new_with_buttons(list.title.c_str(), GTK_WINDOW(window), GTK_DIALOG_MODAL, nullptr, nullptr);
	for (size_t i = 0; i < list.buttons.size(); i++) gtk_dialog_add_button(GTK_DIALOG(dialog), list.buttons[i].c_str(), static_cast<gint>(i));
	gtk_window_set_default_size(GTK_WINDOW(dialog), 720, 460);
	std::vector<GType> types(list.columns.size(), G_TYPE_STRING);
	GtkListStore *store = gtk_list_store_newv(static_cast<gint>(types.size()), types.data());
	for (const auto &row : list.rows) {
		GtkTreeIter iter;
		gtk_list_store_append(store, &iter);
		for (size_t c = 0; c < list.columns.size() && c < row.size(); c++) gtk_list_store_set(store, &iter, static_cast<gint>(c), row[c].c_str(), -1);
	}
	GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
	g_object_unref(store);
	for (size_t c = 0; c < list.columns.size(); c++) {
		GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(list.columns[c].c_str(), gtk_cell_renderer_text_new(), "text", static_cast<gint>(c), nullptr);
		gtk_tree_view_column_set_resizable(column, TRUE);
		gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);
	}
	GtkTreeSelection *treeSelection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
	gtk_tree_selection_set_mode(treeSelection, list.multiSelect ? GTK_SELECTION_MULTIPLE : GTK_SELECTION_SINGLE);
	GtkTreePath *firstRow = gtk_tree_path_new_first();
	if (!list.rows.empty()) gtk_tree_selection_select_path(treeSelection, firstRow);
	gtk_tree_path_free(firstRow);
	g_signal_connect(view, "row-activated", G_CALLBACK(+[](GtkTreeView *, GtkTreePath *, GtkTreeViewColumn *, gpointer d) { gtk_dialog_response(GTK_DIALOG(d), 0); }), dialog);
	GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
	gtk_container_add(GTK_CONTAINER(scroll), view);
	gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), scroll, TRUE, TRUE, 0);
	gtk_widget_show_all(dialog);
	dialogDepth++;
	const gint response = gtk_dialog_run(GTK_DIALOG(dialog));
	dialogDepth--;
	selection.clear();
	GList *rows = gtk_tree_selection_get_selected_rows(treeSelection, nullptr);
	for (GList *entry = rows; entry; entry = entry->next) selection.push_back(gtk_tree_path_get_indices(static_cast<GtkTreePath *>(entry->data))[0]);
	g_list_free_full(rows, reinterpret_cast<GDestroyNotify>(gtk_tree_path_free));
	gtk_widget_destroy(dialog);
	return response >= 0 ? response : -1;
}

void App::setPanelVisible(int panel, const std::string &title, bool visible) {
	PanelWidgets &widgets = panelWidgets[panel];
	if (!widgets.box) {
		widgets.box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
		gtk_style_context_add_class(gtk_widget_get_style_context(widgets.box), "omapad-folders");
		std::string upper = title;
		std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
		GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
		gtk_widget_set_margin_start(header, 10);
		gtk_widget_set_margin_end(header, 4);
		GtkWidget *label = gtk_label_new(upper.c_str());
		gtk_style_context_add_class(gtk_widget_get_style_context(label), "omapad-folder-title");
		gtk_label_set_xalign(GTK_LABEL(label), 0);
		GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
		gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
		gtk_widget_set_can_focus(close, FALSE);
		onSignal(close, "clicked", [this, panel]() { post([this, panel]() { workspace->run(Cmd::TogglePanel, panel); }); });
		gtk_box_pack_start(GTK_BOX(header), label, TRUE, TRUE, 0);
		gtk_box_pack_start(GTK_BOX(header), close, FALSE, FALSE, 0);
		// Column 0 is the title, 1 the tooltip, and the rest are for panels with several columns.
		widgets.store = gtk_list_store_new(8, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
		widgets.view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(widgets.store));
		gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(widgets.view), FALSE);
		gtk_tree_view_set_enable_search(GTK_TREE_VIEW(widgets.view), FALSE);
		gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(widgets.view), 1);
		GtkCellRenderer *text = gtk_cell_renderer_text_new();
		g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_END, "family", panel == kPanelCharacters ? "monospace" : "sans", nullptr);
		gtk_tree_view_append_column(GTK_TREE_VIEW(widgets.view), gtk_tree_view_column_new_with_attributes("", text, "text", 0, nullptr));
		struct Binding { App *app; int panel; };
		auto *binding = new Binding{this, panel};
		g_signal_connect(widgets.view, "row-activated", G_CALLBACK(+[](GtkTreeView *, GtkTreePath *path, GtkTreeViewColumn *column, gpointer data) {
			auto *b = static_cast<Binding *>(data);
			const int row = gtk_tree_path_get_indices(path)[0];
			App *app = b->app;
			const int which = b->panel;
			const int at = column ? GPOINTER_TO_INT(g_object_get_data(G_OBJECT(column), "omapad-column")) : 0;
			app->post([app, which, row, at]() { app->workspace->panelActivate(which, row, at); });
		}), binding);
		g_signal_connect(widgets.view, "button-press-event", G_CALLBACK(+[](GtkWidget *view, GdkEventButton *event, gpointer data) -> gboolean {
			if (event->button != GDK_BUTTON_SECONDARY) return FALSE;
			auto *b = static_cast<Binding *>(data);
			GtkTreePath *path = nullptr;
			int row = 0;
			if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view), static_cast<gint>(event->x), static_cast<gint>(event->y), &path, nullptr, nullptr, nullptr)) {
				row = gtk_tree_path_get_indices(path)[0];
				gtk_tree_path_free(path);
			}
			const std::vector<MenuItem> menu = b->app->workspace->panelMenu(b->panel, row);
			if (!menu.empty()) b->app->popup(menu);
			return TRUE;
		}), binding);
		GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
		gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), workspace->panelColumns(panel).empty() ? GTK_POLICY_NEVER : GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
		gtk_container_add(GTK_CONTAINER(scroll), widgets.view);
		gtk_box_pack_start(GTK_BOX(widgets.box), header, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(widgets.box), scroll, TRUE, TRUE, 0);
		gtk_box_pack_start(GTK_BOX(panelsBox), widgets.box, TRUE, TRUE, 0);
	}
	if (visible) {
		gtk_widget_show_all(widgets.box);
		gtk_widget_set_no_show_all(sideBox, FALSE);
	} else {
		gtk_widget_hide(widgets.box);
	}
	arrangeSidePanels();
}

void App::panelChanged(int panel) {
	PanelWidgets &widgets = panelWidgets[panel];
	if (!widgets.store) return;
	const std::vector<PanelRow> rows = workspace->panelRows(panel);
	const std::vector<std::string> headings = workspace->panelColumns(panel);
	GtkTreeView *view = GTK_TREE_VIEW(widgets.view);
	if (static_cast<int>(headings.size()) != widgets.columnCount || (!headings.empty() && widgets.columnCount != static_cast<int>(gtk_tree_view_get_n_columns(view)))) {
		// Which columns show depends on the preferences, so they can change while it's open.
		widgets.columnCount = static_cast<int>(headings.size());
		widgets.signature.clear();
		while (GtkTreeViewColumn *column = gtk_tree_view_get_column(view, 0)) gtk_tree_view_remove_column(view, column);
		gtk_tree_view_set_headers_visible(view, !headings.empty());
		const auto addColumn = [&](const char *heading, int source, int index) {
			GtkCellRenderer *text = gtk_cell_renderer_text_new();
			g_object_set(text, "family", panel == kPanelCharacters ? "monospace" : "sans", nullptr);
			if (headings.empty()) g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
			GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(heading, text, "text", source, nullptr);
			gtk_tree_view_column_set_resizable(column, !headings.empty());
			g_object_set_data(G_OBJECT(column), "omapad-column", GINT_TO_POINTER(index));
			if (panel == kPanelDocuments) {
				gtk_tree_view_column_set_clickable(column, TRUE);
				onSignal(GTK_WIDGET(gtk_tree_view_column_get_button(column)), "clicked", [this, panel, index]() { workspace->panelColumnClicked(panel, index); });
			}
			gtk_tree_view_append_column(view, column);
		};
		if (headings.empty()) addColumn("", 0, 0);
		for (int i = 0; i < static_cast<int>(headings.size()) && i < 6; i++) addColumn(headings[static_cast<size_t>(i)].c_str(), 2 + i, i);
	}
	// Rewriting the same rows would lose the selection and the scroll position.
	std::string signature;
	for (const PanelRow &row : rows) {
		signature += std::string(2 * row.depth, ' ') + row.title + '\x1f' + row.toolTip + '\x1f';
		for (const std::string &cell : row.columns) signature += cell + '\x1f';
	}
	if (signature == widgets.signature) return;
	widgets.signature = signature;
	GtkTreeIter iter;
	gtk_list_store_clear(widgets.store);
	for (const PanelRow &row : rows) {
		gtk_list_store_append(widgets.store, &iter);
		gtk_list_store_set(widgets.store, &iter, 0, (std::string(2 * row.depth, ' ') + row.title).c_str(), 1, row.toolTip.c_str(), -1);
		for (int i = 0; i < static_cast<int>(row.columns.size()) && i < 6; i++) gtk_list_store_set(widgets.store, &iter, 2 + i, row.columns[static_cast<size_t>(i)].c_str(), -1);
	}
}

void App::focusPanel(int panel) {
	GtkWidget *target = panel == kPanelFolders ? folderView : panel == kPanelFunctions ? functionView : panel >= 0 && panel < kPanelCount ? panelWidgets[panel].view : nullptr;
	if (target) gtk_widget_grab_focus(target);
}

void App::selectFolderPath(const std::string &path) {
	if (!folderStore) return;
	GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(folderView));
	std::function<bool(GtkTreeIter *)> find = [&](GtkTreeIter *iter) -> bool {
		do {
			gchar *candidate = nullptr;
			gtk_tree_model_get(GTK_TREE_MODEL(folderStore), iter, ColPath, &candidate, -1);
			const bool match = candidate && path == candidate;
			g_free(candidate);
			if (match) {
				gtk_tree_selection_select_iter(selection, iter);
				GtkTreePath *treePath = gtk_tree_model_get_path(GTK_TREE_MODEL(folderStore), iter);
				gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(folderView), treePath, nullptr, FALSE, 0, 0);
				gtk_tree_path_free(treePath);
				return true;
			}
			GtkTreeIter child;
			if (gtk_tree_model_iter_children(GTK_TREE_MODEL(folderStore), &child, iter) && find(&child)) return true;
		} while (gtk_tree_model_iter_next(GTK_TREE_MODEL(folderStore), iter));
		return false;
	};
	GtkTreeIter first;
	if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(folderStore), &first)) find(&first);
	gtk_widget_grab_focus(folderView);
}

void App::setWindowPosition(long x, long y) {
	// Wayland doesn't let a program place its own window, so this only works under X11.
	gint currentX = 0, currentY = 0;
	gtk_window_get_position(GTK_WINDOW(window), &currentX, &currentY);
	gtk_window_move(GTK_WINDOW(window), x >= 0 ? static_cast<gint>(x) : currentX, y >= 0 ? static_cast<gint>(y) : currentY);
}

bool App::registerFileTypes(const std::vector<std::string> &extensions, std::string &error) {
	GAppInfo *mine = nullptr;
	GList *all = g_app_info_get_all();
	for (GList *item = all; item; item = item->next) {
		GAppInfo *info = G_APP_INFO(item->data);
		const char *id = g_app_info_get_id(info);
		if (id && std::string(id) == "omapad.desktop") mine = G_APP_INFO(g_object_ref(info));
	}
	g_list_free_full(all, g_object_unref);
	if (!mine) {
		error = "omapad.desktop isn't installed, so there's nothing to register. Install omapad first.";
		return false;
	}
	bool ok = true;
	for (const std::string &extension : extensions) {
		GError *failure = nullptr;
		if (!g_app_info_set_as_default_for_extension(mine, extension.c_str(), &failure)) {
			ok = false;
			if (failure) error = failure->message;
			g_clear_error(&failure);
		}
	}
	g_object_unref(mine);
	return ok;
}

void App::buildToolbar() {
	toolbar = gtk_toolbar_new();
	gtk_toolbar_set_style(GTK_TOOLBAR(toolbar), GTK_TOOLBAR_ICONS);
	gtk_toolbar_set_icon_size(GTK_TOOLBAR(toolbar), GTK_ICON_SIZE_SMALL_TOOLBAR);
	gtk_style_context_add_class(gtk_widget_get_style_context(toolbar), "omapad-toolbar");
	for (const ToolItem &item : workspace->toolbarItems()) {
		GtkToolItem *widget;
		if (item.separator) {
			widget = gtk_separator_tool_item_new();
		} else {
			widget = gtk_tool_button_new(nullptr, item.title.c_str());
			gtk_tool_item_set_tooltip_text(widget, item.title.c_str());
			gtk_widget_set_can_focus(GTK_WIDGET(widget), FALSE);
			const Cmd cmd = item.cmd;
			const int arg = item.arg;
			onSignal(GTK_WIDGET(widget), "clicked", [this, cmd, arg]() { post([this, cmd, arg]() { workspace->run(cmd, arg); }); });
			toolButtons.emplace_back(widget, item);
		}
		gtk_toolbar_insert(GTK_TOOLBAR(toolbar), widget, -1);
	}
	applyToolbarIcons();
}

void App::applyToolbarIcons() {
	GtkIconTheme *theme = gtk_icon_theme_get_default();
	const int pixels = chromeState.largeToolbar ? 24 : 16;
	// Symbolic icons are the line style, and the only ones GTK can recolour.
	const bool lines = !chromeState.filledToolbar || !chromeState.toolbarColour.empty();
	for (auto &button : toolButtons) {
		const ToolItem &item = button.second;
		GtkToolButton *tool = GTK_TOOL_BUTTON(button.first);
		if (!item.customIcon.empty()) {
			GError *failure = nullptr;
			GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_size(item.customIcon.c_str(), pixels, pixels, &failure);
			g_clear_error(&failure);
			if (pixbuf) {
				GtkWidget *image = gtk_image_new_from_pixbuf(pixbuf);
				gtk_widget_show(image);
				gtk_tool_button_set_icon_widget(tool, image);
				g_object_unref(pixbuf);
				continue;
			}
		}
		const std::string symbolic = item.icon + "-symbolic";
		gtk_tool_button_set_icon_name(tool, lines && gtk_icon_theme_has_icon(theme, symbolic.c_str()) ? symbolic.c_str() : item.icon.c_str());
	}
	static GtkCssProvider *colourCss = nullptr;
	if (!colourCss) {
		colourCss = gtk_css_provider_new();
		gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(colourCss), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
	}
	std::string css = " ";
	const std::string &colour = chromeState.toolbarColour;
	if (colour.size() == 6 && colour.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) css = ".omapad-toolbar image { color: #" + colour + "; }";
	gtk_css_provider_load_from_data(colourCss, css.c_str(), -1, nullptr);
}

void App::runIncremental(bool backward, bool next) {
	const std::string status = workspace->incrementalSearch(gtk_entry_get_text(GTK_ENTRY(incrementalEntry)), backward, next,
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(incrementalCase)), gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(incrementalAll)));
	gtk_label_set_text(GTK_LABEL(incrementalStatus), status.c_str());
}

void App::buildIncrementalBar() {
	incrementalBar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_container_set_border_width(GTK_CONTAINER(incrementalBar), 4);
	GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
	gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
	onSignal(close, "clicked", [this]() { workspace->closeIncrementalSearch(); });
	incrementalEntry = gtk_search_entry_new();
	gtk_widget_set_size_request(incrementalEntry, 260, -1);
	GtkWidget *previous = gtk_button_new_from_icon_name("go-up-symbolic", GTK_ICON_SIZE_MENU);
	GtkWidget *next = gtk_button_new_from_icon_name("go-down-symbolic", GTK_ICON_SIZE_MENU);
	gtk_widget_set_tooltip_text(previous, "Previous");
	gtk_widget_set_tooltip_text(next, "Next");
	incrementalAll = gtk_check_button_new_with_label("Highlight all");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(incrementalAll), TRUE);
	incrementalCase = gtk_check_button_new_with_label("Match case");
	incrementalStatus = gtk_label_new("");
	gtk_box_pack_start(GTK_BOX(incrementalBar), close, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(incrementalBar), gtk_label_new("Find:"), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(incrementalBar), incrementalEntry, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(incrementalBar), previous, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(incrementalBar), next, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(incrementalBar), incrementalAll, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(incrementalBar), incrementalCase, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(incrementalBar), incrementalStatus, FALSE, FALSE, 0);
	onSignal(incrementalEntry, "search-changed", [this]() { runIncremental(false, false); });
	onSignal(next, "clicked", [this]() { runIncremental(false, true); });
	onSignal(previous, "clicked", [this]() { runIncremental(true, true); });
	onSignal(incrementalAll, "toggled", [this]() { runIncremental(false, false); });
	onSignal(incrementalCase, "toggled", [this]() { runIncremental(false, false); });
	g_signal_connect(incrementalEntry, "key-press-event", G_CALLBACK(+[](GtkWidget *, GdkEventKey *event, gpointer self) -> gboolean {
		App *app = static_cast<App *>(self);
		if (event->keyval == GDK_KEY_Escape) {
			app->workspace->closeIncrementalSearch();
			return TRUE;
		}
		if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) {
			app->runIncremental((event->state & GDK_SHIFT_MASK) != 0, true);
			return TRUE;
		}
		return FALSE;
	}), this);
}

void App::setIncrementalBar(bool visible) {
	if (notesUiState.enabled) visible = false;
	if (visible) {
		gtk_widget_show_all(incrementalBar);
		gtk_widget_grab_focus(incrementalEntry);
	} else {
		gtk_widget_hide(incrementalBar);
	}
}

void App::setChrome(const ChromeState &chrome) {
	chromeState = chrome;
	menuTemporarilyShown = false;
	gtk_widget_set_visible(menubar, chrome.menu);
	gtk_widget_set_visible(toolbar, chrome.toolbar);
	gtk_toolbar_set_icon_size(GTK_TOOLBAR(toolbar), chrome.largeToolbar ? GTK_ICON_SIZE_LARGE_TOOLBAR : GTK_ICON_SIZE_SMALL_TOOLBAR);
	applyToolbarIcons();
	for (int pane = 0; pane < 2; pane++) gtk_container_set_border_width(GTK_CONTAINER(panes[pane].box), static_cast<guint>(chrome.borderWidth));
	gtk_widget_set_visible(statusBar, chrome.status);
	gtk_window_set_decorated(GTK_WINDOW(window), !chrome.frameless);
	for (int pane = 0; pane < 2; pane++) {
		gtk_notebook_set_show_tabs(GTK_NOTEBOOK(panes[pane].notebook), chrome.tabs && !tabStyle.multiLine);
		syncFlowTabs(pane);
	}
	if (!chrome.panels) {
		gtk_widget_hide(folderBox);
		gtk_widget_hide(sideBox);
	} else {
		setFolderPanelVisible(workspace->folderPanelVisible());
		gtk_widget_set_visible(functionBox, workspace->functionListVisible());
		gtk_widget_set_visible(map.widget, workspace->documentMapVisible());
		arrangeSidePanels();
	}
	if (chrome.appearance != "system")
		g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", chrome.appearance == "dark" ? TRUE : FALSE, nullptr);
	applyNotesLayout();
}

void App::setTabStyle(const TabStyle &style) {
	tabStyle = style;
	static GtkCssProvider *tabCss = nullptr;
	if (!tabCss) {
		tabCss = gtk_css_provider_new();
		gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(tabCss), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
		gtk_css_provider_load_from_data(tabCss,
			".omapad-tab-colour-1 { background-color: alpha(#e0c040, 0.45); }\n"
			".omapad-tab-colour-2 { background-color: alpha(#60c060, 0.45); }\n"
			".omapad-tab-colour-3 { background-color: alpha(#5090e0, 0.45); }\n"
			".omapad-tab-colour-4 { background-color: alpha(#e07050, 0.45); }\n"
			".omapad-tab-colour-5 { background-color: alpha(#b070e0, 0.45); }\n"
			".omapad-reduced tab { min-height: 0; padding: 1px 4px; font-size: 90%; }\n"
			".omapad-no-bar tab:checked { box-shadow: none; border-bottom-color: transparent; }\n"
			".omapad-plain-inactive tab:not(:checked) { background-color: transparent; }\n", -1, nullptr);
	}
	for (int pane = 0; pane < 2; pane++) {
		GtkWidget *notebook = panes[pane].notebook;
		gtk_notebook_set_tab_pos(GTK_NOTEBOOK(notebook), style.vertical ? GTK_POS_LEFT : GTK_POS_TOP);
		// A vertical tab bar runs down the side of the editor, so the notebook has to take its height.
		gtk_orientable_set_orientation(GTK_ORIENTABLE(panes[pane].box), style.vertical ? GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL);
		GtkStyleContext *context = gtk_widget_get_style_context(notebook);
		const auto setClass = [&](const char *name, bool on) {
			if (on) gtk_style_context_add_class(context, name);
			else gtk_style_context_remove_class(context, name);
		};
		setClass("omapad-reduced", style.reduced);
		setClass("omapad-no-bar", !style.activeBar);
		setClass("omapad-plain-inactive", !style.inactiveColour);
		gtk_notebook_set_show_tabs(GTK_NOTEBOOK(notebook), chromeState.tabs && !style.multiLine);
		if (tabShortcuts[pane]) gtk_widget_set_visible(tabShortcuts[pane], style.rightShortcuts);
	}
	tabsChanged();
}

// GTK's notebook can't wrap its tabs, so multi-line tabs are a row of buttons above it.
void App::syncFlowTabs(int pane) {
	PaneWidgets &p = panes[pane];
	const bool wanted = tabStyle.multiLine && chromeState.tabs && !notesUiState.enabled;
	if (!wanted) {
		if (flowTabs[pane]) gtk_widget_hide(flowTabs[pane]);
		return;
	}
	if (!flowTabs[pane]) {
		flowTabs[pane] = gtk_flow_box_new();
		gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flowTabs[pane]), GTK_SELECTION_NONE);
		gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flowTabs[pane]), 40);
		gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flowTabs[pane]), FALSE);
		gtk_box_pack_start(GTK_BOX(p.box), flowTabs[pane], FALSE, FALSE, 0);
		gtk_box_reorder_child(GTK_BOX(p.box), flowTabs[pane], 0);
	}
	GList *children = gtk_container_get_children(GTK_CONTAINER(flowTabs[pane]));
	for (GList *child = children; child; child = child->next) gtk_widget_destroy(GTK_WIDGET(child->data));
	g_list_free(children);
	const std::vector<TabInfo> tabs = workspace->tabs(pane);
	const int active = workspace->activeTab(pane);
	for (int i = 0; i < static_cast<int>(tabs.size()); i++) {
		const std::string title = tabDisplayTitle(tabs[i], tabStyle);
		GtkWidget *button = gtk_toggle_button_new_with_label(title.c_str());
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), i == active);
		gtk_widget_set_can_focus(button, FALSE);
		gtk_widget_set_tooltip_text(button, tabs[i].toolTip.c_str());
		if (tabs[i].colour > 0) gtk_style_context_add_class(gtk_widget_get_style_context(button), ("omapad-tab-colour-" + std::to_string(tabs[i].colour)).c_str());
		onSignal(button, "clicked", [this, pane, i]() { post([this, pane, i]() { workspace->clickTab(pane, i); }); });
		struct Binding { App *app; int pane, index; };
		g_signal_connect_data(button, "button-press-event", G_CALLBACK(+[](GtkWidget *, GdkEventButton *event, gpointer data) -> gboolean {
			auto *b = static_cast<Binding *>(data);
			if (event->button == GDK_BUTTON_SECONDARY) {
				b->app->popup(b->app->workspace->tabMenu(b->pane, b->index));
				return TRUE;
			}
			if (event->type == GDK_2BUTTON_PRESS) {
				App *app = b->app;
				const int pane = b->pane;
				const int index = b->index;
				app->post([app, pane, index]() { app->workspace->tabDoubleClicked(pane, index); });
				return TRUE;
			}
			return FALSE;
		}), new Binding{this, pane, i}, +[](gpointer data, GClosure *) { delete static_cast<Binding *>(data); }, GConnectFlags(0));
		gtk_container_add(GTK_CONTAINER(flowTabs[pane]), button);
	}
	gtk_widget_show_all(flowTabs[pane]);
}

#pragma mark - Dialogs

int App::ask(const std::string &title, const std::string &detail, const std::vector<std::string> &buttons) {
	GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_MODAL, GTK_MESSAGE_OTHER, GTK_BUTTONS_NONE, "%s", title.c_str());
	if (!detail.empty()) gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", detail.c_str());
	for (int i = 0; i < static_cast<int>(buttons.size()); i++) gtk_dialog_add_button(GTK_DIALOG(dialog), buttons[i].c_str(), i);
	gtk_dialog_set_default_response(GTK_DIALOG(dialog), 0);
	dialogDepth++;
	const int response = gtk_dialog_run(GTK_DIALOG(dialog));
	dialogDepth--;
	gtk_widget_destroy(dialog);
	if (response >= 0) return response;
	// Escape or closing the dialog must mean the safe choice, never "Don't Save".
	for (const char *safe : {"Cancel", "Don't Quit", "Keep Current Text"})
		for (int i = 0; i < static_cast<int>(buttons.size()); i++)
			if (buttons[i] == safe) return i;
	return 0;
}

bool App::chooseFilesToOpen(const std::string &start, std::vector<std::string> &out) {
	GtkFileChooserNative *chooser = gtk_file_chooser_native_new("Open", GTK_WINDOW(window), GTK_FILE_CHOOSER_ACTION_OPEN, "_Open", "_Cancel");
	gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(chooser), TRUE);
	if (!start.empty()) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), start.c_str());
	dialogDepth++;
	const bool ok = gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT;
	dialogDepth--;
	if (ok) {
		GSList *names = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(chooser));
		for (GSList *name = names; name; name = name->next) out.emplace_back(static_cast<char *>(name->data));
		g_slist_free_full(names, g_free);
	}
	g_object_unref(chooser);
	return ok;
}

bool App::chooseSavePath(const std::string &name, const std::string &start, std::string &out) {
	GtkFileChooserNative *chooser = gtk_file_chooser_native_new("Save As", GTK_WINDOW(window), GTK_FILE_CHOOSER_ACTION_SAVE, "_Save", "_Cancel");
	gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(chooser), TRUE);
	gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(chooser), name.c_str());
	if (!start.empty()) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), start.c_str());
	dialogDepth++;
	const bool ok = gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT;
	dialogDepth--;
	if (ok) {
		char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
		if (path) out = path;
		g_free(path);
	}
	g_object_unref(chooser);
	return ok && !out.empty();
}

bool App::chooseFolder(const std::string &start, std::string &out) {
	GtkFileChooserNative *chooser = gtk_file_chooser_native_new("Open Folder", GTK_WINDOW(window), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "_Open", "_Cancel");
	if (!start.empty()) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), start.c_str());
	dialogDepth++;
	const bool ok = gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT;
	dialogDepth--;
	if (ok) {
		char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
		if (path) out = path;
		g_free(path);
	}
	g_object_unref(chooser);
	return ok && !out.empty();
}

bool App::chooseFont(std::string &family, double &size) {
	GtkWidget *dialog = gtk_font_chooser_dialog_new("Font", GTK_WINDOW(window));
	const std::string current = family + " " + std::to_string(static_cast<int>(size));
	gtk_font_chooser_set_font(GTK_FONT_CHOOSER(dialog), current.c_str());
	dialogDepth++;
	const bool ok = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK;
	dialogDepth--;
	if (ok) {
		PangoFontDescription *description = gtk_font_chooser_get_font_desc(GTK_FONT_CHOOSER(dialog));
		if (description) {
			const char *name = pango_font_description_get_family(description);
			if (name) family = name;
			const int pangoSize = pango_font_description_get_size(description);
			if (pangoSize > 0) size = static_cast<double>(pangoSize) / PANGO_SCALE;
			pango_font_description_free(description);
		}
	}
	gtk_widget_destroy(dialog);
	return ok;
}

bool App::promptText(const std::string &title, const std::string &label, const std::string &initial, std::string &out) {
	GtkWidget *dialog = gtk_dialog_new_with_buttons(title.c_str(), GTK_WINDOW(window), GTK_DIALOG_MODAL, "Cancel", GTK_RESPONSE_CANCEL, "OK", GTK_RESPONSE_OK, nullptr);
	gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
	GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	gtk_container_set_border_width(GTK_CONTAINER(content), 12);
	gtk_box_set_spacing(GTK_BOX(content), 8);
	GtkWidget *text = gtk_label_new(label.c_str());
	gtk_label_set_xalign(GTK_LABEL(text), 0);
	GtkWidget *entry = gtk_entry_new();
	gtk_entry_set_text(GTK_ENTRY(entry), initial.c_str());
	gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
	gtk_entry_set_width_chars(GTK_ENTRY(entry), 36);
	gtk_box_pack_start(GTK_BOX(content), text, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(content), entry, FALSE, FALSE, 0);
	gtk_widget_show_all(content);
	dialogDepth++;
	const bool ok = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK;
	dialogDepth--;
	if (ok) out = gtk_entry_get_text(GTK_ENTRY(entry));
	gtk_widget_destroy(dialog);
	return ok;
}

namespace {

GtkWidget *newFormGrid() {
	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
	gtk_container_set_border_width(GTK_CONTAINER(grid), 14);
	return grid;
}

GtkWidget *scrollingForm(GtkWidget *grid) {
	GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
	gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 620);
	gtk_container_add(GTK_CONTAINER(scroll), grid);
	return scroll;
}

GdkRGBA colourFromHex(const std::string &hex) {
	const unsigned long value = std::strtoul(hex.c_str(), nullptr, 16);
	return GdkRGBA{((value >> 16) & 0xFF) / 255.0, ((value >> 8) & 0xFF) / 255.0, (value & 0xFF) / 255.0, 1};
}

std::string hexFromColour(const GdkRGBA &colour) {
	char out[8];
	std::snprintf(out, sizeof out, "%02X%02X%02X", static_cast<int>(colour.red * 255 + 0.5), static_cast<int>(colour.green * 255 + 0.5), static_cast<int>(colour.blue * 255 + 0.5));
	return out;
}

struct StyleRowWidgets {
	GtkWidget *fgOn, *fg, *bgOn, *bg, *bold, *italic, *underline;
	GtkWidget *fontName = nullptr, *fontSize = nullptr;
};

GtkWidget *makeColourPair(const char *label, const std::string &hex, GtkWidget *box, GtkWidget *&toggle, GtkWidget *&button) {
	toggle = gtk_check_button_new_with_label(label);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), !hex.empty());
	const GdkRGBA colour = colourFromHex(hex.empty() ? "808080" : hex);
	button = gtk_color_button_new_with_rgba(&colour);
	// Picking a colour means using it.
	g_signal_connect(button, "color-set", G_CALLBACK(+[](GtkColorButton *, gpointer on) { gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(on), TRUE); }), toggle);
	gtk_box_pack_start(GTK_BOX(box), toggle, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
	return button;
}

std::string longTextValue(GtkWidget *view) {
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
	GtkTextIter start, end;
	gtk_text_buffer_get_bounds(buffer, &start, &end);
	char *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
	std::string out = text;
	g_free(text);
	return out;
}

}  // namespace

bool App::runForm(const std::string &title, std::vector<FormField> &fields) {
	bool hasPages = false;
	for (const FormField &field : fields) hasPages = hasPages || field.kind == FormField::Kind::Page;
	GtkWidget *dialog = gtk_dialog_new_with_buttons(title.c_str(), GTK_WINDOW(window), GTK_DIALOG_MODAL, "Cancel", GTK_RESPONSE_CANCEL, "OK", GTK_RESPONSE_OK, nullptr);
	gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
	// Short forms don't need all the room a long one does.
	gtk_window_set_default_size(GTK_WINDOW(dialog), hasPages ? 820 : 520, hasPages ? 680 : fields.size() > 8 ? 620 : -1);
	GtkWidget *notebook = hasPages ? gtk_notebook_new() : nullptr;
	GtkWidget *grid = newFormGrid();
	GtkWidget *singleGrid = grid;
	std::string pageTitle;
	const auto finishPage = [&]() {
		GtkWidget *scroll = scrollingForm(grid);
		gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scroll, gtk_label_new(pageTitle.c_str()));
	};
	std::vector<GtkWidget *> controls(fields.size(), nullptr);
	std::vector<StyleRowWidgets> styleRows(fields.size());
	std::vector<std::vector<GtkWidget *>> flagBoxes(fields.size());
	int row = 0;
	for (size_t i = 0; i < fields.size(); i++) {
		const FormField &field = fields[i];
		switch (field.kind) {
		case FormField::Kind::Page: {
			if (!pageTitle.empty()) {
				finishPage();
				grid = newFormGrid();
				row = 0;
			}
			pageTitle = field.label;
			break;
		}
		case FormField::Kind::Heading: {
			GtkWidget *heading = gtk_label_new(nullptr);
			char *markup = g_markup_printf_escaped("<b>%s</b>", field.label.c_str());
			gtk_label_set_markup(GTK_LABEL(heading), markup);
			g_free(markup);
			gtk_label_set_xalign(GTK_LABEL(heading), 0);
			if (row > 0) gtk_widget_set_margin_top(heading, 12);
			gtk_grid_attach(GTK_GRID(grid), heading, 0, row++, 2, 1);
			break;
		}
		case FormField::Kind::Bool: {
			GtkWidget *box = gtk_check_button_new_with_label(field.label.c_str());
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(box), field.value == "1");
			gtk_grid_attach(GTK_GRID(grid), box, 0, row++, 2, 1);
			controls[i] = box;
			break;
		}
		case FormField::Kind::LongText: {
			GtkWidget *label = gtk_label_new(field.label.c_str());
			gtk_label_set_xalign(GTK_LABEL(label), 0);
			gtk_grid_attach(GTK_GRID(grid), label, 0, row++, 2, 1);
			GtkWidget *text = gtk_text_view_new();
			gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text), GTK_WRAP_WORD_CHAR);
			gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(text)), field.value.c_str(), -1);
			GtkWidget *holder = gtk_scrolled_window_new(nullptr, nullptr);
			gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(holder), GTK_SHADOW_IN);
			gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(holder), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
			gtk_widget_set_size_request(holder, -1, 58);
			gtk_widget_set_hexpand(holder, TRUE);
			gtk_container_add(GTK_CONTAINER(holder), text);
			gtk_grid_attach(GTK_GRID(grid), holder, 0, row++, 2, 1);
			controls[i] = text;
			break;
		}
		case FormField::Kind::StyleRow: {
			const size_t first = field.value.find(',');
			const size_t second = first == std::string::npos ? first : field.value.find(',', first + 1);
			const std::string fg = second == std::string::npos ? "" : field.value.substr(0, first);
			const std::string bg = second == std::string::npos ? "" : field.value.substr(first + 1, second - first - 1);
			const int fontStyle = second == std::string::npos ? 0 : std::atoi(field.value.c_str() + second + 1);
			GtkWidget *label = gtk_label_new(field.label.c_str());
			gtk_label_set_xalign(GTK_LABEL(label), 0);
			gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
			GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
			StyleRowWidgets &widgets = styleRows[i];
			makeColourPair("Text", fg, box, widgets.fgOn, widgets.fg);
			makeColourPair("Back", bg, box, widgets.bgOn, widgets.bg);
			const auto fontBox = [&](const char *name, int bit) {
				GtkWidget *check = gtk_check_button_new_with_label(name);
				gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), (fontStyle & bit) != 0);
				gtk_box_pack_start(GTK_BOX(box), check, FALSE, FALSE, 0);
				return check;
			};
			widgets.bold = fontBox("Bold", 1);
			widgets.italic = fontBox("Italic", 2);
			widgets.underline = fontBox("Underline", 4);
			{
				// The font's name and size come after the third comma, for rows that have them.
				const size_t third = second == std::string::npos ? second : field.value.find(',', second + 1);
				const size_t fourth = third == std::string::npos ? third : field.value.find(',', third + 1);
				if (fourth != std::string::npos) {
					widgets.fontName = gtk_entry_new();
					gtk_entry_set_placeholder_text(GTK_ENTRY(widgets.fontName), "Font");
					gtk_entry_set_width_chars(GTK_ENTRY(widgets.fontName), 14);
					gtk_entry_set_text(GTK_ENTRY(widgets.fontName), field.value.substr(third + 1, fourth - third - 1).c_str());
					widgets.fontSize = gtk_spin_button_new_with_range(0, 72, 1);
					gtk_widget_set_tooltip_text(widgets.fontSize, "Size. 0 keeps the editor's size.");
					gtk_spin_button_set_value(GTK_SPIN_BUTTON(widgets.fontSize), std::atoi(field.value.c_str() + fourth + 1));
					gtk_box_pack_start(GTK_BOX(box), widgets.fontName, FALSE, FALSE, 0);
					gtk_box_pack_start(GTK_BOX(box), widgets.fontSize, FALSE, FALSE, 0);
				}
			}
			gtk_grid_attach(GTK_GRID(grid), box, 1, row++, 1, 1);
			break;
		}
		case FormField::Kind::Flags: {
			GtkWidget *heading = gtk_label_new(nullptr);
			char *markup = g_markup_printf_escaped("<b>%s</b>", field.label.c_str());
			gtk_label_set_markup(GTK_LABEL(heading), markup);
			g_free(markup);
			gtk_label_set_xalign(GTK_LABEL(heading), 0);
			if (row > 0) gtk_widget_set_margin_top(heading, 8);
			gtk_grid_attach(GTK_GRID(grid), heading, 0, row++, 2, 1);
			GtkWidget *boxes = gtk_grid_new();
			gtk_grid_set_column_spacing(GTK_GRID(boxes), 12);
			const long value = std::strtol(field.value.c_str(), nullptr, 10);
			int index = 0;
			for (const auto &choice : field.choices) {
				GtkWidget *check = gtk_check_button_new_with_label(choice.second.c_str());
				gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), (value & std::strtol(choice.first.c_str(), nullptr, 10)) != 0);
				gtk_grid_attach(GTK_GRID(boxes), check, index % 5, index / 5, 1, 1);
				flagBoxes[i].push_back(check);
				index++;
			}
			gtk_grid_attach(GTK_GRID(grid), boxes, 0, row++, 2, 1);
			break;
		}
		default: {
			GtkWidget *label = gtk_label_new(field.label.c_str());
			gtk_label_set_xalign(GTK_LABEL(label), 0);
			gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
			GtkWidget *control;
			if (field.kind == FormField::Kind::Int) {
				control = gtk_spin_button_new_with_range(field.minimum, field.maximum, 1);
				gtk_spin_button_set_value(GTK_SPIN_BUTTON(control), std::strtod(field.value.c_str(), nullptr));
				gtk_widget_set_halign(control, GTK_ALIGN_START);
			} else if (field.kind == FormField::Kind::Text) {
				control = gtk_entry_new();
				gtk_entry_set_text(GTK_ENTRY(control), field.value.c_str());
				gtk_entry_set_activates_default(GTK_ENTRY(control), TRUE);
				gtk_widget_set_hexpand(control, TRUE);
			} else {
				control = gtk_combo_box_text_new();
				int active = 0, index = 0;
				for (const auto &choice : field.choices) {
					gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(control), choice.second.c_str());
					if (choice.first == field.value) active = index;
					index++;
				}
				gtk_combo_box_set_active(GTK_COMBO_BOX(control), active);
				gtk_widget_set_halign(control, GTK_ALIGN_START);
			}
			gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
			gtk_grid_attach(GTK_GRID(grid), control, 1, row++, 1, 1);
			controls[i] = control;
			break;
		}
		}
	}
	GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	if (hasPages) {
		if (!pageTitle.empty()) finishPage();
		gtk_box_pack_start(GTK_BOX(content), notebook, TRUE, TRUE, 0);
	} else {
		gtk_box_pack_start(GTK_BOX(content), scrollingForm(singleGrid), TRUE, TRUE, 0);
	}
	gtk_widget_show_all(content);
	dialogDepth++;
	const bool ok = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK;
	dialogDepth--;
	if (ok) {
		for (size_t i = 0; i < fields.size(); i++) {
			FormField &field = fields[i];
			GtkWidget *control = controls[i];
			switch (field.kind) {
			case FormField::Kind::Heading:
			case FormField::Kind::Page: break;
			case FormField::Kind::LongText: field.value = longTextValue(control); break;
			case FormField::Kind::StyleRow: {
				const StyleRowWidgets &widgets = styleRows[i];
				const auto colourOf = [](GtkWidget *on, GtkWidget *button) {
					if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(on))) return std::string();
					GdkRGBA colour;
					gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &colour);
					return hexFromColour(colour);
				};
				const auto isOn = [](GtkWidget *check) { return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check)) != FALSE; };
				const int fontStyle = (isOn(widgets.bold) ? 1 : 0) | (isOn(widgets.italic) ? 2 : 0) | (isOn(widgets.underline) ? 4 : 0);
				field.value = colourOf(widgets.fgOn, widgets.fg) + "," + colourOf(widgets.bgOn, widgets.bg) + "," + std::to_string(fontStyle);
				if (widgets.fontName) {
					const int size = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widgets.fontSize));
					field.value += std::string(",") + gtk_entry_get_text(GTK_ENTRY(widgets.fontName)) + "," + (size > 0 ? std::to_string(size) : std::string());
				}
				break;
			}
			case FormField::Kind::Flags: {
				long value = 0;
				for (size_t choice = 0; choice < field.choices.size(); choice++)
					if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(flagBoxes[i][choice]))) value |= std::strtol(field.choices[choice].first.c_str(), nullptr, 10);
				field.value = std::to_string(value);
				break;
			}
			case FormField::Kind::Bool: field.value = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(control)) ? "1" : "0"; break;
			case FormField::Kind::Int:
				gtk_spin_button_update(GTK_SPIN_BUTTON(control));
				field.value = std::to_string(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(control)));
				break;
			case FormField::Kind::Text: field.value = gtk_entry_get_text(GTK_ENTRY(control)); break;
			case FormField::Kind::Choice: {
				const int active = gtk_combo_box_get_active(GTK_COMBO_BOX(control));
				if (active >= 0 && active < static_cast<int>(field.choices.size())) field.value = field.choices[active].first;
				break;
			}
			}
		}
	}
	gtk_widget_destroy(dialog);
	return ok;
}

#pragma mark - Printing

namespace {

struct PrintRun {
	PrintJob *job;
	double scale;  // From Scintilla's units to points. Pango sizes fonts for the screen's resolution.
};

void drawCaption(cairo_t *cr, GtkPrintContext *context, const PrintPageText &text, double y, double width, bool bold, bool italic) {
	PangoLayout *layout = gtk_print_context_create_pango_layout(context);
	PangoFontDescription *font = pango_font_description_from_string("sans");
	pango_font_description_set_size(font, PrintJob::kCaptionPoints * PANGO_SCALE);
	if (bold) pango_font_description_set_weight(font, PANGO_WEIGHT_BOLD);
	if (italic) pango_font_description_set_style(font, PANGO_STYLE_ITALIC);
	pango_layout_set_font_description(layout, font);
	pango_font_description_free(font);
	cairo_set_source_rgb(cr, 0, 0, 0);
	const auto draw = [&](const std::string &part, double align) {
		if (part.empty()) return;
		pango_layout_set_text(layout, part.c_str(), -1);
		int textWidth = 0;
		pango_layout_get_size(layout, &textWidth, nullptr);
		cairo_move_to(cr, (width - textWidth / static_cast<double>(PANGO_SCALE)) * align, y);
		pango_cairo_show_layout(cr, layout);
	};
	draw(text.left, 0);
	draw(text.middle, 0.5);
	draw(text.right, 1);
	g_object_unref(layout);
}

}  // namespace

bool App::print(PrintJob &job, const PrintOutput &output, std::string &error) {
	GtkPrintOperation *operation = gtk_print_operation_new();
	gtk_print_operation_set_unit(operation, GTK_UNIT_POINTS);
	gtk_print_operation_set_job_name(operation, job.settings().title.c_str());
	GtkPageSetup *setup = gtk_page_setup_new();
	const PrintOptions &options = job.settings();
	gtk_page_setup_set_top_margin(setup, options.marginTop, GTK_UNIT_MM);
	gtk_page_setup_set_bottom_margin(setup, options.marginBottom, GTK_UNIT_MM);
	gtk_page_setup_set_left_margin(setup, options.marginLeft, GTK_UNIT_MM);
	gtk_page_setup_set_right_margin(setup, options.marginRight, GTK_UNIT_MM);
	gtk_print_operation_set_default_page_setup(operation, setup);
	g_object_unref(setup);
	if (printSettings) gtk_print_operation_set_print_settings(operation, printSettings);

	double resolution = gdk_screen_get_resolution(gdk_screen_get_default());
	if (resolution <= 0) resolution = 96;
	PrintRun run{&job, 72.0 / resolution};
	g_signal_connect(operation, "begin-print", G_CALLBACK(+[](GtkPrintOperation *op, GtkPrintContext *context, gpointer data) {
		auto *r = static_cast<PrintRun *>(data);
		cairo_t *cr = gtk_print_context_get_cairo_context(context);
		cairo_save(cr);
		cairo_scale(cr, r->scale, r->scale);
		r->job->paginate(cr, static_cast<int>(gtk_print_context_get_width(context) / r->scale), static_cast<int>(gtk_print_context_get_height(context) / r->scale));
		cairo_restore(cr);
		gtk_print_operation_set_n_pages(op, r->job->pageCount());
	}), &run);
	g_signal_connect(operation, "draw-page", G_CALLBACK(+[](GtkPrintOperation *, GtkPrintContext *context, gint page, gpointer data) {
		auto *r = static_cast<PrintRun *>(data);
		cairo_t *cr = gtk_print_context_get_cairo_context(context);
		const double width = gtk_print_context_get_width(context);
		const double height = gtk_print_context_get_height(context);
		cairo_save(cr);
		cairo_scale(cr, r->scale, r->scale);
		r->job->drawPage(cr, page, static_cast<int>(width / r->scale), static_cast<int>(height / r->scale));
		cairo_restore(cr);
		if (r->job->headerHeight() > 0) drawCaption(cr, context, r->job->header(page), 0, width, r->job->settings().headerBold, r->job->settings().headerItalic);
		if (r->job->footerHeight() > 0) drawCaption(cr, context, r->job->footer(page), height - r->job->footerHeight() * r->scale + 2, width, r->job->settings().footerBold, r->job->settings().footerItalic);
	}), &run);

	const bool toFile = !output.pdfPath.empty();
	if (toFile) gtk_print_operation_set_export_filename(operation, output.pdfPath.c_str());
	GError *failure = nullptr;
	dialogDepth++;
	const GtkPrintOperationResult result = gtk_print_operation_run(operation,
		toFile ? GTK_PRINT_OPERATION_ACTION_EXPORT : output.showDialog ? GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG : GTK_PRINT_OPERATION_ACTION_PRINT,
		GTK_WINDOW(window), &failure);
	dialogDepth--;
	bool ok = true;
	if (result == GTK_PRINT_OPERATION_RESULT_ERROR) {
		error = failure ? failure->message : "Printing failed.";
		ok = false;
	} else if (result == GTK_PRINT_OPERATION_RESULT_APPLY) {
		if (printSettings) g_object_unref(printSettings);
		printSettings = GTK_PRINT_SETTINGS(g_object_ref(gtk_print_operation_get_print_settings(operation)));
	}
	if (failure) g_error_free(failure);
	g_object_unref(operation);
	return ok;
}

namespace {

struct ShortcutMapper {
	App *app = nullptr;
	Workspace *workspace = nullptr;
	GtkWidget *dialog = nullptr, *view = nullptr, *search = nullptr, *hint = nullptr, *category = nullptr;
	GtkListStore *store = nullptr;
	std::string capturing;
	std::vector<ShortcutEntry> entries;

	static const char *idleHint() { return "Double-click a command, then press its new shortcut."; }

	std::string selectedId() const {
		GtkTreeIter iter;
		GtkTreeModel *model = nullptr;
		if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), &model, &iter)) return {};
		gchar *id = nullptr;
		gtk_tree_model_get(model, &iter, ScId, &id, -1);
		const std::string out = id ? id : "";
		g_free(id);
		return out;
	}

	void reload() {
		const std::string selected = selectedId();
		const auto lower = [](std::string value) {
			std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(g_ascii_tolower(c)); });
			return value;
		};
		const std::string filter = lower(gtk_entry_get_text(GTK_ENTRY(search)));
		entries = workspace->shortcutEntries();
		gtk_list_store_clear(store);
		gchar *chosen = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(category));
		const std::string shownCategory = chosen ? chosen : "";
		g_free(chosen);
		for (const ShortcutEntry &entry : entries) {
			if (entry.category != shownCategory) continue;
			if (!filter.empty() && lower(entry.id).find(filter) == std::string::npos && lower(entry.shortcut).find(filter) == std::string::npos) continue;
			std::string shown = entry.shortcut;
			const size_t mod = shown.find("Mod+");
			if (mod != std::string::npos) shown.replace(mod, 4, "Ctrl+");
			std::string command = entry.id;
			for (size_t at = command.find('/'); at != std::string::npos; at = command.find('/', at + 3)) command.replace(at, 1, " › ");
			GtkTreeIter iter;
			gtk_list_store_append(store, &iter);
			gtk_list_store_set(store, &iter, ScCommand, command.c_str(), ScShortcut, shown.c_str(), ScId, entry.id.c_str(),
			                   ScWeight, entry.shortcut != entry.defaultShortcut ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL, -1);
			if (entry.id == selected) {
				gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), &iter);
				GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(store), &iter);
				gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(view), path, nullptr, FALSE, 0, 0);
				gtk_tree_path_free(path);
			}
		}
	}

	void startCapture() {
		capturing = selectedId();
		if (capturing.empty()) return;
		gtk_label_set_text(GTK_LABEL(hint), "Press the new shortcut, or Esc to cancel.");
		gtk_widget_grab_focus(view);
	}

	void stopCapture() {
		capturing.clear();
		gtk_label_set_text(GTK_LABEL(hint), idleHint());
	}
};

}  // namespace

void App::runShortcutMapper() {
	ShortcutMapper m;
	m.app = this;
	m.workspace = workspace.get();
	m.dialog = gtk_dialog_new_with_buttons("Shortcut Mapper", GTK_WINDOW(window), GTK_DIALOG_MODAL, nullptr, nullptr);
	gtk_window_set_default_size(GTK_WINDOW(m.dialog), 640, 560);

	m.search = gtk_search_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(m.search), "Filter commands or shortcuts");
	m.category = gtk_combo_box_text_new();
	for (const char *name : {"Main menu", "Macros", "Run commands", "Plugin commands", "Scintilla commands"}) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(m.category), name);
	gtk_combo_box_set_active(GTK_COMBO_BOX(m.category), 0);
	m.store = gtk_list_store_new(ScCount, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
	m.view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(m.store));
	gtk_tree_view_set_enable_search(GTK_TREE_VIEW(m.view), FALSE);
	GtkCellRenderer *commandText = gtk_cell_renderer_text_new();
	g_object_set(commandText, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, nullptr);
	GtkTreeViewColumn *commandColumn = gtk_tree_view_column_new_with_attributes("Command", commandText, "text", ScCommand, nullptr);
	gtk_tree_view_column_set_expand(commandColumn, TRUE);
	gtk_tree_view_append_column(GTK_TREE_VIEW(m.view), commandColumn);
	gtk_tree_view_append_column(GTK_TREE_VIEW(m.view), gtk_tree_view_column_new_with_attributes("Shortcut", gtk_cell_renderer_text_new(), "text", ScShortcut, "weight", ScWeight, nullptr));
	GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
	gtk_container_add(GTK_CONTAINER(scroll), m.view);
	m.hint = gtk_label_new(ShortcutMapper::idleHint());
	gtk_label_set_xalign(GTK_LABEL(m.hint), 0);

	ShortcutMapper *mapper = &m;
	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	const auto addButton = [&](const char *label, std::function<void()> action, bool end) {
		GtkWidget *button = gtk_button_new_with_label(label);
		onSignal(button, "clicked", std::move(action));
		if (end) gtk_box_pack_end(GTK_BOX(buttons), button, FALSE, FALSE, 0);
		else gtk_box_pack_start(GTK_BOX(buttons), button, FALSE, FALSE, 0);
	};
	addButton("Set…", [mapper]() { mapper->startCapture(); }, false);
	addButton("Clear", [mapper]() {
		const std::string id = mapper->selectedId();
		mapper->stopCapture();
		if (id.empty()) return;
		mapper->workspace->assignShortcut(id, "");
		mapper->reload();
	}, false);
	addButton("Reset", [mapper]() {
		const std::string id = mapper->selectedId();
		mapper->stopCapture();
		for (const ShortcutEntry &entry : mapper->entries) {
			if (entry.id != id) continue;
			mapper->workspace->assignShortcut(id, entry.defaultShortcut);
			break;
		}
		mapper->reload();
	}, false);
	addButton("Delete…", [this, mapper]() {
		const std::string id = mapper->selectedId();
		mapper->stopCapture();
		if (id.empty()) return;
		if (id.rfind("Macro/", 0) != 0 && id.rfind("Run/", 0) != 0) {
			ask("Only saved macros and run commands can be deleted.", "", {"OK"});
			return;
		}
		if (ask("Delete “" + id + "”?", "", {"Cancel", "Delete"}) != 1) return;
		mapper->workspace->deleteShortcutCommand(id);
		mapper->reload();
	}, false);
	addButton("Reset All…", [this, mapper]() {
		mapper->stopCapture();
		if (ask("Reset every shortcut to its default?", "", {"Cancel", "Reset All"}) != 1) return;
		mapper->workspace->resetShortcuts();
		mapper->reload();
	}, false);
	addButton("Done", [mapper]() { gtk_dialog_response(GTK_DIALOG(mapper->dialog), GTK_RESPONSE_CLOSE); }, true);

	GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(m.dialog));
	gtk_container_set_border_width(GTK_CONTAINER(content), 12);
	gtk_box_set_spacing(GTK_BOX(content), 8);
	gtk_box_pack_start(GTK_BOX(content), m.category, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(content), m.search, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(content), scroll, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(content), m.hint, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(content), buttons, FALSE, FALSE, 0);

	onSignal(m.search, "search-changed", [mapper]() { mapper->reload(); });
	onSignal(m.category, "changed", [mapper]() { mapper->reload(); });
	g_signal_connect(m.view, "row-activated", G_CALLBACK(+[](GtkTreeView *, GtkTreePath *, GtkTreeViewColumn *, gpointer data) {
		static_cast<ShortcutMapper *>(data)->startCapture();
	}), mapper);
	// Connected to run first, so a key being captured never reaches the menus or the dialog.
	g_signal_connect(m.dialog, "key-press-event", G_CALLBACK(+[](GtkWidget *, GdkEventKey *event, gpointer data) -> gboolean {
		auto *self = static_cast<ShortcutMapper *>(data);
		if (self->capturing.empty()) return FALSE;
		if (event->keyval == GDK_KEY_Escape) {
			self->stopCapture();
			return TRUE;
		}
		if (event->is_modifier) return TRUE;
		const std::string shortcut = shortcutFromEvent(event);
		if (shortcut.empty()) {
			gtk_label_set_text(GTK_LABEL(self->hint), "That key types text. Add Control or Alt, or press Esc to cancel.");
			return TRUE;
		}
		const std::string id = self->capturing;
		self->stopCapture();
		self->workspace->assignShortcut(id, shortcut);
		self->reload();
		return TRUE;
	}), mapper);

	m.reload();
	gtk_widget_show_all(content);
	dialogDepth++;
	gtk_dialog_run(GTK_DIALOG(m.dialog));
	dialogDepth--;
	gtk_widget_destroy(m.dialog);
	g_object_unref(m.store);
}

void App::reveal(const std::string &path) {
	GFile *file = g_file_new_for_path(path.c_str());
	GError *error = nullptr;
	// Ask the file manager to select it, and fall back to just opening its folder.
	char *uri = g_file_get_uri(file);
	GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
	bool shown = false;
	if (bus) {
		const char *uris[] = {uri, nullptr};
		GVariant *result = g_dbus_connection_call_sync(bus, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1", "org.freedesktop.FileManager1", "ShowItems",
			g_variant_new("(^ass)", uris, ""), nullptr, G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, nullptr);
		if (result) {
			shown = true;
			g_variant_unref(result);
		}
		g_object_unref(bus);
	}
	if (!shown) {
		GFile *parent = g_file_get_parent(file);
		if (parent) {
			char *parentUri = g_file_get_uri(parent);
			if (!gtk_show_uri_on_window(GTK_WINDOW(window), parentUri, GDK_CURRENT_TIME, &error)) {
				flash(error ? error->message : "The file manager couldn't be opened");
				g_clear_error(&error);
			}
			g_free(parentUri);
			g_object_unref(parent);
		}
	}
	g_free(uri);
	g_object_unref(file);
}

bool App::moveToTrash(const std::string &path, std::string &errorOut) {
	GFile *file = g_file_new_for_path(path.c_str());
	GError *error = nullptr;
	const bool ok = g_file_trash(file, nullptr, &error);
	if (!ok && error) errorOut = error->message;
	g_clear_error(&error);
	g_object_unref(file);
	return ok;
}

void App::flash(const std::string &message) {
	gtk_label_set_text(GTK_LABEL(statusMessage), message.c_str());
	if (flashTimer) g_source_remove(flashTimer);
	flashTimer = g_timeout_add_seconds(6, +[](gpointer self) -> gboolean {
		App *app = static_cast<App *>(self);
		gtk_label_set_text(GTK_LABEL(app->statusMessage), "");
		app->flashTimer = 0;
		return G_SOURCE_REMOVE;
	}, this);
}

void App::setAlwaysOnTop(bool on) {
	gtk_window_set_keep_above(GTK_WINDOW(window), on);
	// Wayland doesn't let an app raise itself. Hyprland does it with a window rule or by pinning.
	if (on && g_getenv("WAYLAND_DISPLAY")) flash("Wayland apps can't keep themselves on top. In Hyprland, float and pin the window instead");
}

bool App::systemIsDark() {
	GtkSettings *settings = gtk_settings_get_default();
	gboolean prefersDark = FALSE;
	gchar *theme = nullptr;
	g_object_get(settings, "gtk-application-prefer-dark-theme", &prefersDark, "gtk-theme-name", &theme, nullptr);
	const bool dark = prefersDark || (theme && g_strrstr(theme, "dark")) || (theme && g_strrstr(theme, "Dark"));
	g_free(theme);
	return dark;
}

#pragma mark - Omarchy theme

void App::themeChanged() {
	if (!window) return;
	GtkSettings *settings = gtk_settings_get_default();
	const ChromeColors c = workspace->chromeColors();
	g_object_set(settings, "gtk-application-prefer-dark-theme", workspace->themeIsDark() ? TRUE : FALSE, nullptr);
	std::string css;
	if (c.valid) {
		const auto hash = [](const std::string &hex) { return "#" + hex; };
		const std::string bg = hash(c.background), fg = hash(c.foreground), dim = hash(c.dim), accent = hash(c.accent);
		const std::string selection = hash(c.selection), dark = hash(c.darkBackground), lighter = hash(c.lighterBackground);
		css =
			"window.omapad-main, .omapad-main .background { background-color: " + bg + "; color: " + fg + "; }\n"
			".omapad-main menubar { background-color: " + dark + "; color: " + fg + "; border: none; box-shadow: none; }\n"
			".omapad-main menubar > menuitem { color: " + fg + "; }\n"
			".omapad-main menubar > menuitem:hover { background-color: " + selection + "; color: " + fg + "; }\n"
			"menu { background-color: " + bg + "; color: " + fg + "; border: 1px solid " + selection + "; }\n"
			"menu menuitem { color: " + fg + "; }\n"
			"menu menuitem:hover { background-color: " + selection + "; color: " + fg + "; }\n"
			"menu menuitem:disabled { color: " + dim + "; }\n"
			".omapad-main notebook, .omapad-main notebook > header { background-color: " + dark + "; border: none; box-shadow: none; }\n"
			".omapad-main notebook > header > tabs > tab { background-color: " + dark + "; color: " + dim + "; border: none; border-top: 2px solid transparent; "
				"padding: 2px 8px; min-height: 22px; box-shadow: none; outline: none; }\n"
			".omapad-main notebook > header > tabs > tab:hover { background-color: " + lighter + "; color: " + fg + "; }\n"
			".omapad-main notebook > header > tabs > tab:checked { background-color: " + bg + "; color: " + fg + "; border-top: 2px solid " + accent + "; }\n"
			".omapad-main notebook.omapad-unfocused > header > tabs > tab:checked { border-top: 2px solid " + dim + "; }\n"
			".omapad-main notebook > stack { background-color: " + bg + "; }\n"
			".omapad-folders, .omapad-folders treeview.view { background-color: " + dark + "; color: " + fg + "; }\n"
			".omapad-folders treeview.view:selected { background-color: " + selection + "; color: " + fg + "; }\n"
			".omapad-folder-title { color: " + dim + "; font-size: 80%; font-weight: bold; }\n"
			".omapad-status { background-color: " + dark + "; color: " + dim + "; }\n"
			".omapad-status label { color: " + dim + "; }\n"
			".omapad-status-message { color: " + accent + "; }\n"
			".omapad-main paned > separator { background-color: " + selection + "; background-image: none; min-width: 1px; min-height: 1px; }\n"
			".omapad-tab-close { min-height: 16px; min-width: 16px; padding: 0; border: none; background: none; box-shadow: none; color: " + dim + "; }\n"
			".omapad-tab-close:hover { color: " + fg + "; }\n";
	} else {
		css =
			".omapad-tab-close { min-height: 16px; min-width: 16px; padding: 0; }\n"
			".omapad-main notebook > header > tabs > tab { padding: 2px 8px; min-height: 22px; }\n"
			".omapad-folder-title { font-size: 80%; font-weight: bold; }\n"
			".omapad-status { padding: 0; }\n";
	}
	css +=
		".omapad-notes-library { padding: 2px; }\n"
		".omapad-notes-library treeview.view { border: none; }\n"
		".omapad-notes-bar { border-bottom: 1px solid alpha(currentColor, 0.15); }\n"
		".omapad-notes-bar button { padding: 4px 9px; }\n"
		".omapad-notes-preview { font-size: 110%; }\n";
	gtk_css_provider_load_from_data(themeCss, css.c_str(), -1, nullptr);
}

void App::watchFile(const std::string &path) {
	GFile *file = g_file_new_for_path(path.c_str());
	GFileMonitor *monitor = g_file_monitor_file(file, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
	g_object_unref(file);
	if (!monitor) return;
	g_signal_connect(monitor, "changed", G_CALLBACK(+[](GFileMonitor *, GFile *, GFile *, GFileMonitorEvent, gpointer self) {
		App *app = static_cast<App *>(self);
		// A theme switch rewrites several files. Wait for it to settle.
		if (app->themeTimer) g_source_remove(app->themeTimer);
		app->themeTimer = g_timeout_add(250, +[](gpointer data) -> gboolean {
			App *a = static_cast<App *>(data);
			a->themeTimer = 0;
			a->workspace->reloadOmarchyTheme();
			return G_SOURCE_REMOVE;
		}, app);
	}), this);
	themeMonitors.push_back(monitor);
}

void App::watchTheme() {
	const std::string path = workspace->omarchyThemePath();
	if (path.empty()) return;
	watchFile(path);
	// Omarchy records each switch here too, which still fires if the theme folder is replaced.
	const fs::path current = fs::path(path).parent_path().parent_path();
	watchFile((current / "theme.name").string());
}

void App::scheduleActivationCheck() {
	if (activationTimer || dialogDepth > 0) return;
	activationTimer = g_timeout_add(150, +[](gpointer self) -> gboolean {
		App *app = static_cast<App *>(self);
		app->activationTimer = 0;
		if (app->dialogDepth == 0 && gtk_window_is_active(GTK_WINDOW(app->window))) app->workspace->applicationActivated();
		return G_SOURCE_REMOVE;
	}, this);
}

#pragma mark - Notes mode

void App::buildNotes() {
	// These are native widgets around the existing editor, not a second document model.
	notesBar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_style_context_add_class(gtk_widget_get_style_context(notesBar), "omapad-notes-bar");
	gtk_container_set_border_width(GTK_CONTAINER(notesBar), 8);
	const auto button = [this](GtkWidget *box, const char *label, const char *hint, Cmd cmd, int arg = 0, const std::string &text = std::string()) {
		GtkWidget *b = gtk_button_new_with_label(label);
		gtk_widget_set_tooltip_text(b, hint);
		gtk_widget_set_can_focus(b, FALSE);
		onSignal(b, "clicked", [this, cmd, arg, text]() { post([this, cmd, arg, text]() { workspace->run(cmd, arg, text); }); });
		gtk_box_pack_start(GTK_BOX(box), b, FALSE, FALSE, 0);
		return b;
	};
	button(notesBar, "Editor", "Return to the regular editor", Cmd::NotesMode);
	notesFocusButton = button(notesBar, "Focus", "Hide or show the notes library", Cmd::NotesFocus);
	GtkWidget *formatButton = gtk_menu_button_new();
	gtk_button_set_label(GTK_BUTTON(formatButton), "Format");
	gtk_widget_set_tooltip_text(formatButton, "Markdown formatting helpers");
	std::vector<MenuItem> formatItems;
	for (const auto &entry : std::vector<std::pair<std::string, std::string>>{{"Heading", "heading"}, {"Bold", "bold"}, {"Italic", "italic"}, {"Bullet List", "bullet"}, {"Checklist", "task"}, {"Quote", "quote"}, {"Code", "code"}, {"Link", "link"}}) {
		MenuItem item;
		item.title = entry.first;
		item.cmd = Cmd::NotesFormat;
		item.text = entry.second;
		formatItems.push_back(item);
	}
	GtkWidget *formatMenu = nativeMenu(formatItems, nullptr);
	gtk_widget_show_all(formatMenu);
	gtk_menu_button_set_popup(GTK_MENU_BUTTON(formatButton), formatMenu);
	gtk_box_pack_start(GTK_BOX(notesBar), formatButton, FALSE, FALSE, 0);
	notesFormatButton = formatButton;
	notesTitle = gtk_label_new("Notes");
	gtk_label_set_ellipsize(GTK_LABEL(notesTitle), PANGO_ELLIPSIZE_END);
	gtk_label_set_xalign(GTK_LABEL(notesTitle), 0);
	gtk_widget_set_margin_start(notesTitle, 8);
	gtk_box_pack_start(GTK_BOX(notesBar), notesTitle, TRUE, TRUE, 0);
	notesBarStatus = gtk_label_new("");
	gtk_label_set_ellipsize(GTK_LABEL(notesBarStatus), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(notesBarStatus), 24);
	gtk_style_context_add_class(gtk_widget_get_style_context(notesBarStatus), "dim-label");
	gtk_box_pack_start(GTK_BOX(notesBar), notesBarStatus, FALSE, FALSE, 4);
	notesBarRetry = button(notesBar, "Retry", "Retry saving unsaved notes", Cmd::NotesRetry);
	GtkWidget *viewButtons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(viewButtons), "linked");
	for (int i = 0; i < 3; i++) {
		const char *label = i == 0 ? "Write" : i == 1 ? "Split" : "Read";
		notesViewButtons[i] = gtk_toggle_button_new_with_label(label);
		gtk_widget_set_can_focus(notesViewButtons[i], FALSE);
		onSignal(notesViewButtons[i], "clicked", [this, i]() {
			if (!syncingNotes) post([this, i]() { workspace->run(Cmd::NotesView, i); });
		});
		gtk_box_pack_start(GTK_BOX(viewButtons), notesViewButtons[i], FALSE, FALSE, 0);
	}
	gtk_box_pack_end(GTK_BOX(notesBar), viewButtons, FALSE, FALSE, 0);

	notesSidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	gtk_style_context_add_class(gtk_widget_get_style_context(notesSidebar), "omapad-notes-library");
	gtk_widget_set_size_request(notesSidebar, 280, -1);
	gtk_container_set_border_width(GTK_CONTAINER(notesSidebar), 12);
	GtkWidget *libraryHeader = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *libraryTitle = gtk_label_new("NOTES");
	gtk_style_context_add_class(gtk_widget_get_style_context(libraryTitle), "omapad-folder-title");
	gtk_label_set_xalign(GTK_LABEL(libraryTitle), 0);
	gtk_box_pack_start(GTK_BOX(libraryHeader), libraryTitle, TRUE, TRUE, 0);
	button(libraryHeader, "+ New", "Create a note and start writing", Cmd::NotesNew);
	GtkWidget *more = gtk_menu_button_new();
	gtk_button_set_label(GTK_BUTTON(more), "⋯");
	gtk_widget_set_tooltip_text(more, "Library actions");
	std::vector<MenuItem> libraryItems;
	for (const auto &entry : std::vector<std::pair<std::string, Cmd>>{{"Choose Notes Folder…", Cmd::NotesFolder}, {"Reveal Notes Folder", Cmd::NotesReveal}, {"Import Markdown…", Cmd::NotesImport}, {"Refresh Library", Cmd::NotesRefresh}, {"Export Note as HTML…", Cmd::NotesExportHtml}, {"Move Note to Trash…", Cmd::NotesTrash}}) {
		MenuItem item;
		item.title = entry.first;
		item.cmd = entry.second;
		libraryItems.push_back(item);
	}
	GtkWidget *libraryMenu = nativeMenu(libraryItems, nullptr);
	gtk_widget_show_all(libraryMenu);
	gtk_menu_button_set_popup(GTK_MENU_BUTTON(more), libraryMenu);
	gtk_box_pack_end(GTK_BOX(libraryHeader), more, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(notesSidebar), libraryHeader, FALSE, FALSE, 0);
	notesFolderLabel = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(notesFolderLabel), 0);
	gtk_label_set_ellipsize(GTK_LABEL(notesFolderLabel), PANGO_ELLIPSIZE_MIDDLE);
	gtk_style_context_add_class(gtk_widget_get_style_context(notesFolderLabel), "dim-label");
	gtk_box_pack_start(GTK_BOX(notesSidebar), notesFolderLabel, FALSE, FALSE, 0);
	notesSearch = gtk_search_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(notesSearch), "Search all notes");
	gtk_widget_set_tooltip_text(notesSearch, "Search titles and text. Every word must match.");
	onSignal(notesSearch, "search-changed", [this]() {
		if (!syncingNotes) workspace->setNotesQuery(gtk_entry_get_text(GTK_ENTRY(notesSearch)));
	});
	gtk_box_pack_start(GTK_BOX(notesSidebar), notesSearch, FALSE, FALSE, 0);

	notesTagStore = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
	notesTagView = gtk_tree_view_new_with_model(GTK_TREE_MODEL(notesTagStore));
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(notesTagView), FALSE);
	gtk_tree_view_append_column(GTK_TREE_VIEW(notesTagView), gtk_tree_view_column_new_with_attributes("Tags", gtk_cell_renderer_text_new(), "text", 0, nullptr));
	g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(notesTagView)), "changed", G_CALLBACK(+[](GtkTreeSelection *selection, gpointer self) {
		App *a = static_cast<App *>(self);
		if (a->syncingNotes) return;
		GtkTreeIter iter;
		GtkTreeModel *model = nullptr;
		if (!gtk_tree_selection_get_selected(selection, &model, &iter)) return;
		gchar *raw = nullptr;
		gtk_tree_model_get(model, &iter, 1, &raw, -1);
		const std::string tag = raw ? raw : "";
		g_free(raw);
		a->post([a, tag]() { a->workspace->setNotesTag(tag); });
	}), this);
	GtkWidget *tagScroll = gtk_scrolled_window_new(nullptr, nullptr);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tagScroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request(tagScroll, -1, 110);
	gtk_container_add(GTK_CONTAINER(tagScroll), notesTagView);
	gtk_box_pack_start(GTK_BOX(notesSidebar), tagScroll, FALSE, FALSE, 0);

	notesStore = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
	notesList = gtk_tree_view_new_with_model(GTK_TREE_MODEL(notesStore));
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(notesList), FALSE);
	gtk_tree_view_set_enable_search(GTK_TREE_VIEW(notesList), FALSE);
	GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
	g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, "ypad", 10, "xpad", 8, nullptr);
	GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes("Notes", renderer, "markup", 0, nullptr);
	gtk_tree_view_column_set_expand(column, TRUE);
	gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_FIXED);
	gtk_tree_view_append_column(GTK_TREE_VIEW(notesList), column);
	g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(notesList)), "changed", G_CALLBACK(+[](GtkTreeSelection *selection, gpointer self) {
		App *a = static_cast<App *>(self);
		if (a->syncingNotes) return;
		GtkTreeIter iter;
		GtkTreeModel *model = nullptr;
		if (!gtk_tree_selection_get_selected(selection, &model, &iter)) return;
		gchar *raw = nullptr;
		gtk_tree_model_get(model, &iter, 1, &raw, -1);
		const std::string path = raw ? raw : "";
		g_free(raw);
		if (!path.empty() && path != a->notesUiState.selectedPath)
			a->post([a, path]() { a->workspace->openNote(path); });
	}), this);
	notesListScroll = gtk_scrolled_window_new(nullptr, nullptr);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(notesListScroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(notesListScroll), notesList);
	gtk_box_pack_start(GTK_BOX(notesSidebar), notesListScroll, TRUE, TRUE, 0);
	notesEmpty = gtk_label_new("Your notes live here.\nChoose + New and start writing.");
	gtk_label_set_line_wrap(GTK_LABEL(notesEmpty), TRUE);
	gtk_label_set_justify(GTK_LABEL(notesEmpty), GTK_JUSTIFY_CENTER);
	gtk_style_context_add_class(gtk_widget_get_style_context(notesEmpty), "dim-label");
	gtk_box_pack_start(GTK_BOX(notesSidebar), notesEmpty, FALSE, FALSE, 8);

	notesSaveBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	notesSaveStatus = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(notesSaveStatus), 0);
	gtk_label_set_line_wrap(GTK_LABEL(notesSaveStatus), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(notesSaveStatus), 32);
	gtk_box_pack_start(GTK_BOX(notesSaveBox), notesSaveStatus, TRUE, TRUE, 0);
	notesRetry = button(notesSaveBox, "Retry", "Retry saving unsaved notes", Cmd::NotesRetry);
	gtk_box_pack_start(GTK_BOX(notesSidebar), notesSaveBox, FALSE, FALSE, 0);

	notesPreview = gtk_text_view_new();
	gtk_style_context_add_class(gtk_widget_get_style_context(notesPreview), "omapad-notes-preview");
	gtk_text_view_set_editable(GTK_TEXT_VIEW(notesPreview), FALSE);
	gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(notesPreview), FALSE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(notesPreview), GTK_WRAP_WORD_CHAR);
	gtk_text_view_set_left_margin(GTK_TEXT_VIEW(notesPreview), 28);
	gtk_text_view_set_right_margin(GTK_TEXT_VIEW(notesPreview), 28);
	gtk_text_view_set_top_margin(GTK_TEXT_VIEW(notesPreview), 24);
	gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(notesPreview), 24);
	gtk_text_view_set_pixels_below_lines(GTK_TEXT_VIEW(notesPreview), 8);
	gtk_widget_set_tooltip_text(notesPreview, "Rendered Markdown. Select text to copy; links and HTML are inert.");
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(notesPreview));
	for (int level = 1; level <= 6; level++) {
		const std::string name = "heading" + std::to_string(level);
		gtk_text_buffer_create_tag(buffer, name.c_str(), "weight", PANGO_WEIGHT_BOLD, "scale", 1.9 - level * 0.13, "pixels-above-lines", 12, "pixels-below-lines", 8, nullptr);
	}
	gtk_text_buffer_create_tag(buffer, "quote", "left-margin", 18, "style", PANGO_STYLE_ITALIC, nullptr);
	gtk_text_buffer_create_tag(buffer, "codeblock", "family", "monospace", "left-margin", 14, "right-margin", 14, "pixels-above-lines", 6, "pixels-below-lines", 6, nullptr);
	for (unsigned style = 0; style < 16; style++) {
		const std::string name = "inline" + std::to_string(style);
		GtkTextTag *tag = gtk_text_buffer_create_tag(buffer, name.c_str(), nullptr);
		if (style & 1) g_object_set(tag, "weight", PANGO_WEIGHT_BOLD, nullptr);
		if (style & 2) g_object_set(tag, "style", PANGO_STYLE_ITALIC, nullptr);
		if (style & 4) g_object_set(tag, "family", "monospace", nullptr);
		if (style & 8) g_object_set(tag, "underline", PANGO_UNDERLINE_SINGLE, nullptr);
	}
	notesPreviewScroll = gtk_scrolled_window_new(nullptr, nullptr);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(notesPreviewScroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(notesPreviewScroll), notesPreview);
	notesContentPaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_paned_pack1(GTK_PANED(notesContentPaned), outerPaned, TRUE, FALSE);
	gtk_paned_pack2(GTK_PANED(notesContentPaned), notesPreviewScroll, TRUE, FALSE);
	notesRootPaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_paned_pack1(GTK_PANED(notesRootPaned), notesSidebar, FALSE, FALSE);
	gtk_paned_pack2(GTK_PANED(notesRootPaned), notesContentPaned, TRUE, FALSE);
	gtk_paned_set_position(GTK_PANED(notesRootPaned), 300);
	gtk_widget_show_all(notesRootPaned);
	gtk_widget_show_all(notesBar);
	gtk_widget_hide(notesSidebar);
	gtk_widget_hide(notesBar);
	gtk_widget_hide(notesPreviewScroll);
	gtk_widget_hide(notesRetry);
	gtk_widget_hide(notesBarRetry);
}

void App::applyNotesLayout() {
	if (!notesBar) return;
	const bool enabled = notesUiState.enabled;
	gtk_widget_set_visible(notesBar, enabled);
	gtk_widget_set_visible(notesSidebar, enabled && !notesUiState.focus);
	gtk_widget_set_visible(notesPreviewScroll, enabled && notesUiState.viewMode != 0);
	gtk_widget_set_visible(outerPaned, !enabled || notesUiState.viewMode != 2);
	gtk_widget_set_visible(toolbar, !enabled && chromeState.toolbar);
	for (int pane = 0; pane < 2; pane++) {
		gtk_widget_set_visible(panes[pane].notebook, !enabled);
		gtk_notebook_set_show_tabs(GTK_NOTEBOOK(panes[pane].notebook), !enabled && chromeState.tabs && !tabStyle.multiLine);
		if (flowTabs[pane]) gtk_widget_set_visible(flowTabs[pane], !enabled && chromeState.tabs && tabStyle.multiLine);
		if (enabled) gtk_widget_set_visible(panes[pane].box, pane == workspace->focusedPane());
	}
	if (enabled) {
		gtk_widget_hide(folderBox);
		gtk_widget_hide(sideBox);
		gtk_widget_hide(results.widget);
		gtk_widget_hide(incrementalBar);
		gtk_widget_show(menubar); // Notes remains reachable even if the coding menu is hidden.
	}
}

void App::renderNotesPreview(const NotesState &state) {
	std::string signature = state.selectedPath + (state.activeNote ? "1\n" : "0\n");
	for (const NoteBlock &block : state.preview) {
		signature += std::to_string(static_cast<int>(block.kind)) + ":" + std::to_string(block.level) + (block.checked ? "1" : "0");
		for (const NoteSpan &span : block.spans) signature += std::to_string(span.style) + ":" + span.text + '\0';
	}
	if (signature == notesPreviewSignature) return;
	const bool sameNote = notesPreviewPath == state.selectedPath;
	notesPreviewPath = state.selectedPath;
	notesPreviewSignature = std::move(signature);
	GtkAdjustment *scroll = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(notesPreviewScroll));
	const double previous = sameNote ? gtk_adjustment_get_value(scroll) : 0;
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(notesPreview));
	GtkTextIter selectedStart, selectedEnd;
	const bool hadSelection = sameNote && gtk_text_buffer_get_selection_bounds(buffer, &selectedStart, &selectedEnd);
	const int selectionStart = hadSelection ? gtk_text_iter_get_offset(&selectedStart) : 0;
	const int selectionEnd = hadSelection ? gtk_text_iter_get_offset(&selectedEnd) : 0;
	gtk_text_buffer_set_text(buffer, "", 0);
	if (!state.activeNote) gtk_text_buffer_set_text(buffer, "Choose a note from your library, or create a new one.", -1);
	for (const NoteBlock &block : state.preview) {
		GtkTextIter end;
		gtk_text_buffer_get_end_iter(buffer, &end);
		const int start = gtk_text_iter_get_offset(&end);
		const char *prefix = block.kind == NoteBlock::Kind::List ? "•  " : block.kind == NoteBlock::Kind::Task ? (block.checked ? "☑  " : "☐  ") : block.kind == NoteBlock::Kind::Quote ? "│  " : block.kind == NoteBlock::Kind::Rule ? "──────────────" : "";
		gtk_text_buffer_insert(buffer, &end, prefix, -1);
		for (const NoteSpan &span : block.spans) {
			const std::string name = "inline" + std::to_string(span.style & 15);
			gtk_text_buffer_insert_with_tags_by_name(buffer, &end, span.text.c_str(), static_cast<gint>(span.text.size()), name.c_str(), nullptr);
		}
		gtk_text_buffer_insert(buffer, &end, "\n", 1);
		GtkTextIter from;
		gtk_text_buffer_get_iter_at_offset(buffer, &from, start);
		std::string tag;
		if (block.kind == NoteBlock::Kind::Heading) tag = "heading" + std::to_string(std::clamp(block.level, 1, 6));
		else if (block.kind == NoteBlock::Kind::Quote) tag = "quote";
		else if (block.kind == NoteBlock::Kind::Code) tag = "codeblock";
		if (!tag.empty()) gtk_text_buffer_apply_tag_by_name(buffer, tag.c_str(), &from, &end);
	}
	if (hadSelection) {
		const int length = gtk_text_buffer_get_char_count(buffer);
		gtk_text_buffer_get_iter_at_offset(buffer, &selectedStart, std::min(selectionStart, length));
		gtk_text_buffer_get_iter_at_offset(buffer, &selectedEnd, std::min(selectionEnd, length));
		gtk_text_buffer_select_range(buffer, &selectedStart, &selectedEnd);
	}
	gtk_adjustment_set_value(scroll, previous);
}

void App::notesChanged(const NotesState &state) {
	if (!notesBar) return; // Startup restores the state again after all widgets exist.
	const bool entering = state.enabled && !notesUiState.enabled;
	const bool leaving = !state.enabled && notesUiState.enabled;
	const bool previewChanged = state.viewMode != notesUiState.viewMode;
	const bool selectionChanged = state.selectedPath != notesUiState.selectedPath;
	if (entering) {
		notesSavedPositions = {gtk_paned_get_position(GTK_PANED(outerPaned)), gtk_paned_get_position(GTK_PANED(sidePaned)), gtk_paned_get_position(GTK_PANED(rightPaned)), gtk_paned_get_position(GTK_PANED(editorPaned))};
		notesSavedResults = gtk_widget_get_visible(results.widget);
		notesSavedIncremental = gtk_widget_get_visible(incrementalBar);
	}
	notesUiState = state;
	syncingNotes = true;
	if (state.query != gtk_entry_get_text(GTK_ENTRY(notesSearch))) gtk_entry_set_text(GTK_ENTRY(notesSearch), state.query.c_str());
	gtk_label_set_text(GTK_LABEL(notesFolderLabel), fs::path(state.folder).filename().string().c_str());
	gtk_widget_set_tooltip_text(notesFolderLabel, state.folder.c_str());
	gtk_label_set_text(GTK_LABEL(notesTitle), (state.activeNote ? state.title : "Notes").c_str());
	gtk_widget_set_sensitive(notesFormatButton, state.activeNote);
	gtk_button_set_label(GTK_BUTTON(notesFocusButton), state.focus ? "Library" : "Focus");
	for (int i = 0; i < 3; i++) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(notesViewButtons[i]), i == state.viewMode);
	std::string tagSignature = std::to_string(state.total) + "|" + state.tag;
	for (const auto &tag : state.tags) tagSignature += "|" + tag.first + ":" + std::to_string(tag.second);
	if (tagSignature != notesTagSignature) {
		notesTagSignature = std::move(tagSignature);
		gtk_list_store_clear(notesTagStore);
		const auto addTag = [&](const std::string &label, const std::string &value) {
			GtkTreeIter row;
			gtk_list_store_append(notesTagStore, &row);
			gtk_list_store_set(notesTagStore, &row, 0, label.c_str(), 1, value.c_str(), -1);
			if (state.tag == value) gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(notesTagView)), &row);
		};
		addTag("All Notes  ·  " + std::to_string(state.total), "");
		for (const auto &tag : state.tags) addTag("#" + tag.first + "  ·  " + std::to_string(tag.second), tag.first);
	}
	std::string listSignature;
	for (const NoteSummary &note : state.notes) listSignature += note.path + '\0' + note.title + '\0' + note.preview + '\0';
	if (listSignature != notesListSignature) {
		notesListSignature = std::move(listSignature);
		GtkAdjustment *scroll = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(notesListScroll));
		const double previous = gtk_adjustment_get_value(scroll);
		gtk_list_store_clear(notesStore);
		for (const NoteSummary &note : state.notes) {
			gchar *title = g_markup_escape_text(note.title.c_str(), -1);
			gchar *preview = g_markup_escape_text(note.preview.c_str(), -1);
			const std::string markup = std::string("<b>") + title + "</b>\n<small>" + preview + "</small>";
			g_free(title);
			g_free(preview);
			GtkTreeIter row;
			gtk_list_store_append(notesStore, &row);
			gtk_list_store_set(notesStore, &row, 0, markup.c_str(), 1, note.path.c_str(), -1);
		}
		gtk_adjustment_set_value(scroll, previous);
	}
	GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(notesList));
	gtk_tree_selection_unselect_all(selection);
	GtkTreeIter row;
	if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(notesStore), &row)) {
		do {
			gchar *path = nullptr;
			gtk_tree_model_get(GTK_TREE_MODEL(notesStore), &row, 1, &path, -1);
			if (path && state.selectedPath == path) gtk_tree_selection_select_iter(selection, &row);
			g_free(path);
		} while (gtk_tree_model_iter_next(GTK_TREE_MODEL(notesStore), &row));
	}
	gtk_widget_set_visible(notesEmpty, state.notes.empty());
	gtk_label_set_text(GTK_LABEL(notesEmpty), state.total == 0 ? "Your notes live here.\nChoose + New and start writing." : "No notes match.\nTry another search or tag.");
	gtk_label_set_text(GTK_LABEL(notesSaveStatus), (state.status + (state.activeNote ? " · " + std::to_string(state.words) + " words" : "")).c_str());
	gtk_widget_set_tooltip_text(notesSaveStatus, state.status.c_str());
	GtkStyleContext *statusStyle = gtk_widget_get_style_context(notesSaveStatus);
	if (state.saveError) gtk_style_context_add_class(statusStyle, "error");
	else gtk_style_context_remove_class(statusStyle, "error");
	gtk_widget_set_visible(notesRetry, state.saveError);
	gtk_label_set_text(GTK_LABEL(notesBarStatus), (state.saveError ? "Save failed" : state.status).c_str());
	gtk_widget_set_tooltip_text(notesBarStatus, (state.status + (state.activeNote ? " · " + std::to_string(state.words) + " words" : "")).c_str());
	GtkStyleContext *barStatusStyle = gtk_widget_get_style_context(notesBarStatus);
	if (state.saveError) gtk_style_context_add_class(barStatusStyle, "error");
	else gtk_style_context_remove_class(barStatusStyle, "error");
	gtk_widget_set_visible(notesBarRetry, state.saveError);
	renderNotesPreview(state);
	syncingNotes = false;
	if (leaving) {
		setChrome(chromeState);
		gtk_widget_show(panes[0].box);
		gtk_widget_set_visible(panes[1].box, workspace->splitVisible());
		gtk_widget_set_visible(results.widget, notesSavedResults);
		gtk_widget_set_visible(incrementalBar, notesSavedIncremental);
		if (notesSavedPositions.size() == 4) {
			gtk_paned_set_position(GTK_PANED(outerPaned), notesSavedPositions[0]);
			gtk_paned_set_position(GTK_PANED(sidePaned), notesSavedPositions[1]);
			gtk_paned_set_position(GTK_PANED(rightPaned), notesSavedPositions[2]);
			gtk_paned_set_position(GTK_PANED(editorPaned), notesSavedPositions[3]);
		}
	}
	applyNotesLayout();
	// A pane returning from Read view must be mapped before GTK can focus its editor.
	if (state.enabled && (entering || previewChanged || selectionChanged))
		post([this]() { if (notesUiState.enabled) focusPane(workspace->focusedPane()); });
	if ((entering || previewChanged) && state.enabled && state.viewMode == 1) {
		GtkAllocation allocation;
		gtk_widget_get_allocation(notesContentPaned, &allocation);
		if (allocation.width > 1) gtk_paned_set_position(GTK_PANED(notesContentPaned), allocation.width / 2);
	}
}

#pragma mark - Window

bool App::tryQuit() {
	if (!workspace->prepareToQuit()) return false;
	// The clipboard would otherwise disappear with this app.
	gtk_clipboard_store(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
	g_application_quit(G_APPLICATION(application));
	return true;
}

void App::build() {
	std::string settingsDir = homeSubdir("XDG_CONFIG_HOME", ".config");
	std::string stateDir = homeSubdir("XDG_STATE_HOME", ".local/state");
	const char *supportOverride = g_getenv("OMAPAD_SUPPORT_DIR");
	if (supportOverride) settingsDir = stateDir = supportOverride;
	resolveConfigDirectories(settingsDir, stateDir);
	workspace = std::make_unique<Workspace>(*this, findResources(), settingsDir, stateDir);

	window = gtk_application_window_new(application);
	gtk_style_context_add_class(gtk_widget_get_style_context(window), "omapad-main");
	gtk_window_set_default_size(GTK_WINDOW(window), 1100, 720);
	gtk_window_set_title(GTK_WINDOW(window), "omapad");
	gtk_window_set_icon_name(GTK_WINDOW(window), "omapad");
	gtk_application_window_set_show_menubar(GTK_APPLICATION_WINDOW(window), FALSE);

	themeCss = gtk_css_provider_new();
	gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(themeCss), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

	buildPane(0);
	buildPane(1);
	results.widget = scintilla_new();
	g_signal_connect(results.widget, SCINTILLA_NOTIFY, G_CALLBACK(+[](GtkWidget *, gint, SCNotification *n, gpointer self) {
		App *app = static_cast<App *>(self);
		if (app->workspace) app->workspace->resultsNotification(*n);
	}), this);
	g_signal_connect(results.widget, "button-press-event", G_CALLBACK(+[](GtkWidget *, GdkEventButton *event, gpointer self) -> gboolean {
		if (event->button != GDK_BUTTON_SECONDARY) return FALSE;
		App *app = static_cast<App *>(self);
		app->popup(app->workspace->resultsMenu());
		return TRUE;
	}), this);
	background.widget = scintilla_new();
	g_object_ref_sink(background.widget);
	printer.widget = scintilla_new();
	g_object_ref_sink(printer.widget);
	buildDocumentMap();

	editorPaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_paned_pack1(GTK_PANED(editorPaned), panes[0].box, TRUE, FALSE);
	gtk_paned_pack2(GTK_PANED(editorPaned), panes[1].box, TRUE, FALSE);
	rightPaned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
	gtk_paned_pack1(GTK_PANED(rightPaned), editorPaned, TRUE, FALSE);
	gtk_paned_pack2(GTK_PANED(rightPaned), results.widget, FALSE, FALSE);
	buildFolderPanel();
	buildFunctionPanel();
	sideBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	panelsBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_size_request(panelsBox, 240, -1);
	gtk_widget_set_no_show_all(panelsBox, TRUE);
	gtk_box_pack_start(GTK_BOX(sideBox), panelsBox, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(sideBox), functionBox, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(sideBox), map.widget, FALSE, FALSE, 0);
	sidePaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_paned_pack1(GTK_PANED(sidePaned), rightPaned, TRUE, FALSE);
	gtk_paned_pack2(GTK_PANED(sidePaned), sideBox, FALSE, FALSE);
	outerPaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_paned_pack1(GTK_PANED(outerPaned), folderBox, FALSE, FALSE);
	gtk_paned_pack2(GTK_PANED(outerPaned), sidePaned, TRUE, FALSE);

	GtkWidget *status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
	statusBar = status;
	gtk_style_context_add_class(gtk_widget_get_style_context(status), "omapad-status");
	statusLeft = gtk_label_new("");
	statusMessage = gtk_label_new("");
	statusRight = gtk_label_new("");
	gtk_style_context_add_class(gtk_widget_get_style_context(statusMessage), "omapad-status-message");
	for (GtkWidget *label : {statusLeft, statusMessage, statusRight}) {
		gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
		gtk_widget_set_margin_top(label, 3);
		gtk_widget_set_margin_bottom(label, 3);
	}
	gtk_widget_set_margin_start(statusLeft, 10);
	gtk_widget_set_margin_end(statusRight, 10);
	gtk_label_set_xalign(GTK_LABEL(statusMessage), 0);
	// Clicking either end of the status bar offers what it shows.
	const auto clickable = [this](GtkWidget *label, bool rightSide) {
		GtkWidget *events = gtk_event_box_new();
		gtk_event_box_set_visible_window(GTK_EVENT_BOX(events), FALSE);
		gtk_container_add(GTK_CONTAINER(events), label);
		struct Binding { App *app; bool right; };
		g_signal_connect_data(events, "button-press-event", G_CALLBACK(+[](GtkWidget *, GdkEventButton *, gpointer data) -> gboolean {
			auto *b = static_cast<Binding *>(data);
			b->app->popup(b->app->workspace->statusMenu(b->right));
			return TRUE;
		}), new Binding{this, rightSide}, +[](gpointer data, GClosure *) { delete static_cast<Binding *>(data); }, GConnectFlags(0));
		return events;
	};
	gtk_box_pack_start(GTK_BOX(status), clickable(statusLeft, false), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(status), statusMessage, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(status), clickable(statusRight, true), FALSE, FALSE, 0);

	GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_container_add(GTK_CONTAINER(window), root);

	workspace->start();

	menubar = gtk_menu_bar_new();
	buildMenuBar();
	gtk_box_pack_start(GTK_BOX(root), menubar, FALSE, FALSE, 0);
	buildToolbar();
	gtk_box_pack_start(GTK_BOX(root), toolbar, FALSE, FALSE, 0);
	buildNotes();
	gtk_box_pack_start(GTK_BOX(root), notesBar, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(root), notesRootPaned, TRUE, TRUE, 0);
	buildIncrementalBar();
	gtk_box_pack_start(GTK_BOX(root), incrementalBar, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(root), status, FALSE, FALSE, 0);
	g_signal_connect(window, "key-press-event", G_CALLBACK(+[](GtkWidget *, GdkEventKey *event, gpointer self) -> gboolean {
		App *app = static_cast<App *>(self);
		// A hidden menu bar comes back for a moment with F10.
		if (event->keyval == GDK_KEY_F10 && !app->chromeState.menu) {
			app->menuTemporarilyShown = !app->menuTemporarilyShown;
			gtk_widget_set_visible(app->menubar, app->menuTemporarilyShown);
			return TRUE;
		}
		return FALSE;
	}), this);

	buildFindWindow();

	g_signal_connect(window, "delete-event", G_CALLBACK(+[](GtkWidget *, GdkEvent *, gpointer self) -> gboolean {
		static_cast<App *>(self)->tryQuit();
		return TRUE;
	}), this);
	g_signal_connect(window, "notify::is-active", G_CALLBACK(+[](GObject *object, GParamSpec *, gpointer self) {
		if (gtk_window_is_active(GTK_WINDOW(object))) static_cast<App *>(self)->scheduleActivationCheck();
	}), this);
	g_timeout_add_seconds(1, +[](gpointer self) -> gboolean {
		static_cast<App *>(self)->workspace->tick();
		return G_SOURCE_CONTINUE;
	}, this);

	gtk_widget_show_all(root);
	gtk_widget_hide(notesBar);
	gtk_widget_hide(notesSidebar);
	gtk_widget_hide(notesPreviewScroll);
	gtk_widget_hide(folderBox);
	gtk_widget_hide(panes[1].box);
	gtk_widget_hide(results.widget);
	gtk_widget_hide(sideBox);
	gtk_widget_hide(functionBox);
	gtk_widget_hide(map.widget);
	themeChanged();
	watchTheme();
	tabsChanged();
	gtk_widget_hide(incrementalBar);
	workspace->prepareCommandLine(gCommandLine);
	workspace->restoreSession();
	workspace->afterStartup();
	notesChanged(workspace->notesState());
	splitChanged(workspace->splitVisible(), workspace->splitStacked());
	setFolderPanelVisible(workspace->folderPanelVisible());
	foldersChanged();
	tabsChanged();
	// Sizes aren't known until the window is on screen, so even out a restored split then.
	g_signal_connect(window, "map-event", G_CALLBACK(+[](GtkWidget *, GdkEvent *, gpointer self) -> gboolean {
		App *app = static_cast<App *>(self);
		g_timeout_add(50, +[](gpointer data) -> gboolean {
			App *a = static_cast<App *>(data);
			if (!a->splitPositioned && a->workspace->splitVisible()) a->splitChanged(true, a->workspace->splitStacked());
			a->arrangeSidePanels();
			return G_SOURCE_REMOVE;
		}, app);
		return FALSE;
	}), this);
}

void App::openPaths(const std::vector<std::string> &paths) {
	for (const std::string &path : paths) {
		std::error_code error;
		if (fs::is_directory(path, error)) workspace->addFolder(path);
		else workspace->openPath(path);
	}
}

void App::present() {
	gtk_window_present(GTK_WINDOW(window));
	focusPane(workspace->focusedPane());
}

static bool wantsNewInstance() {
	if (gCommandLine.multiInstance || g_getenv("OMAPAD_NON_UNIQUE")) return true;
	if (gCommandLine.noPlugin || !gCommandLine.settingsDirectory.empty()) return true;
	const char *config = g_getenv("XDG_CONFIG_HOME");
	const std::string path = (config && *config ? std::string(config) : std::string(g_get_home_dir()) + "/.config") + "/omapad/settings.json";
	gchar *contents = nullptr;
	if (!g_file_get_contents(path.c_str(), &contents, nullptr, nullptr)) return false;
	const bool multi = std::string(contents).find("\"instanceMode\": \"multi\"") != std::string::npos;
	g_free(contents);
	return multi;
}

int main(int argc, char **argv) {
	std::vector<std::string> incoming;
	for (int i = 1; i < argc; i++) incoming.push_back(argv[i]);
	gCommandLine = parseCommandLine(incoming);
	if (gCommandLine.help) {
		fputs(commandLineHelp(), stdout);
		return 0;
	}
	setLaunchOptions(gCommandLine);
	// GApplication only sees what's left: the files, and its own options.
	std::vector<char *> remaining = {argv[0]};
	for (int i = 1; i < argc; i++) {
		const std::string argument = argv[i];
		const bool known = std::find(gCommandLine.ignored.begin(), gCommandLine.ignored.end(), argument) != gCommandLine.ignored.end();
		if (argument.empty() || argument[0] != '-' || known) remaining.push_back(argv[i]);
	}
	int remainingCount = static_cast<int>(remaining.size());
	GApplicationFlags flags = G_APPLICATION_HANDLES_OPEN;
	if (wantsNewInstance()) flags = GApplicationFlags(flags | G_APPLICATION_NON_UNIQUE);
	g_set_prgname("omapad");
	gtk_window_set_default_icon_name("omapad");
	GtkApplication *application = gtk_application_new(kAppId, flags);
	auto app = std::make_unique<App>(application);
	App *pointer = app.get();
	g_signal_connect(application, "startup", G_CALLBACK(+[](GApplication *, gpointer self) { static_cast<App *>(self)->build(); }), pointer);
	g_signal_connect(application, "activate", G_CALLBACK(+[](GApplication *, gpointer self) {
		App *a = static_cast<App *>(self);
		// Switches with no files still count.
		CommandLine line = gCommandLine;
		gCommandLine = CommandLine();
		a->workspace->applyCommandLine(line);
		a->present();
	}), pointer);
	g_signal_connect(application, "open", G_CALLBACK(+[](GApplication *, GFile **files, gint count, const gchar *, gpointer self) {
		App *a = static_cast<App *>(self);
		std::vector<std::string> paths;
		for (gint i = 0; i < count; i++) {
			const std::string path = pathFromFile(files[i]);
			if (!path.empty()) paths.push_back(path);
		}
		CommandLine line = gCommandLine;
		line.files = paths;
		gCommandLine = CommandLine();
		a->workspace->applyCommandLine(line);
		a->present();
	}), pointer);
	const int status = g_application_run(G_APPLICATION(application), remainingCount, remaining.data());
	g_object_unref(application);
	return status;
}
