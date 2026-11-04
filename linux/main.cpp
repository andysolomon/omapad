// The Linux window around the shared Workspace, using GTK3 (Scintilla's GTK port).
// Built for Omarchy on Hyprland, and works on any GTK3 desktop.
#include <gtk/gtk.h>

#include "Scintilla.h"
#include "ScintillaWidget.h"
#include "Workspace.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <sys/resource.h>
#include <unistd.h>
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

bool isCheckable(Cmd cmd) {
	static const std::set<Cmd> checkable = {
		Cmd::AlwaysOnTop, Cmd::FullScreen, Cmd::ToggleSpace, Cmd::ToggleEol, Cmd::ToggleAllCharacters, Cmd::ToggleGuides,
		Cmd::ToggleWrapSymbol, Cmd::ToggleWrap, Cmd::ToggleResults, Cmd::RotateSplit, Cmd::SyncVertical, Cmd::SyncHorizontal,
		Cmd::ToggleFolderPanel, Cmd::ToggleHiddenInFolders, Cmd::EolConvert, Cmd::ReopenEncoding, Cmd::ConvertEncoding,
		Cmd::SetLanguage, Cmd::SetTheme, Cmd::SetTabWidth, Cmd::ToggleInsertSpaces, Cmd::SelectDocument,
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
	int ask(const std::string &title, const std::string &detail, const std::vector<std::string> &buttons) override;
	bool chooseFilesToOpen(const std::string &start, std::vector<std::string> &out) override;
	bool chooseSavePath(const std::string &name, const std::string &start, std::string &out) override;
	bool chooseFolder(const std::string &start, std::string &out) override;
	bool chooseFont(std::string &family, double &size) override;
	bool promptText(const std::string &title, const std::string &label, const std::string &initial, std::string &out) override;
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
		if (panes[pane].sci.widget) gtk_widget_grab_focus(panes[pane].sci.widget);
	}
	void themeChanged() override;
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
	GtkWidget *nativeMenu(const std::vector<MenuItem> &items, GtkAccelGroup *accel);
	void refreshMenu(GtkWidget *menu);
	void popup(const std::vector<MenuItem> &items);
	void buildFolderPanel();
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
	GtkSciView results, background;
	GtkWidget *editorPaned = nullptr, *rightPaned = nullptr, *outerPaned = nullptr;
	GtkWidget *statusLeft = nullptr, *statusRight = nullptr, *statusMessage = nullptr;
	guint flashTimer = 0, activationTimer = 0, themeTimer = 0;
	int dialogDepth = 0;
	bool settingMenus = false;
	bool splitPositioned = false;
	bool reloadingFolders = false;

	GtkWidget *folderBox = nullptr, *folderView = nullptr;
	GtkTreeStore *folderStore = nullptr;

	GtkWidget *findWindow = nullptr, *findTabs[3] = {nullptr, nullptr, nullptr};
	GtkWidget *what = nullptr, *with = nullptr, *filters = nullptr, *directory = nullptr;
	GtkWidget *matchCase = nullptr, *wholeWord = nullptr, *wrap = nullptr, *backward = nullptr, *inSelection = nullptr;
	GtkWidget *recursive = nullptr, *hidden = nullptr, *mode = nullptr, *findStatus = nullptr;
	GtkWidget *withRow = nullptr, *filtersRow = nullptr, *directoryRow = nullptr, *filesOptions = nullptr, *selectionRow = nullptr;
	GtkWidget *findButtons = nullptr, *replaceButtons = nullptr, *filesButtons = nullptr;
	GtkWidget *findInFilesButton = nullptr, *stopButton = nullptr;
	std::vector<std::string> whatHistory, withHistory;
	FindTab findTab = FindTab::Find;

	GtkCssProvider *themeCss = nullptr;
	std::vector<GFileMonitor *> themeMonitors;
};

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
	gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(row), close, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(events), row);
	gtk_widget_show_all(events);
	g_object_set_data(G_OBJECT(events), "omapad-label", label);
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
	const std::string title = (info.dirty ? "● " : "") + info.title;
	gtk_label_set_text(GTK_LABEL(label), title.c_str());
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
}

void App::setResultsVisible(bool visible) {
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
	GtkWidget *widget;
	if (topLevel) {
		const std::string title = "_" + item.title;
		widget = gtk_menu_item_new_with_mnemonic(title.c_str());
	} else if (isCheckable(item.cmd)) {
		widget = gtk_check_menu_item_new_with_label(item.title.c_str());
		if (item.cmd == Cmd::SetLanguage || item.cmd == Cmd::SetTheme || item.cmd == Cmd::EolConvert || item.cmd == Cmd::SetTabWidth ||
		    item.cmd == Cmd::ReopenEncoding || item.cmd == Cmd::ConvertEncoding || item.cmd == Cmd::SelectDocument)
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
		gchar **parts = g_strsplit(item.shortcut.c_str(), "+", -1);
		for (gchar **part = parts; *part; part++) {
			const std::string name = *part;
			if (name == "Mod" || name == "Ctrl") mods = GdkModifierType(mods | GDK_CONTROL_MASK);
			else if (name == "Alt") mods = GdkModifierType(mods | GDK_MOD1_MASK);
			else if (name == "Shift") mods = GdkModifierType(mods | GDK_SHIFT_MASK);
			else if (name == "/") key = GDK_KEY_slash;
			else if (name == "=") key = GDK_KEY_equal;
			else if (name == "-") key = GDK_KEY_minus;
			else if (name == "[") key = GDK_KEY_bracketleft;
			else if (name == "]") key = GDK_KEY_bracketright;
			else if (name == "PageUp") key = GDK_KEY_Page_Up;
			else if (name == "PageDown") key = GDK_KEY_Page_Down;
			else if (name.size() == 1) key = gdk_keyval_from_name(g_ascii_strdown(name.c_str(), -1));
			else key = gdk_keyval_from_name(name.c_str());
		}
		g_strfreev(parts);
		if (key != 0 && key != GDK_KEY_VoidSymbol) {
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
	gtk_widget_set_visible(folderBox, visible);
	if (visible) {
		gtk_widget_show_all(folderBox);
		gtk_paned_set_position(GTK_PANED(outerPaned), 240);
		foldersChanged();
	}
}

#pragma mark - Find window

void App::buildFindWindow() {
	findWindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(findWindow), "Find");
	gtk_window_set_transient_for(GTK_WINDOW(findWindow), GTK_WINDOW(window));
	gtk_window_set_type_hint(GTK_WINDOW(findWindow), GDK_WINDOW_TYPE_HINT_DIALOG);
	gtk_window_set_resizable(GTK_WINDOW(findWindow), TRUE);
	gtk_window_set_default_size(GTK_WINDOW(findWindow), 620, -1);
	g_signal_connect(findWindow, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), nullptr);
	g_signal_connect(findWindow, "key-press-event", G_CALLBACK(+[](GtkWidget *w, GdkEventKey *event, gpointer) -> gboolean {
		if (event->keyval != GDK_KEY_Escape) return FALSE;
		gtk_widget_hide(w);
		return TRUE;
	}), nullptr);

	GtkWidget *stack = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_container_set_border_width(GTK_CONTAINER(stack), 12);
	gtk_container_add(GTK_CONTAINER(findWindow), stack);

	GtkWidget *tabs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(tabs), "linked");
	const char *titles[] = {"Find", "Replace", "Find in Files"};
	for (int i = 0; i < 3; i++) {
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
	gtk_box_pack_start(GTK_BOX(directoryControl), directory, TRUE, TRUE, 0);
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
	selectionRow = checkRow({inSelection});
	filesOptions = checkRow({recursive, hidden});

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
	findButtons = buttonRow({{"Find Next", [w] { w->findPanelFindNext(); }}, {"Count", [w] { w->findPanelCount(); }}, {"Find All", [w] { w->findPanelFindAll(); }},
	                         {"Mark All", [w] { w->findPanelMarkAll(); }}, {"Clear Marks", [w] { w->findPanelClearMarks(); }}});
	replaceButtons = buttonRow({{"Find Next", [w] { w->findPanelFindNext(); }}, {"Replace", [w] { w->findPanelReplace(); }}, {"Replace All", [w] { w->findPanelReplaceAll(); }},
	                            {"In All Open Documents", [w] { w->findPanelReplaceAllOpen(); }}});
	filesButtons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	findInFilesButton = gtk_button_new_with_label("Find All");
	stopButton = gtk_button_new_with_label("Stop");
	gtk_widget_set_sensitive(stopButton, FALSE);
	gtk_box_pack_start(GTK_BOX(filesButtons), findInFilesButton, FALSE, FALSE, 0);
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
	gtk_widget_set_visible(withRow, tab == FindTab::Replace);
	gtk_widget_set_visible(filtersRow, isFiles);
	gtk_widget_set_visible(directoryRow, isFiles);
	gtk_widget_set_visible(filesOptions, isFiles);
	gtk_widget_set_visible(selectionRow, !isFiles);
	gtk_widget_set_visible(findButtons, tab == FindTab::Find);
	gtk_widget_set_visible(replaceButtons, tab == FindTab::Replace);
	gtk_widget_set_visible(filesButtons, isFiles);
	gtk_widget_set_visible(backward, !isFiles);
	gtk_widget_set_visible(wrap, !isFiles);
	const char *titles[] = {"Find", "Replace", "Find in Files"};
	gtk_window_set_title(GTK_WINDOW(findWindow), titles[static_cast<int>(tab)]);
	gtk_label_set_text(GTK_LABEL(findStatus), "");
	gtk_window_resize(GTK_WINDOW(findWindow), 620, 1);
}

void App::showFind(FindTab tab, const std::string &prefill, const std::string &folder) {
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
	std::string folder = gtk_entry_get_text(GTK_ENTRY(directory));
	if (folder.rfind("~/", 0) == 0 || folder == "~") folder = std::string(g_get_home_dir()) + folder.substr(1);
	o.directory = folder;
	o.filters = gtk_entry_get_text(GTK_ENTRY(filters));
	o.recursive = on(recursive);
	o.includeHidden = on(hidden);
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
	background.widget = scintilla_new();
	g_object_ref_sink(background.widget);

	editorPaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_paned_pack1(GTK_PANED(editorPaned), panes[0].box, TRUE, FALSE);
	gtk_paned_pack2(GTK_PANED(editorPaned), panes[1].box, TRUE, FALSE);
	rightPaned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
	gtk_paned_pack1(GTK_PANED(rightPaned), editorPaned, TRUE, FALSE);
	gtk_paned_pack2(GTK_PANED(rightPaned), results.widget, FALSE, FALSE);
	buildFolderPanel();
	outerPaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_paned_pack1(GTK_PANED(outerPaned), folderBox, FALSE, FALSE);
	gtk_paned_pack2(GTK_PANED(outerPaned), rightPaned, TRUE, FALSE);

	GtkWidget *status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
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
	gtk_box_pack_start(GTK_BOX(status), statusLeft, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(status), statusMessage, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(status), statusRight, FALSE, FALSE, 0);

	GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_container_add(GTK_CONTAINER(window), root);

	workspace->start();

	GtkAccelGroup *accel = gtk_accel_group_new();
	gtk_window_add_accel_group(GTK_WINDOW(window), accel);
	GtkWidget *menubar = gtk_menu_bar_new();
	for (const MenuItem &top : workspace->mainMenu()) gtk_menu_shell_append(GTK_MENU_SHELL(menubar), nativeItem(top, accel, true));
	gtk_box_pack_start(GTK_BOX(root), menubar, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(root), outerPaned, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(root), status, FALSE, FALSE, 0);

	buildFindWindow();

	g_signal_connect(window, "delete-event", G_CALLBACK(+[](GtkWidget *, GdkEvent *, gpointer self) -> gboolean {
		static_cast<App *>(self)->tryQuit();
		return TRUE;
	}), this);
	g_signal_connect(window, "notify::is-active", G_CALLBACK(+[](GObject *object, GParamSpec *, gpointer self) {
		if (gtk_window_is_active(GTK_WINDOW(object))) static_cast<App *>(self)->scheduleActivationCheck();
	}), this);
	g_timeout_add_seconds(7, +[](gpointer self) -> gboolean {
		static_cast<App *>(self)->workspace->tick();
		return G_SOURCE_CONTINUE;
	}, this);

	gtk_widget_show_all(root);
	gtk_widget_hide(folderBox);
	gtk_widget_hide(panes[1].box);
	gtk_widget_hide(results.widget);
	themeChanged();
	watchTheme();
	tabsChanged();
	workspace->restoreSession();
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

// libstdc++'s regex recurses once per character, so a long line in the document being
// searched can overflow an ordinary stack and crash. Scintilla searches on this thread, so
// give it a large stack, which is only reserved rather than used. This takes a new exec.
static void ensureLargeStack(char **argv) {
	const rlim_t wanted = 1024UL * 1024UL * 1024UL;
	struct rlimit limit;
	if (getrlimit(RLIMIT_STACK, &limit) != 0 || limit.rlim_cur >= wanted || limit.rlim_cur == RLIM_INFINITY) return;
	if (limit.rlim_max != RLIM_INFINITY && limit.rlim_max < wanted) return;
	limit.rlim_cur = wanted;
	if (setrlimit(RLIMIT_STACK, &limit) != 0) return;
	std::error_code error;
	const fs::path executable = fs::read_symlink("/proc/self/exe", error);
	if (!error) execv(executable.c_str(), argv);
	// If that fails, carry on with what we have.
}

int main(int argc, char **argv) {
	ensureLargeStack(argv);
	GApplicationFlags flags = G_APPLICATION_HANDLES_OPEN;
	if (g_getenv("OMAPAD_NON_UNIQUE")) flags = GApplicationFlags(flags | G_APPLICATION_NON_UNIQUE);
	g_set_prgname("omapad");
	gtk_window_set_default_icon_name("omapad");
	GtkApplication *application = gtk_application_new(kAppId, flags);
	auto app = std::make_unique<App>(application);
	App *pointer = app.get();
	g_signal_connect(application, "startup", G_CALLBACK(+[](GApplication *, gpointer self) { static_cast<App *>(self)->build(); }), pointer);
	g_signal_connect(application, "activate", G_CALLBACK(+[](GApplication *, gpointer self) { static_cast<App *>(self)->present(); }), pointer);
	g_signal_connect(application, "open", G_CALLBACK(+[](GApplication *, GFile **files, gint count, const gchar *, gpointer self) {
		App *a = static_cast<App *>(self);
		std::vector<std::string> paths;
		for (gint i = 0; i < count; i++) {
			const std::string path = pathFromFile(files[i]);
			if (!path.empty()) paths.push_back(path);
		}
		a->openPaths(paths);
		a->present();
	}), pointer);
	const int status = g_application_run(G_APPLICATION(application), argc, argv);
	g_object_unref(application);
	return status;
}
