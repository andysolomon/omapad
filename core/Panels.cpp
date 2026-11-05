// Side panels, the toolbar and context menu, Style Configurator, plugins and the tools Notepad++ ships as plugins.
#include "WorkspaceInternal.h"
#include "OmapadPlugin.h"

#include "SciLexer.h"
#include "Scintilla.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace omapad {

namespace {

std::string readFileText(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	std::stringstream out;
	out << in.rdbuf();
	return out.str();
}

FormField simpleField(FormField::Kind kind, const std::string &key, const std::string &label, const std::string &value = std::string()) {
	FormField field;
	field.kind = kind;
	field.key = key;
	field.label = label;
	field.value = value;
	return field;
}

std::string lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return value;
}

std::string htmlName(int code) {
	static const std::map<int, std::string> names = {
#include "HtmlEntities.inc"
	};
	const auto found = names.find(code);
	return found == names.end() ? std::string() : found->second;
}

std::string valueOf(const std::vector<FormField> &fields, const std::string &key) {
	for (const FormField &field : fields)
		if (field.key == key) return field.value;
	return std::string();
}

}  // namespace

#pragma mark - Panels

std::string tabDisplayTitle(const TabInfo &info, const TabStyle &style) {
	std::string out;
	if (info.pinned) out += "⚲ ";
	if (info.monitoring) out += style.alternateIcons ? "👁 " : "◉ ";
	if (info.readOnly) out += style.alternateIcons ? "🔒 " : "⊘ ";
	if (info.dirty) out += style.alternateIcons ? "✎ " : "● ";
	return out + info.title;
}

const char *panelTitle(int panel) {
	switch (panel) {
	case kPanelClipboard: return "Clipboard History";
	case kPanelCharacters: return "Character Panel";
	case kPanelDocuments: return "Document List";
	case kPanelProject1: return "Project Panel 1";
	case kPanelProject2: return "Project Panel 2";
	case kPanelProject3: return "Project Panel 3";
	default: return "";
	}
}

bool Workspace::panelVisible(int panel) const { return panel >= 0 && panel < kPanelCount && panelShown[panel]; }

void Workspace::togglePanel(int panel) {
	if (panel < 0 || panel >= kPanelCount) return;
	panelShown[panel] = !panelShown[panel];
	host.setPanelVisible(panel, panelTitle(panel), panelShown[panel]);
	if (panelShown[panel]) host.panelChanged(panel);
}

std::vector<PanelRow> Workspace::panelRows(int panel) const {
	std::vector<PanelRow> rows;
	switch (panel) {
	case kPanelClipboard:
		for (const std::string &entry : clipboardHistory) {
			std::string shown = entry.substr(0, 80);
			std::replace(shown.begin(), shown.end(), '\n', ' ');
			std::replace(shown.begin(), shown.end(), '\r', ' ');
			rows.push_back({shown, entry.size() > 2000 ? entry.substr(0, 2000) + "…" : entry, 0});
		}
		break;
	case kPanelCharacters:
		for (int code = 0; code < 256; code++) {
			static const char *const controls[] = {"NULL", "SOH", "STX", "ETX", "EOT", "ENQ", "ACK", "BEL", "BS", "TAB", "LF", "VT", "FF", "CR", "SO", "SI", "DLE", "DC1", "DC2", "DC3", "DC4", "NAK", "SYN", "ETB", "CAN", "EM", "SUB", "ESC", "FS", "GS", "RS", "US"};
			std::string glyph;
			if (code < 32) glyph = controls[code];
			else if (code == 127) glyph = "DEL";
			else appendUtf8(glyph, static_cast<unsigned long>(code));
			char hex[8], decimal[12], htmlHex[12];
			snprintf(hex, sizeof hex, "%02X", code);
			snprintf(decimal, sizeof decimal, "&#%d;", code);
			snprintf(htmlHex, sizeof htmlHex, "&#x%X;", code);
			const bool printable = code >= 33 && code != 127;
			PanelRow row;
			row.title = std::to_string(code) + "  " + hex + "  " + glyph;
			row.toolTip = decimal;
			row.columns = {std::to_string(code), hex, glyph, printable ? htmlName(code) : std::string(), printable ? decimal : std::string(), printable ? htmlHex : std::string()};
			rows.push_back(row);
		}
		break;
	case kPanelDocuments:
		for (const auto &where : documentListOrder()) {
			PanelRow row;
			if (where.first < 0) {
				row.title = where.second == 0 ? "First view" : "Second view";
				row.columns = {row.title, "", ""};
				rows.push_back(row);
				continue;
			}
			const Tab &tab = paneAt(where.first).tabs[static_cast<size_t>(where.second)];
			const Document &document = *tab.document;
			const std::string name = document.title();
			const size_t dot = name.rfind('.');
			row.title = (document.dirty() ? "● " : "") + name;
			row.toolTip = document.path.empty() ? name : document.path;
			row.columns = {row.title};
			if (settings.prefOn("docListExtColumn")) row.columns.push_back(dot == std::string::npos || dot == 0 ? std::string() : name.substr(dot + 1));
			if (settings.prefOn("docListPathColumn")) row.columns.push_back(document.path);
			rows.push_back(row);
		}
		break;
	default:
		if (panel >= kPanelProject1 && panel <= kPanelProject3) {
			const ProjectPanel &project = projectPanels[panel - kPanelProject1];
			std::function<void(const ProjectNode &, int)> add = [&](const ProjectNode &node, int depth) {
				const std::string arrow = node.kind == ProjectNode::Kind::File ? "" : node.expanded ? "▾ " : "▸ ";
				rows.push_back({arrow + node.name, node.kind == ProjectNode::Kind::File ? node.path : node.name, depth});
				if (node.expanded)
					for (const ProjectNode &child : node.children) add(child, depth + 1);
			};
			add(project.root, 0);
		}
		break;
	}
	return rows;
}

std::vector<std::string> Workspace::panelColumns(int panel) const {
	if (panel == kPanelCharacters) return {"Value", "Hex", "Character", "HTML Name", "HTML Decimal", "HTML Hexadecimal"};
	if (panel == kPanelDocuments) {
		std::vector<std::string> out = {"Name"};
		if (settings.prefOn("docListExtColumn")) out.push_back("Ext.");
		if (settings.prefOn("docListPathColumn")) out.push_back("Path");
		return out;
	}
	return {};
}

std::vector<std::pair<int, int>> Workspace::documentListOrder() const {
	const bool grouped = settings.prefOn("docListGrouped");
	std::vector<std::pair<int, int>> out;
	const auto key = [&](const std::pair<int, int> &where) {
		const Document &document = *paneAt(where.first).tabs[static_cast<size_t>(where.second)].document;
		const std::string name = lower(document.title());
		if (documentSortColumn == 1) {
			const size_t dot = name.rfind('.');
			return dot == std::string::npos ? std::string() : name.substr(dot + 1);
		}
		if (documentSortColumn == 2) return lower(document.path);
		return name;
	};
	const auto sorted = [&](std::vector<std::pair<int, int>> &list) {
		if (documentSortColumn < 0) return;
		std::stable_sort(list.begin(), list.end(), [&](const auto &a, const auto &b) { return documentSortDescending ? key(b) < key(a) : key(a) < key(b); });
	};
	std::vector<std::pair<int, int>> all;
	for (int pn = 0; pn < 2; pn++) {
		std::vector<std::pair<int, int>> mine;
		for (int i = 0; i < static_cast<int>(paneAt(pn).tabs.size()); i++) mine.emplace_back(pn, i);
		if (grouped) {
			if (mine.empty()) continue;
			sorted(mine);
			out.emplace_back(-1, pn);
			out.insert(out.end(), mine.begin(), mine.end());
		} else {
			all.insert(all.end(), mine.begin(), mine.end());
		}
	}
	if (!grouped) {
		sorted(all);
		out = all;
	}
	return out;
}

void Workspace::panelColumnClicked(int panel, int column) {
	if (panel != kPanelDocuments) return;
	// The visible columns can leave out Ext., which moves Path along.
	int kind = column;
	if (column == 1 && !settings.prefOn("docListExtColumn")) kind = 2;
	if (documentSortColumn == kind) documentSortDescending = !documentSortDescending;
	else documentSortDescending = false;
	documentSortColumn = kind;
	host.panelChanged(panel);
}

void Workspace::panelActivate(int panel, int row, int column) {
	if (panel == kPanelClipboard) {
		if (row >= 0 && row < static_cast<int>(clipboardHistory.size())) {
			const std::string &text = clipboardHistory[static_cast<size_t>(row)];
			s(SCI_BEGINUNDOACTION);
			s(SCI_REPLACESEL, 0, reinterpret_cast<intptr_t>(""));
			s(SCI_ADDTEXT, text.size(), reinterpret_cast<intptr_t>(text.data()));
			s(SCI_ENDUNDOACTION);
			host.focusPane(focus);
		}
	} else if (panel == kPanelCharacters) {
		if (row >= 0 && row < 256) {
			// The Character column types the character. The others type what they show.
			std::string glyph;
			if (column == 2 || column < 0 || column > 5) {
				if (row >= 32 && row != 127) appendUtf8(glyph, static_cast<unsigned long>(row));
				else glyph.assign(1, static_cast<char>(row));
			} else {
				glyph = panelRows(panel)[static_cast<size_t>(row)].columns[static_cast<size_t>(column)];
			}
			if (glyph.empty()) return;
			s(SCI_REPLACESEL, 0, reinterpret_cast<intptr_t>(""));
			s(SCI_ADDTEXT, glyph.size(), reinterpret_cast<intptr_t>(glyph.data()));
			host.focusPane(focus);
		}
	} else if (panel == kPanelDocuments) {
		const std::vector<std::pair<int, int>> order = documentListOrder();
		if (row >= 0 && row < static_cast<int>(order.size()) && order[static_cast<size_t>(row)].first >= 0) clickTab(order[static_cast<size_t>(row)].first, order[static_cast<size_t>(row)].second);
	} else if (panel >= kPanelProject1 && panel <= kPanelProject3) {
		ProjectNode *node = projectNodeAt(panel, row);
		if (!node) return;
		if (node->kind == ProjectNode::Kind::File) {
			openPath(node->path);
		} else {
			node->expanded = !node->expanded;
			host.panelChanged(panel);
		}
	}
}

std::vector<MenuItem> Workspace::panelMenu(int panel, int row) const {
	std::vector<MenuItem> out;
	const auto add = [&](const std::string &title, const std::string &action) {
		MenuItem item;
		item.title = title;
		item.cmd = Cmd::PanelCommand;
		item.arg = panel * 100000 + std::max(row, 0);
		item.text = action;
		out.push_back(item);
	};
	const auto separator = [&]() {
		MenuItem item;
		item.separator = true;
		out.push_back(item);
	};
	if (panel == kPanelClipboard) {
		add("Clear History", "clear");
	} else if (panel == kPanelDocuments) {
		add("Close", "closeDocument");
		add("Save", "saveDocument");
		separator();
		add(settings.prefOn("docListGrouped") ? "Don't Group by View" : "Group by View", "groupByView");
		add("Sort by Name", "sortName");
		add("Sort by Ext.", "sortExt");
		add("Sort by Path", "sortPath");
		add("Tab Order", "sortNone");
	} else if (panel >= kPanelProject1 && panel <= kPanelProject3) {
		add("New Workspace", "wsNew");
		add("Open Workspace…", "wsOpen");
		add("Reload Workspace", "wsReload");
		add("Save", "wsSave");
		add("Save As…", "wsSaveAs");
		add("Save a Copy As…", "wsCopy");
		separator();
		add("Add New Project", "projectAdd");
		add("Find in Projects…", "projectFind");
		const ProjectNode *node = const_cast<Workspace *>(this)->projectNodeAt(panel, row);
		if (node && node->kind != ProjectNode::Kind::Root) {
			separator();
			if (node->kind != ProjectNode::Kind::File) {
				add("Add Folder", "folderAdd");
				add("Add Files…", "filesAdd");
				add("Add Files from Directory…", "dirAdd");
			}
			add("Rename", "nodeRename");
			if (node->kind == ProjectNode::Kind::File) add("Modify File Path…", "nodeEditPath");
			add("Remove", "nodeRemove");
			add("Move Up", "nodeUp");
			add("Move Down", "nodeDown");
		}
	}
	return out;
}

void Workspace::notePanelsChanged() {
	if (panelShown[kPanelDocuments]) host.panelChanged(kPanelDocuments);
}

void Workspace::pollClipboard() {
	if (!panelShown[kPanelClipboard]) return;
	std::string text;
	if (!host.clipboardData("text", text) || text.empty() || text == lastClipboard) return;
	lastClipboard = text;
	clipboardHistory.erase(std::remove(clipboardHistory.begin(), clipboardHistory.end(), text), clipboardHistory.end());
	clipboardHistory.insert(clipboardHistory.begin(), text);
	if (clipboardHistory.size() > 30) clipboardHistory.resize(30);
	host.panelChanged(kPanelClipboard);
}

#pragma mark - Project panels

Workspace::ProjectNode *Workspace::projectNodeAt(int panel, int row) {
	if (panel < kPanelProject1 || panel > kPanelProject3 || row < 0) return nullptr;
	int remaining = row;
	std::function<ProjectNode *(ProjectNode &)> walk = [&](ProjectNode &node) -> ProjectNode * {
		if (remaining-- == 0) return &node;
		if (node.expanded) {
			for (ProjectNode &child : node.children)
				if (ProjectNode *found = walk(child)) return found;
		}
		return nullptr;
	};
	return walk(projectPanels[panel - kPanelProject1].root);
}

Workspace::ProjectNode *Workspace::projectParentOf(int panel, ProjectNode *target) {
	std::function<ProjectNode *(ProjectNode &)> walk = [&](ProjectNode &node) -> ProjectNode * {
		for (ProjectNode &child : node.children) {
			if (&child == target) return &node;
			if (ProjectNode *found = walk(child)) return found;
		}
		return nullptr;
	};
	return walk(projectPanels[panel - kPanelProject1].root);
}

namespace {

// Paths in a workspace file are relative to it, so a workspace can move with its project.
void readProjectNode(const pugi::xml_node &xml, Workspace::ProjectNode &out, const fs::path &base) {
	for (const pugi::xml_node child : xml.children()) {
		const std::string tag = child.name();
		Workspace::ProjectNode node;
		node.name = child.attribute("name").as_string();
		if (tag == "File") {
			node.kind = Workspace::ProjectNode::Kind::File;
			fs::path path(node.name);
			if (path.is_relative()) path = base / path;
			node.path = path.lexically_normal().string();
			node.name = path.filename().string();
		} else if (tag == "Folder" || tag == "Project") {
			node.kind = tag == "Folder" ? Workspace::ProjectNode::Kind::Folder : Workspace::ProjectNode::Kind::Project;
			node.expanded = child.attribute("expanded").as_bool(true);
			readProjectNode(child, node, base);
		} else {
			continue;
		}
		out.children.push_back(std::move(node));
	}
}

void writeProjectNode(pugi::xml_node &xml, const Workspace::ProjectNode &node, const fs::path &base) {
	for (const Workspace::ProjectNode &child : node.children) {
		if (child.kind == Workspace::ProjectNode::Kind::File) {
			pugi::xml_node file = xml.append_child("File");
			file.append_attribute("name") = fs::path(child.path).lexically_relative(base).string().c_str();
		} else {
			pugi::xml_node folder = xml.append_child(child.kind == Workspace::ProjectNode::Kind::Folder ? "Folder" : "Project");
			folder.append_attribute("name") = child.name.c_str();
			folder.append_attribute("expanded") = child.expanded;
			writeProjectNode(folder, child, base);
		}
	}
}

}  // namespace

bool Workspace::loadWorkspaceFile(int panel, const std::string &path) {
	pugi::xml_document document;
	if (!document.load_file(path.c_str())) return false;
	const pugi::xml_node root = document.child("NotepadPlus");
	if (!root) return false;
	ProjectPanel &project = projectPanels[panel - kPanelProject1];
	project.file = path;
	project.root = ProjectNode();
	project.root.kind = ProjectNode::Kind::Root;
	project.root.name = baseName(path);
	project.root.expanded = true;
	readProjectNode(root, project.root, fs::path(path).parent_path());
	host.panelChanged(panel);
	return true;
}

bool Workspace::saveWorkspaceFile(int panel, const std::string &path, bool adopt) {
	ProjectPanel &project = projectPanels[panel - kPanelProject1];
	pugi::xml_document document;
	pugi::xml_node declaration = document.append_child(pugi::node_declaration);
	declaration.append_attribute("version") = "1.0";
	declaration.append_attribute("encoding") = "UTF-8";
	pugi::xml_node root = document.append_child("NotepadPlus");
	writeProjectNode(root, project.root, fs::path(path).parent_path());
	std::ostringstream out;
	document.save(out, "    ", pugi::format_default, pugi::encoding_utf8);
	std::string error;
	if (!atomicWrite(out.str(), path, 0644, error)) {
		host.ask("The workspace could not be saved.", error, {"OK"});
		return false;
	}
	if (adopt) {
		project.file = path;
		project.root.name = baseName(path);
		host.panelChanged(panel);
	}
	return true;
}

void Workspace::findInProjects() {
	std::vector<FormField> fields;
	fields.push_back(simpleField(FormField::Kind::Text, "what", "Find what"));
	fields.push_back(simpleField(FormField::Kind::Bool, "doReplace", "Replace in the project files", "0"));
	fields.push_back(simpleField(FormField::Kind::Text, "with", "Replace with"));
	for (int i = 0; i < 3; i++)
		fields.push_back(simpleField(FormField::Kind::Bool, "panel" + std::to_string(i), std::string("Search ") + panelTitle(kPanelProject1 + i), panelShown[kPanelProject1 + i] ? "1" : "0"));
	fields.push_back(simpleField(FormField::Kind::Bool, "matchCase", "Match case", "0"));
	fields.push_back(simpleField(FormField::Kind::Bool, "wholeWord", "Match whole word only", "0"));
	if (!host.runForm("Find in Projects", fields)) return;
	FindOptions options;
	options.what = valueOf(fields, "what");
	if (options.what.empty()) return;
	options.matchCase = valueOf(fields, "matchCase") == "1";
	options.wholeWord = valueOf(fields, "wholeWord") == "1";
	std::function<void(const ProjectNode &)> gather = [&](const ProjectNode &node) {
		if (node.kind == ProjectNode::Kind::File) options.files.push_back(node.path);
		for (const ProjectNode &child : node.children) gather(child);
	};
	for (int i = 0; i < 3; i++)
		if (valueOf(fields, "panel" + std::to_string(i)) == "1") gather(projectPanels[i].root);
	if (options.files.empty()) {
		host.flash("No files in those panels");
		return;
	}
	options.directory = "projects";
	if (valueOf(fields, "doReplace") == "1") {
		options.with = valueOf(fields, "with");
		if (host.ask("Replace in " + std::to_string(options.files.size()) + " files?", "Files that aren't open change on disk straight away, and this can't be undone.", {"Cancel", "Replace"}) != 1) return;
		const long count = replaceInFileList(options, options.files);
		host.flash(count < 0 ? "That regular expression isn't valid" : "Replaced " + std::to_string(count));
		refreshChrome();
		return;
	}
	showResults(true);
	findInFiles(options, [this](long matches, long files) { host.flash(std::to_string(matches) + " matches in " + std::to_string(files) + " files"); });
}

void Workspace::panelCommand(int panel, int row, const std::string &action) {
	if (panel == kPanelClipboard) {
		if (action == "clear") {
			clipboardHistory.clear();
			host.panelChanged(panel);
		}
		return;
	}
	if (panel == kPanelDocuments) {
		if (action.rfind("sort", 0) == 0) {
			documentSortColumn = action == "sortName" ? 0 : action == "sortExt" ? 1 : action == "sortPath" ? 2 : -1;
			documentSortDescending = false;
			host.panelChanged(panel);
			return;
		}
		if (action == "groupByView") {
			settings.prefs["docListGrouped"] = settings.prefOn("docListGrouped") ? "0" : "1";
			saveSettings();
			host.panelChanged(panel);
			return;
		}
		const std::vector<std::pair<int, int>> order = documentListOrder();
		if (row < 0 || row >= static_cast<int>(order.size()) || order[static_cast<size_t>(row)].first < 0) return;
		const auto where = order[static_cast<size_t>(row)];
		if (action == "closeDocument") {
			closeTab(where.first, where.second);
		} else {
			const std::shared_ptr<Document> document = paneAt(where.first).tabs[static_cast<size_t>(where.second)].document;
			if (document->path.empty()) saveAsDialog(*document);
			else saveDocument(*document, document->path);
		}
		notePanelsChanged();
		return;
	}
	if (panel < kPanelProject1 || panel > kPanelProject3) return;
	ProjectPanel &project = projectPanels[panel - kPanelProject1];
	ProjectNode *node = projectNodeAt(panel, row);
	const std::string extension = settings.pref("workspaceExtension");
	const auto ask = [&](const char *title, const std::string &initial, std::string &out) { return host.promptText(title, "Name:", initial, out) && !out.empty(); };
	if ((action == "wsNew" || action == "wsOpen" || action == "wsReload") && workspaceDirty[panel - kPanelProject1]) {
		const int choice = host.ask("Save the changes to this workspace first?", project.file.empty() ? "It hasn't been saved yet." : project.file, {"Cancel", "Don't Save", "Save"});
		if (choice <= 0) return;
		if (choice == 2) {
			if (!project.file.empty()) {
				saveWorkspaceFile(panel, project.file, true);
			} else {
				std::string path;
				if (!host.chooseSavePath("workspace" + extension, dialogDirectory(), path) || !saveWorkspaceFile(panel, path, true)) return;
			}
		}
		workspaceDirty[panel - kPanelProject1] = false;
	}
	if (action == "wsNew") {
		project = ProjectPanel();
		project.root.kind = ProjectNode::Kind::Root;
		project.root.name = "Workspace";
		project.root.expanded = true;
	} else if (action == "wsOpen") {
		std::vector<std::string> paths;
		if (host.chooseFilesToOpen(std::string(), paths) && !paths.empty() && !loadWorkspaceFile(panel, paths[0]))
			host.ask("That isn't a workspace file.", paths[0], {"OK"});
	} else if (action == "wsReload") {
		if (!project.file.empty()) loadWorkspaceFile(panel, project.file);
	} else if (action == "wsSave" && !project.file.empty()) {
		saveWorkspaceFile(panel, project.file, true);
	} else if (action == "wsSave" || action == "wsSaveAs" || action == "wsCopy") {
		std::string path;
		if (host.chooseSavePath("workspace" + extension, std::string(), path)) saveWorkspaceFile(panel, path, action != "wsCopy");
	} else if (action == "projectAdd") {
		std::string name;
		if (ask("Add New Project", "Project", name)) {
			ProjectNode added;
			added.kind = ProjectNode::Kind::Project;
			added.name = name;
			added.expanded = true;
			project.root.children.push_back(std::move(added));
			project.root.expanded = true;
		}
	} else if (action == "projectFind") {
		findInProjects();
		return;
	} else if (!node) {
		return;
	} else if (action == "folderAdd") {
		std::string name;
		if (ask("Add Folder", "Folder", name)) {
			ProjectNode added;
			added.kind = ProjectNode::Kind::Folder;
			added.name = name;
			node->children.push_back(std::move(added));
			node->expanded = true;
		}
	} else if (action == "filesAdd") {
		std::vector<std::string> paths;
		if (host.chooseFilesToOpen(std::string(), paths)) {
			for (const std::string &path : paths) {
				ProjectNode file;
				file.kind = ProjectNode::Kind::File;
				file.path = path;
				file.name = baseName(path);
				node->children.push_back(std::move(file));
			}
			node->expanded = true;
		}
	} else if (action == "dirAdd") {
		std::string directory;
		if (host.chooseFolder(std::string(), directory)) {
			std::function<void(const fs::path &, ProjectNode &)> addAll = [&](const fs::path &from, ProjectNode &into) {
				std::error_code error;
				std::vector<fs::directory_entry> entries;
				for (const auto &entry : fs::directory_iterator(from, fs::directory_options::skip_permission_denied, error)) entries.push_back(entry);
				std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) { return a.path() < b.path(); });
				for (const auto &entry : entries) {
					if (entry.path().filename().string().rfind('.', 0) == 0) continue;
					ProjectNode added;
					added.name = entry.path().filename().string();
					if (entry.is_directory(error)) {
						added.kind = ProjectNode::Kind::Folder;
						addAll(entry.path(), added);
					} else {
						added.kind = ProjectNode::Kind::File;
						added.path = entry.path().string();
					}
					into.children.push_back(std::move(added));
				}
			};
			addAll(directory, *node);
			node->expanded = true;
		}
	} else if (action == "nodeRename") {
		std::string name;
		if (ask("Rename", node->name, name)) node->name = name;
	} else if (action == "nodeEditPath") {
		std::string path;
		if (host.promptText("Modify File Path", "Path:", node->path, path) && !path.empty()) {
			node->path = expandPath(path);
			node->name = baseName(node->path);
		}
	} else if (action == "nodeRemove" || action == "nodeUp" || action == "nodeDown") {
		ProjectNode *parent = projectParentOf(panel, node);
		if (action == "nodeRemove" && node->kind != ProjectNode::Kind::File &&
		    host.ask("Remove “" + node->name + "”?", "Everything inside it leaves the workspace too. The files stay on disk.", {"Cancel", "Remove"}) != 1) return;
		if (parent) {
			const auto at = std::find_if(parent->children.begin(), parent->children.end(), [&](const ProjectNode &n) { return &n == node; });
			const long index = at - parent->children.begin();
			if (action == "nodeRemove") parent->children.erase(at);
			else if (action == "nodeUp" && index > 0) std::swap(parent->children[index], parent->children[index - 1]);
			else if (action == "nodeDown" && index + 1 < static_cast<long>(parent->children.size())) std::swap(parent->children[index], parent->children[index + 1]);
		}
	}
	const bool persisted = action == "wsNew" || action == "wsOpen" || action == "wsReload" || action == "wsSave" || action == "wsSaveAs";
	if (persisted) workspaceDirty[panel - kPanelProject1] = false;
	else if (action != "wsCopy" && action != "projectFind") workspaceDirty[panel - kPanelProject1] = true;
	host.panelChanged(panel);
}

bool Workspace::offerSavingWorkspaces() {
	for (int i = 0; i < 3; i++) {
		if (!workspaceDirty[i]) continue;
		ProjectPanel &project = projectPanels[i];
		const int panel = kPanelProject1 + i;
		const int choice = host.ask(std::string("Save the changes to ") + panelTitle(panel) + "?", project.file.empty() ? "It hasn't been saved yet." : project.file, {"Cancel", "Don't Save", "Save"});
		if (choice <= 0) return false;
		if (choice == 2) {
			if (!project.file.empty()) {
				if (!saveWorkspaceFile(panel, project.file, true)) return false;
			} else {
				std::string path;
				if (!host.chooseSavePath("workspace" + settings.pref("workspaceExtension"), dialogDirectory(), path) || !saveWorkspaceFile(panel, path, true)) return false;
			}
		}
		workspaceDirty[i] = false;
	}
	return true;
}

#pragma mark - Incremental search

std::string Workspace::incrementalSearch(const std::string &text, bool backward, bool next, bool matchCase, bool highlightAll) {
	SciView &v = view();
	const long length = v.send(SCI_GETLENGTH);
	v.send(SCI_SETINDICATORCURRENT, kIndicatorIncremental);
	v.send(SCI_INDICATORCLEARRANGE, 0, length);
	if (text.empty()) return std::string();
	const int flags = matchCase ? SCFIND_MATCHCASE : 0;
	v.send(SCI_SETSEARCHFLAGS, flags);
	const auto search = [&](long from, long to) {
		v.send(SCI_SETTARGETRANGE, from, to);
		return v.send(SCI_SEARCHINTARGET, text.size(), reinterpret_cast<intptr_t>(text.c_str()));
	};
	long found;
	if (backward) {
		const long start = v.send(SCI_GETSELECTIONSTART);
		found = search(start, 0);
		if (found < 0) found = search(length, start);
	} else {
		const long start = next ? v.send(SCI_GETSELECTIONEND) : v.send(SCI_GETSELECTIONSTART);
		found = search(start, length);
		if (found < 0) found = search(0, start);
	}
	long total = 0;
	std::string status;
	if (found >= 0) {
		const long matchStart = v.send(SCI_GETTARGETSTART), matchEnd = v.send(SCI_GETTARGETEND);
		v.send(SCI_SETSEL, matchStart, matchEnd);
		v.send(SCI_SCROLLCARET);
		long at = 0;
		while (at < length) {
			v.send(SCI_SETTARGETRANGE, at, length);
			const long hit = v.send(SCI_SEARCHINTARGET, text.size(), reinterpret_cast<intptr_t>(text.c_str()));
			if (hit < 0) break;
			const long end = v.send(SCI_GETTARGETEND);
			if (highlightAll) {
				v.send(SCI_SETINDICATORCURRENT, kIndicatorIncremental);
				v.send(SCI_INDICATORFILLRANGE, hit, end - hit);
			}
			total++;
			at = end > hit ? end : hit + 1;
		}
		status = std::to_string(total) + (total == 1 ? " match" : " matches");
	} else {
		status = "Phrase not found";
	}
	return status;
}

void Workspace::closeIncrementalSearch() {
	SciView &v = view();
	v.send(SCI_SETINDICATORCURRENT, kIndicatorIncremental);
	v.send(SCI_INDICATORCLEARRANGE, 0, v.send(SCI_GETLENGTH));
	host.setIncrementalBar(false);
	host.focusPane(focus);
}

#pragma mark - Window modes and toolbar

void Workspace::applyChromeState() {
	ChromeState chrome;
	const bool bare = postIt || distractionFree;
	chrome.menu = !bare && !settings.prefOn("hideMenu");
	chrome.toolbar = !bare && !settings.prefOn("hideToolbar");
	chrome.tabs = !bare && !tabsHiddenByCommandLine && !settings.tabHide && !settings.prefOn("tabHide");
	chrome.status = !bare && !settings.prefOn("hideStatus");
	chrome.panels = !bare;
	chrome.frameless = postIt;
	chrome.largeToolbar = settings.pref("toolbarSize") == "large";
	chrome.appearance = settings.pref("appearance");
	chrome.filledToolbar = settings.prefOn("toolbarFilled");
	chrome.borderWidth = static_cast<int>(settings.prefInt("borderWidth"));
	{
		// The same seven colours Notepad++ offers, plus the person's own.
		static const std::map<std::string, std::string> colours = {{"red", "E81123"}, {"green", "107C10"}, {"blue", "0078D4"}, {"purple", "881798"}, {"cyan", "00B7C3"}, {"olive", "808000"}, {"yellow", "CCA700"}};
		const std::string choice = settings.pref("toolbarColour");
		if (choice == "custom") chrome.toolbarColour = settings.pref("toolbarCustomColour");
		else if (colours.count(choice)) chrome.toolbarColour = colours.at(choice);
	}
	host.setChrome(chrome);

	TabStyle tabs;
	tabs.vertical = settings.prefOn("tabVertical");
	tabs.multiLine = settings.prefOn("tabMultiLine");
	tabs.locked = settings.prefOn("tabLock");
	tabs.reduced = settings.prefOn("tabReduce");
	tabs.inactiveColour = settings.prefOn("tabInactiveColour");
	tabs.activeBar = settings.prefOn("tabActiveBar");
	tabs.closeButton = settings.prefOn("tabCloseButton");
	tabs.pinButton = settings.prefOn("tabPinEnabled") && settings.prefOn("tabPinButton");
	tabs.buttonsOnInactive = settings.prefOn("tabButtonsInactive");
	tabs.alternateIcons = settings.prefOn("tabAlternateIcons");
	tabs.rightShortcuts = !settings.prefOn("hideRightShortcuts");
	host.setTabStyle(tabs);
	if (!launchOptions().systemTray) host.setTray(settings.pref("systemTray") == "always");
}

void Workspace::togglePostIt() {
	postIt = !postIt;
	applyChromeState();
	for (int pn = 0; pn < 2; pn++) applyExtraViewPreferences(pn);
}

void Workspace::toggleDistractionFree() {
	distractionFree = !distractionFree;
	if (distractionFree) fullScreenBeforeDistraction = fullScreen;
	const bool wanted = distractionFree || fullScreenBeforeDistraction;
	if (fullScreen != wanted) {
		fullScreen = wanted;
		host.setFullScreen(wanted);
	}
	applyChromeState();
	for (int pn = 0; pn < 2; pn++) applyExtraViewPreferences(pn);
}

void Workspace::tabDoubleClicked(int pane, int index) {
	if (settings.prefOn("tabDoubleClickClose")) closeTab(pane, index);
}

std::vector<ToolItem> Workspace::toolbarItems() const {
	const auto item = [](const char *title, const char *icon, const char *symbol, const char *iconName, Cmd cmd, int arg = 0) {
		ToolItem out;
		out.title = title;
		out.icon = icon;
		out.symbol = symbol;
		out.iconName = iconName;
		out.cmd = cmd;
		out.arg = arg;
		return out;
	};
	const auto gap = []() {
		ToolItem out;
		out.separator = true;
		return out;
	};
	// In the order of Notepad++'s toolbar, so toolbarButtonsConf.xml's index numbers mean the same thing.
	return configuredToolbar({
		item("New", "document-new", "doc.badge.plus", "new", Cmd::New),
		item("Open", "document-open", "folder", "open", Cmd::Open),
		item("Save", "document-save", "square.and.arrow.down", "save", Cmd::Save),
		item("Save All", "document-save-as", "square.and.arrow.down.on.square", "save-all", Cmd::SaveAll),
		item("Close", "window-close", "xmark", "close", Cmd::Close),
		item("Close All", "edit-clear-all", "xmark.square", "close-all", Cmd::CloseAll),
		item("Print", "document-print", "printer", "print", Cmd::Print),
		gap(),
		item("Cut", "edit-cut", "scissors", "cut", Cmd::Cut),
		item("Copy", "edit-copy", "doc.on.doc", "copy", Cmd::Copy),
		item("Paste", "edit-paste", "doc.on.clipboard", "paste", Cmd::Paste),
		gap(),
		item("Undo", "edit-undo", "arrow.uturn.backward", "undo", Cmd::Undo),
		item("Redo", "edit-redo", "arrow.uturn.forward", "redo", Cmd::Redo),
		gap(),
		item("Find", "edit-find", "magnifyingglass", "find", Cmd::ShowFind),
		item("Replace", "edit-find-replace", "arrow.left.arrow.right", "replace", Cmd::ShowReplace),
		gap(),
		item("Zoom In", "zoom-in", "plus.magnifyingglass", "zoom-in", Cmd::ZoomIn),
		item("Zoom Out", "zoom-out", "minus.magnifyingglass", "zoom-out", Cmd::ZoomOut),
		gap(),
		item("Synchronize Vertical Scrolling", "object-flip-vertical", "arrow.up.and.down", "sync-vertical", Cmd::SyncVertical),
		item("Synchronize Horizontal Scrolling", "object-flip-horizontal", "arrow.left.and.right", "sync-horizontal", Cmd::SyncHorizontal),
		gap(),
		item("Word Wrap", "format-justify-left", "text.justify.left", "word-wrap", Cmd::ToggleWrap),
		item("Show All Characters", "format-text-bold", "paragraphsign", "all-chars", Cmd::ToggleAllCharacters),
		item("Show Indent Guide", "format-indent-more", "text.alignleft", "indent-guide", Cmd::ToggleGuides),
		gap(),
		item("Define Your Language", "preferences-desktop-font", "textformat", "udl-dlg", Cmd::DefineLanguage),
		item("Document Map", "view-paged", "map", "doc-map", Cmd::ToggleDocumentMap),
		item("Document List", "view-list", "list.bullet", "doc-list", Cmd::TogglePanel, kPanelDocuments),
		item("Function List", "view-list-tree", "function", "function-list", Cmd::ToggleFunctionList),
		item("Folder as Workspace", "folder", "folder.badge.gearshape", "folder-as-workspace", Cmd::ToggleFolderPanel),
		gap(),
		item("Monitoring (tail -f)", "utilities-log-viewer", "eye", "monitoring", Cmd::ToggleMonitoring),
		gap(),
		item("Start Recording", "media-record", "record.circle", "record", Cmd::MacroStart),
		item("Stop Recording", "media-playback-stop", "stop.circle", "stop-record", Cmd::MacroStop),
		item("Playback", "media-playback-start", "play.circle", "playback", Cmd::MacroPlay),
		item("Run a Macro Multiple Times", "media-seek-forward", "forward.circle", "playback-multiple", Cmd::MacroRunMultiple),
		item("Save Current Recorded Macro", "media-floppy", "square.and.arrow.down.fill", "save-macro", Cmd::MacroSave),
	});
}

std::vector<ToolItem> Workspace::configuredToolbar(std::vector<ToolItem> items) const {
	const fs::path base = fs::path(settingsDir);
	const std::string xml = readFileText((base / "toolbarButtonsConf.xml").string());
	pugi::xml_document document;
	if (xml.empty() || !document.load_buffer(xml.data(), xml.size())) return items;
	const pugi::xml_node root = document.child("NotepadPlus");
	const pugi::xml_node standard = root.child("ToolbarButtons").child("Standard");
	const bool hideAll = std::strcmp(standard.attribute("hideAll").as_string(), "yes") == 0;
	std::set<size_t> hidden;
	for (const pugi::xml_node button : standard.children("Button"))
		if (std::strcmp(button.attribute("hide").as_string(), "yes") == 0) hidden.insert(button.attribute("index").as_uint());
	const std::string folder = root.child("ToolBarIcons").attribute("icoFolderName").as_string();
	const fs::path icons = base / "toolbarIcons" / (folder.empty() ? "default" : fs::path(folder).filename());
	std::vector<ToolItem> out;
	for (size_t i = 0; i < items.size(); i++) {
		if (hideAll || hidden.count(i)) continue;
		ToolItem item = items[i];
		std::error_code error;
		for (const char *extension : {".ico", ".png", ".svg"}) {
			const fs::path file = icons / (item.iconName + extension);
			if (!item.iconName.empty() && item.customIcon.empty() && fs::exists(file, error)) item.customIcon = file.string();
		}
		out.push_back(std::move(item));
	}
	while (!out.empty() && out.back().separator) out.pop_back();
	return out;
}

#pragma mark - The editor's context menu

std::string Workspace::contextMenuPath() const { return (fs::path(settingsDir) / "contextMenu.xml").string(); }

namespace {

const MenuItem *findMenuItem(const std::vector<MenuItem> &items, const std::string &name) {
	for (const MenuItem &item : items) {
		if (item.separator) continue;
		if (item.title == name && item.children.empty()) return &item;
		if (const MenuItem *found = findMenuItem(item.children, name)) return found;
	}
	return nullptr;
}

const char *const kDefaultContextMenu = R"(<?xml version="1.0" encoding="UTF-8" ?>
<!-- Each Item names a menu and a command in it. FolderName puts it in a submenu, and id="0" is a separator. -->
<NotepadPlus>
    <ScintillaContextMenu>
        <Item MenuEntryName="Edit" MenuItemName="Cut"/>
        <Item MenuEntryName="Edit" MenuItemName="Copy"/>
        <Item MenuEntryName="Edit" MenuItemName="Paste"/>
        <Item MenuEntryName="Edit" MenuItemName="Delete"/>
        <Item MenuEntryName="Edit" MenuItemName="Select All"/>
        <Item MenuEntryName="Edit" MenuItemName="Begin/End Select"/>
        <Item MenuEntryName="Edit" MenuItemName="Begin/End Select in Column Mode"/>
        <Item id="0"/>
        <Item MenuEntryName="Edit" MenuItemName="UPPERCASE" FolderName="Convert Case"/>
        <Item MenuEntryName="Edit" MenuItemName="lowercase" FolderName="Convert Case"/>
        <Item MenuEntryName="Edit" MenuItemName="Toggle Single Line Comment"/>
        <Item MenuEntryName="Edit" MenuItemName="Block Comment"/>
        <Item MenuEntryName="Edit" MenuItemName="Block Uncomment"/>
        <Item id="0"/>
        <Item MenuEntryName="Edit" MenuItemName="Open File"/>
        <Item MenuEntryName="Edit" MenuItemName="Open Containing Folder"/>
        <Item MenuEntryName="Edit" MenuItemName="Search on Internet"/>
        <Item id="0"/>
        <Item MenuEntryName="Search" MenuItemName="Toggle Bookmark"/>
        <Item MenuEntryName="View" MenuItemName="Hide Lines"/>
    </ScintillaContextMenu>
</NotepadPlus>
)";

}  // namespace

std::vector<MenuItem> Workspace::menuFromXml(const std::string &raw, const char *section, const char *fallback, int pane, int index) const {
	std::vector<MenuItem> out;
	std::string xml = raw;
	pugi::xml_document document;
	if (xml.empty() || !document.load_buffer(xml.data(), xml.size())) {
		if (!fallback) return out;
		xml = fallback;
		document.load_buffer(xml.data(), xml.size());
	}
	const std::vector<MenuItem> menu = mainMenu();
	std::map<std::string, size_t> folders;
	const auto place = [&](MenuItem copy, const std::string &folder) {
		if (folder.empty()) {
			out.push_back(std::move(copy));
			return;
		}
		auto known = folders.find(folder);
		if (known == folders.end()) {
			MenuItem parent;
			parent.title = folder;
			out.push_back(parent);
			known = folders.emplace(folder, out.size() - 1).first;
		}
		out[known->second].children.push_back(std::move(copy));
	};
	for (const pugi::xml_node item : document.child("NotepadPlus").child(section).children("Item")) {
		const std::string id = item.attribute("id").as_string();
		const std::string folder = item.attribute("FolderName").as_string();
		if (id == "0") {
			MenuItem separator;
			separator.separator = true;
			out.push_back(separator);
			continue;
		}
		if (id == "44048") {
			// Pin Tab has to be by id, as its name changes.
			if (pane < 0 || !settings.prefOn("tabPinEnabled")) continue;
			const auto &list = paneAt(pane).tabs;
			MenuItem pin;
			pin.title = index >= 0 && index < static_cast<int>(list.size()) && list[static_cast<size_t>(index)].pinned ? "Unpin Tab" : "Pin Tab";
			pin.cmd = Cmd::TabPin;
			pin.arg = pane * 10000 + index;
			place(pin, folder);
			continue;
		}
		const std::string entry = item.attribute("MenuEntryName").as_string();
		std::string name = item.attribute("MenuItemName").as_string();
		// Notepad++ writes three dots and names Windows programs. These are their equivalents here.
		const size_t dots = name.find("...");
		if (dots != std::string::npos) name.replace(dots, 3, "…");
		static const std::map<std::string, std::string> equivalents = {{"cmd", "Terminal"}, {"PowerShell", "Terminal"}, {"Explorer", "Reveal in Finder"}};
		if (equivalents.count(name)) {
#ifdef __APPLE__
			name = equivalents.at(name);
#else
			name = name == "Explorer" ? "Show in File Manager" : equivalents.at(name);
#endif
		}
		const MenuItem *found = nullptr;
		for (const MenuItem &top : menu)
			if (top.title == entry && !found) found = findMenuItem(top.children, name);
		if (!found) continue;
		MenuItem copy = *found;
		const std::string alias = item.attribute("ItemNameAs").as_string();
		if (!alias.empty()) copy.title = alias;
		place(copy, folder);
	}
	return out;
}

std::vector<MenuItem> Workspace::editorContextMenu() const {
	return menuFromXml(readFileText(contextMenuPath()), "ScintillaContextMenu", kDefaultContextMenu, -1, -1);
}

void Workspace::editContextMenu() {
	const std::string path = contextMenuPath();
	std::error_code error;
	if (!fs::exists(path, error)) {
		std::string writeError;
		atomicWrite(kDefaultContextMenu, path, 0644, writeError);
	}
	openPath(path);
}

void Workspace::editorRightClicked(int pane, long position) {
	focus = pane;
	SciView &v = view(pane);
	const long start = v.send(SCI_GETSELECTIONSTART), end = v.send(SCI_GETSELECTIONEND);
	const bool inside = position >= start && position <= end;
	if (!inside && !settings.prefOn("rightClickKeepsSelection")) v.send(SCI_GOTOPOS, position);
}

#pragma mark - Style Configurator

void Workspace::styleConfigurator() {
	std::vector<FormField> pick;
	FormField theme = simpleField(FormField::Kind::Choice, "theme", "Select theme", settings.theme == "Auto" ? effectiveTheme() : settings.theme);
	theme.choices.emplace_back("Default", "Default");
	for (const std::string &name : catalog.themeNames()) theme.choices.emplace_back(name, name);
	if (theme.value.empty()) theme.value = "Default";
	pick.push_back(theme);
	FormField language = simpleField(FormField::Kind::Choice, "language", "Language", "global");
	language.choices.emplace_back("global", "Global Styles");
	for (const Language *candidate : catalog.languages())
		if (candidate != catalog.plainText()) language.choices.emplace_back(candidate->name, candidate->displayName);
	const Document *document = activeDocument();
	if (document && !document->language->user && document->language != catalog.plainText()) language.value = document->language->name;
	pick.push_back(language);
	pick.push_back(simpleField(FormField::Kind::Bool, "apply", "Use this theme", "1"));
	if (!host.runForm("Style Configurator", pick)) return;

	const std::string themeName = valueOf(pick, "theme");
	const std::string lexer = valueOf(pick, "language");
	std::vector<ThemeStyle> styles = catalog.editableStyles(themeName, lexer);
	std::vector<FormField> fields;
	for (size_t i = 0; i < styles.size(); i++) {
		const ThemeStyle &style = styles[i];
		const std::string value = style.fg + "," + style.bg + "," + std::to_string(style.fontStyle < 0 ? 0 : style.fontStyle) + "," + style.fontName + "," + (style.fontSize > 0 ? std::to_string(style.fontSize) : std::string());
		fields.push_back(simpleField(FormField::Kind::StyleRow, "style" + std::to_string(i), style.name, value));
	}
	if (lexer == "global") {
		fields.push_back(simpleField(FormField::Kind::Heading, "", "Global override"));
		for (const auto &option : std::vector<std::pair<const char *, const char *>>{
			{"overrideFg", "Enable global foreground colour"}, {"overrideBg", "Enable global background colour"},
			{"overrideFont", "Enable global font"}, {"overrideSize", "Enable global font size"}, {"overrideBold", "Enable global bold"},
			{"overrideItalic", "Enable global italic"}, {"overrideUnderline", "Enable global underline"}}) {
			fields.push_back(simpleField(FormField::Kind::Bool, option.first, option.second, settings.prefs.count(option.first) && settings.prefs.at(option.first) == "1" ? "1" : "0"));
		}
		fields.push_back(simpleField(FormField::Kind::Bool, "goToSettings", "Go to Preferences after this", "0"));
	} else {
		if (const Language *shown = catalog.languageNamed(lexer)) {
			std::string extensions;
			for (const std::string &extension : shown->extensions) extensions += extension + " ";
			fields.push_back(simpleField(FormField::Kind::Heading, "", "Default ext.: " + (extensions.empty() ? std::string("none") : extensions)));
			for (const auto &group : shown->keywords) {
				std::string words = group.second;
				if (words.size() > 400) words = words.substr(0, 400) + "…";
				if (!words.empty()) fields.push_back(simpleField(FormField::Kind::Heading, "", "Default keywords (" + group.first + "): " + words));
			}
		}
		fields.push_back(simpleField(FormField::Kind::Text, "userExtensions", "User extensions (like: foo bar)", settings.prefs.count("ext." + lexer) ? settings.prefs.at("ext." + lexer) : std::string()));
		fields.push_back(simpleField(FormField::Kind::LongText, "userKeywords", "User-defined keywords", settings.prefs.count("keywords." + lexer) ? settings.prefs.at("keywords." + lexer) : std::string()));
	}
	if (!host.runForm("Style Configurator: " + themeName, fields)) return;

	for (size_t i = 0; i < styles.size(); i++) {
		const std::string value = valueOf(fields, "style" + std::to_string(i));
		const size_t first = value.find(','), second = first == std::string::npos ? first : value.find(',', first + 1);
		if (second == std::string::npos) continue;
		styles[i].fg = value.substr(0, first);
		styles[i].bg = value.substr(first + 1, second - first - 1);
		styles[i].fontStyle = std::atoi(value.c_str() + second + 1);
		const size_t third = value.find(',', second + 1), fourth = third == std::string::npos ? third : value.find(',', third + 1);
		if (fourth != std::string::npos) {
			styles[i].fontName = value.substr(third + 1, fourth - third - 1);
			styles[i].fontSize = std::atoi(value.c_str() + fourth + 1);
		}
	}
	std::string error;
	if (!catalog.saveStyles(themeName, lexer, styles, error)) {
		host.ask("The theme could not be saved.", error, {"OK"});
		return;
	}
	if (lexer == "global") {
		for (const char *key : {"overrideFg", "overrideBg", "overrideFont", "overrideSize", "overrideBold", "overrideItalic", "overrideUnderline"}) settings.prefs[key] = valueOf(fields, key);
		applyGlobalOverride();
	} else {
		settings.prefs["ext." + lexer] = valueOf(fields, "userExtensions");
		settings.prefs["keywords." + lexer] = valueOf(fields, "userKeywords");
		applyLanguageCustomisation();
	}
	if (valueOf(pick, "apply") == "1") settings.theme = themeName;
	saveSettings();
	applyThemeEverywhere();
	host.menusChanged();
	if (lexer == "global" && valueOf(fields, "goToSettings") == "1") run(Cmd::ShowPreferences);
}

void Workspace::applyGlobalOverride() {
	GlobalOverride override_;
	override_.fg = settings.prefs.count("overrideFg") && settings.prefs["overrideFg"] == "1";
	override_.bg = settings.prefs.count("overrideBg") && settings.prefs["overrideBg"] == "1";
	override_.font = settings.prefs.count("overrideFont") && settings.prefs["overrideFont"] == "1";
	override_.size = settings.prefs.count("overrideSize") && settings.prefs["overrideSize"] == "1";
	override_.bold = settings.prefs.count("overrideBold") && settings.prefs["overrideBold"] == "1";
	override_.italic = settings.prefs.count("overrideItalic") && settings.prefs["overrideItalic"] == "1";
	override_.underline = settings.prefs.count("overrideUnderline") && settings.prefs["overrideUnderline"] == "1";
	catalog.setGlobalOverride(override_);
}

void Workspace::applyLanguageCustomisation() {
	std::map<std::string, std::string> extensions, keywords;
	for (const auto &entry : settings.prefs) {
		if (entry.first.rfind("ext.", 0) == 0) extensions[entry.first.substr(4)] = entry.second;
		else if (entry.first.rfind("keywords.", 0) == 0) keywords[entry.first.substr(9)] = entry.second;
	}
	catalog.setLanguageCustomisation(extensions, keywords);
}

void Workspace::importTheme() {
	std::vector<std::string> paths;
	if (!host.chooseFilesToOpen(std::string(), paths)) return;
	int imported = 0;
	for (const std::string &path : paths) {
		std::string error;
		if (catalog.importTheme(path, error)) imported++;
		else host.ask("“" + baseName(path) + "” could not be imported.", error, {"OK"});
	}
	if (imported > 0) {
		host.menusChanged();
		host.flash("Imported " + std::to_string(imported) + (imported == 1 ? " theme" : " themes"));
	}
}

#pragma mark - Plugins

std::string Workspace::pluginFolder() const { return (fs::path(settingsDir) / "plugins").string(); }

namespace {

Workspace *gPluginWorkspace = nullptr;
Host *gPluginHost = nullptr;

intptr_t pluginSend(unsigned message, uintptr_t w, intptr_t l) {
	return gPluginWorkspace ? gPluginWorkspace->sendToActiveEditor(message, w, l) : 0;
}
int pluginOpen(const char *path) { return gPluginWorkspace && path ? gPluginWorkspace->openPath(path) : 0; }
void pluginMessage(const char *text) {
	if (gPluginHost && text) gPluginHost->flash(text);
}
size_t pluginActivePath(char *out, size_t capacity) {
	const std::string path = gPluginWorkspace ? gPluginWorkspace->activePath() : std::string();
	if (out && capacity > 0) {
		const size_t count = std::min(path.size(), capacity - 1);
		std::memcpy(out, path.data(), count);
		out[count] = 0;
	}
	return path.size();
}

}  // namespace

intptr_t Workspace::sendToActiveEditor(unsigned message, uintptr_t w, intptr_t l) { return s(message, w, l); }

void Workspace::loadPlugins() {
	gPluginWorkspace = this;
	gPluginHost = &host;
	if (launchOptions().noPlugin) return;
	std::error_code error;
	std::set<std::string> disabled;
	std::istringstream words(settings.pref("disabledPlugins"));
	std::string word;
	while (words >> word) disabled.insert(word);
	std::vector<fs::path> folders;
	for (const auto &entry : fs::directory_iterator(pluginFolder(), error))
		if (entry.is_directory(error)) folders.push_back(entry.path());
	std::sort(folders.begin(), folders.end());
#ifdef __APPLE__
	const char *suffix = ".dylib";
#else
	const char *suffix = ".so";
#endif
	static const OmapadHostApi api = {OMAPAD_PLUGIN_API_VERSION, pluginSend, pluginOpen, pluginMessage, pluginActivePath};
	for (const fs::path &folder : folders) {
		LoadedPlugin plugin;
		plugin.folderName = folder.filename().string();
		plugin.path = (folder / (plugin.folderName + suffix)).string();
		plugin.name = plugin.folderName;
		plugin.enabled = !disabled.count(plugin.folderName);
		if (!plugin.enabled) {
			plugins.push_back(std::move(plugin));
			continue;
		}
		void *handle = dlopen(plugin.path.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (!handle) {
			const char *why = dlerror();
			plugin.error = why ? why : "It could not be loaded.";
			plugins.push_back(std::move(plugin));
			continue;
		}
		const auto init = reinterpret_cast<OmapadPluginInit>(dlsym(handle, "omapadPluginInit"));
		const OmapadPluginInfo *info = init ? init(&api) : nullptr;
		if (!info || info->apiVersion != OMAPAD_PLUGIN_API_VERSION) {
			plugin.error = "It isn't built for this version of omapad.";
			dlclose(handle);
			plugins.push_back(std::move(plugin));
			continue;
		}
		plugin.handle = handle;
		plugin.info = info;
		if (info->name) plugin.name = info->name;
		plugin.description = info->description ? info->description : "";
		plugin.notify = reinterpret_cast<OmapadPluginNotify>(dlsym(handle, "omapadPluginNotify"));
		plugins.push_back(std::move(plugin));
	}
}

void Workspace::pluginsAdmin() {
	std::string search;
	for (;;) {
		ListDialog dialog;
		dialog.title = "Plugins Admin";
		dialog.columns = {"Plugin", "Status", "Description"};
		dialog.buttons = {"Enable or Disable", "Remove", "Install…", "Done", "Search…"};
		std::vector<size_t> shown;
		for (size_t i = 0; i < plugins.size(); i++) {
			const LoadedPlugin &plugin = plugins[i];
			if (!search.empty() && lower(plugin.name + " " + plugin.description).find(lower(search)) == std::string::npos) continue;
			shown.push_back(i);
			dialog.rows.push_back({plugin.name, !plugin.enabled ? "Disabled" : plugin.error.empty() ? "Loaded" : "Failed: " + plugin.error, plugin.description});
		}
		std::vector<int> selected;
		const int button = host.runList(dialog, selected);
		if (button < 0 || button == 3) return;
		if (button == 4) {
			host.promptText("Plugins Admin", "Search:", search, search);
			continue;
		}
		if (button == 2) {
			importPlugin();
			continue;
		}
		if (!selected.empty()) selected[0] = static_cast<int>(shown[static_cast<size_t>(selected[0])]);
		if (selected.empty()) continue;
		const LoadedPlugin &plugin = plugins[static_cast<size_t>(selected[0])];
		if (button == 0) {
			std::set<std::string> disabled;
			std::istringstream words(settings.pref("disabledPlugins"));
			std::string word;
			while (words >> word) disabled.insert(word);
			if (disabled.count(plugin.folderName)) disabled.erase(plugin.folderName);
			else disabled.insert(plugin.folderName);
			std::string joined;
			for (const std::string &name : disabled) joined += name + " ";
			settings.prefs["disabledPlugins"] = joined;
			saveSettings();
			host.ask("Restart omapad to apply this.", "", {"OK"});
			return;
		}
		if (host.ask("Remove “" + plugin.name + "”?", "Its folder is deleted. This takes effect after a restart.", {"Cancel", "Remove"}) == 1) {
			std::error_code error;
			fs::remove_all(fs::path(pluginFolder()) / plugin.folderName, error);
			if (error) host.ask("The plugin could not be removed.", error.message(), {"OK"});
			return;
		}
	}
}

void Workspace::importPlugin() {
	std::vector<std::string> paths;
	if (!host.chooseFilesToOpen(std::string(), paths) || paths.empty()) return;
	for (const std::string &path : paths) {
		const fs::path source(path);
		const std::string extension = source.extension().string();
		if (extension == ".dll") {
			host.ask("That's a Windows plugin.", "Notepad++ plugins are Windows DLLs and can't run on this system. omapad plugins are native libraries that use OmapadPlugin.h.", {"OK"});
			continue;
		}
		if (extension != ".so" && extension != ".dylib") {
			host.ask("“" + source.filename().string() + "” isn't a plugin.", "A plugin is a .dylib on a Mac or a .so on Linux.", {"OK"});
			continue;
		}
		if (host.ask("Install “" + source.stem().string() + "”?", "A plugin runs with all of your permissions, so only install ones you trust.", {"Cancel", "Install"}) != 1) continue;
		const fs::path folder = fs::path(pluginFolder()) / source.stem();
		std::error_code error;
		fs::create_directories(folder, error);
		fs::copy_file(source, folder / source.filename(), fs::copy_options::overwrite_existing, error);
		if (error) host.ask("The plugin could not be installed.", error.message(), {"OK"});
		else host.ask("Restart omapad to load it.", "", {"OK"});
	}
}

#pragma mark - The tools Notepad++ ships as plugins

namespace {

const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::string &data, bool padding, size_t lineLength, const char *eol) {
	std::string out;
	size_t column = 0;
	const auto emit = [&](char c) {
		out += c;
		if (lineLength && ++column == lineLength) {
			out += eol;
			column = 0;
		}
	};
	for (size_t i = 0; i < data.size(); i += 3) {
		const unsigned a = static_cast<unsigned char>(data[i]);
		const unsigned b = i + 1 < data.size() ? static_cast<unsigned char>(data[i + 1]) : 0;
		const unsigned c = i + 2 < data.size() ? static_cast<unsigned char>(data[i + 2]) : 0;
		emit(kBase64[a >> 2]);
		emit(kBase64[((a & 3) << 4) | (b >> 4)]);
		if (i + 1 < data.size()) emit(kBase64[((b & 15) << 2) | (c >> 6)]);
		else if (padding) emit('=');
		if (i + 2 < data.size()) emit(kBase64[c & 63]);
		else if (padding) emit('=');
	}
	return out;
}

bool base64Decode(const std::string &text, std::string &out, bool strict) {
	unsigned buffer = 0;
	int bits = 0;
	out.clear();
	for (const char c : text) {
		if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
			if (strict) return false;
			continue;
		}
		if (c == '=') continue;
		const char *found = std::strchr(kBase64, c);
		if (!found || !c) {
			if (strict) return false;
			continue;
		}
		buffer = (buffer << 6) | static_cast<unsigned>(found - kBase64);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out += static_cast<char>((buffer >> bits) & 0xFF);
		}
	}
	return true;
}

std::string quotedPrintableEncode(const std::string &text) {
	static const char *hex = "0123456789ABCDEF";
	std::string out;
	size_t column = 0;
	for (size_t i = 0; i < text.size(); i++) {
		const unsigned char c = static_cast<unsigned char>(text[i]);
		if (c == '\r' || c == '\n') {
			out += static_cast<char>(c);
			column = 0;
			continue;
		}
		const bool lastOnLine = i + 1 == text.size() || text[i + 1] == '\r' || text[i + 1] == '\n';
		const bool plain = (c >= 33 && c <= 126 && c != '=') || ((c == ' ' || c == '\t') && !lastOnLine);
		const size_t width = plain ? 1 : 3;
		if (column + width > 75) {
			out += "=\n";
			column = 0;
		}
		if (plain) {
			out += static_cast<char>(c);
		} else {
			out += '=';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
		column += width;
	}
	return out;
}

int hexDigit(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

std::string quotedPrintableDecode(const std::string &text) {
	std::string out;
	for (size_t i = 0; i < text.size(); i++) {
		if (text[i] != '=') {
			out += text[i];
			continue;
		}
		if (i + 1 < text.size() && text[i + 1] == '\n') {
			i++;
		} else if (i + 2 < text.size() && text[i + 1] == '\r' && text[i + 2] == '\n') {
			i += 2;
		} else if (i + 2 < text.size() && hexDigit(text[i + 1]) >= 0 && hexDigit(text[i + 2]) >= 0) {
			out += static_cast<char>(hexDigit(text[i + 1]) * 16 + hexDigit(text[i + 2]));
			i += 2;
		} else {
			out += '=';
		}
	}
	return out;
}

std::string urlEncode(const std::string &text, bool full) {
	if (full) return percentEncode(text);
	static const char *hex = "0123456789ABCDEF";
	const std::string keep = "-_.~:/?#[]@!$&'()*+,;=%";
	std::string out;
	for (const char raw : text) {
		const unsigned char c = static_cast<unsigned char>(raw);
		if (std::isalnum(c) || keep.find(raw) != std::string::npos) {
			out += raw;
		} else {
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

std::string urlDecode(const std::string &text) {
	std::string out;
	for (size_t i = 0; i < text.size(); i++) {
		if (text[i] == '%' && i + 2 < text.size() && hexDigit(text[i + 1]) >= 0 && hexDigit(text[i + 2]) >= 0) {
			out += static_cast<char>(hexDigit(text[i + 1]) * 16 + hexDigit(text[i + 2]));
			i += 2;
		} else if (text[i] == '+') {
			out += ' ';
		} else {
			out += text[i];
		}
	}
	return out;
}

// A small DEFLATE decoder, for SAML Decode. Returns false on anything malformed.
class Inflater {
public:
	explicit Inflater(const std::string &data) : in(data) {}
	bool run(std::string &out) {
		bool last = false;
		while (!last) {
			last = bits(1) == 1;
			const int type = bits(2);
			if (failed) return false;
			if (type == 0) {
				bitCount = 0;
				if (position + 4 > in.size()) return false;
				const unsigned length = static_cast<unsigned char>(in[position]) | (static_cast<unsigned char>(in[position + 1]) << 8);
				position += 4;
				if (position + length > in.size()) return false;
				out.append(in, position, length);
				position += length;
			} else if (type == 1 || type == 2) {
				if (!block(type, out)) return false;
			} else {
				return false;
			}
		}
		return !failed;
	}

private:
	struct Huffman {
		short count[16] = {};
		std::vector<short> symbol;
	};
	const std::string &in;
	size_t position = 0;
	unsigned buffer = 0;
	int bitCount = 0;
	bool failed = false;
	int bits(int need) {
		while (bitCount < need) {
			if (position >= in.size()) {
				failed = true;
				return 0;
			}
			buffer |= static_cast<unsigned>(static_cast<unsigned char>(in[position++])) << bitCount;
			bitCount += 8;
		}
		const int value = static_cast<int>(buffer & ((1u << need) - 1));
		buffer >>= need;
		bitCount -= need;
		return value;
	}
	static Huffman build(const short *lengths, int n) {
		Huffman h;
		h.symbol.resize(static_cast<size_t>(n));
		for (int i = 0; i < n; i++) h.count[lengths[i]]++;
		h.count[0] = 0;
		short offsets[16] = {};
		for (int i = 1; i < 15; i++) offsets[i + 1] = static_cast<short>(offsets[i] + h.count[i]);
		for (int i = 0; i < n; i++)
			if (lengths[i]) h.symbol[static_cast<size_t>(offsets[lengths[i]]++)] = static_cast<short>(i);
		return h;
	}
	int decode(const Huffman &h) {
		int code = 0, first = 0, index = 0;
		for (int length = 1; length <= 15; length++) {
			code |= bits(1);
			if (failed) return -1;
			const int count = h.count[length];
			if (code - count < first) return h.symbol[static_cast<size_t>(index + code - first)];
			index += count;
			first = (first + count) << 1;
			code <<= 1;
		}
		return -1;
	}
	bool block(int type, std::string &out) {
		static const short lengthBase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
		static const short lengthExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
		static const short distanceBase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
		static const short distanceExtra[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
		short lengths[320] = {};
		Huffman literals, distances;
		if (type == 1) {
			for (int i = 0; i < 288; i++) lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
			literals = build(lengths, 288);
			for (int i = 0; i < 30; i++) lengths[i] = 5;
			distances = build(lengths, 30);
		} else {
			const int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
			static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
			if (failed || nlen > 286 || ndist > 30) return false;
			for (int i = 0; i < ncode; i++) lengths[order[i]] = static_cast<short>(bits(3));
			const Huffman codes = build(lengths, 19);
			short all[320] = {};
			for (int i = 0; i < nlen + ndist;) {
				const int symbol = decode(codes);
				if (symbol < 0) return false;
				if (symbol < 16) {
					all[i++] = static_cast<short>(symbol);
				} else {
					short previous = 0;
					int repeat = 0;
					if (symbol == 16) {
						if (i == 0) return false;
						previous = all[i - 1];
						repeat = 3 + bits(2);
					} else if (symbol == 17) {
						repeat = 3 + bits(3);
					} else {
						repeat = 11 + bits(7);
					}
					if (i + repeat > nlen + ndist) return false;
					while (repeat--) all[i++] = previous;
				}
			}
			literals = build(all, nlen);
			distances = build(all + nlen, ndist);
		}
		for (;;) {
			int symbol = decode(literals);
			if (symbol < 0) return false;
			if (symbol < 256) {
				out += static_cast<char>(symbol);
			} else if (symbol == 256) {
				return true;
			} else {
				symbol -= 257;
				if (symbol >= 29) return false;
				const int length = lengthBase[symbol] + bits(lengthExtra[symbol]);
				const int code = decode(distances);
				if (code < 0 || code >= 30) return false;
				const size_t distance = static_cast<size_t>(distanceBase[code] + bits(distanceExtra[code]));
				if (distance > out.size()) return false;
				for (int i = 0; i < length; i++) out += out[out.size() - distance];
			}
		}
	}
};

}  // namespace

std::string applyTextTool(const std::string &tool, const std::string &input, bool &ok) {
	ok = true;
	std::string out;
	if (tool.size() > 6 && tool.compare(tool.size() - 6, 6, "ByLine") == 0) {
		// Each line on its own, keeping the line endings as they are.
		const std::string inner = tool.substr(0, tool.size() - 6);
		std::string out, line;
		for (size_t i = 0; i <= input.size(); i++) {
			if (i == input.size() || input[i] == '\n' || input[i] == '\r') {
				bool lineOk = true;
				out += applyTextTool(inner, line, lineOk);
				ok = ok && lineOk;
				line.clear();
				if (i < input.size()) out += input[i];
			} else {
				line += input[i];
			}
		}
		return out;
	}
	if (tool == "base64") return base64Encode(input, false, 0, "");
	if (tool == "base64UnixEol") return base64Encode(input, false, 64, "\n");
	if (tool == "urlEncodeRfc1738") {
		static const char *hex = "0123456789ABCDEF";
		std::string out;
		for (const char raw : input) {
			const unsigned char c = static_cast<unsigned char>(raw);
			if (std::isalnum(c) || std::strchr("$-_.+!*'(),", c)) {
				out += raw;
			} else {
				out += '%';
				out += hex[c >> 4];
				out += hex[c & 15];
			}
		}
		return out;
	}
	if (tool == "base64Padded") return base64Encode(input, true, 0, "");
	if (tool == "base64Lines") return base64Encode(input, true, 64, "\r\n");
	if (tool == "base64LinesUnix") return base64Encode(input, true, 64, "\n");
	if (tool == "base64Decode") { ok = base64Decode(input, out, false); return out; }
	if (tool == "base64DecodeStrict") { ok = base64Decode(input, out, true); return out; }
	if (tool == "qpEncode") return quotedPrintableEncode(input);
	if (tool == "qpDecode") return quotedPrintableDecode(input);
	if (tool == "urlEncode") return urlEncode(input, false);
	if (tool == "urlEncodeFull") return urlEncode(input, true);
	if (tool == "urlDecode") return urlDecode(input);
	if (tool == "samlDecode") {
		std::string compressed;
		ok = base64Decode(urlDecode(input), compressed, false);
		if (!ok) return out;
		std::string inflated;
		Inflater inflater(compressed);
		if (inflater.run(inflated)) return inflated;
		return compressed;  // A POST binding isn't compressed.
	}
	if (tool == "asciiToHex") {
		static const char *hex = "0123456789ABCDEF";
		for (size_t i = 0; i < input.size(); i++) {
			if (i) out += ' ';
			out += hex[static_cast<unsigned char>(input[i]) >> 4];
			out += hex[static_cast<unsigned char>(input[i]) & 15];
		}
		return out;
	}
	if (tool == "hexToAscii") {
		int high = -1;
		for (const char c : input) {
			if (std::isspace(static_cast<unsigned char>(c))) continue;
			const int digit = hexDigit(c);
			if (digit < 0) {
				ok = false;
				return std::string();
			}
			if (high < 0) {
				high = digit;
			} else {
				out += static_cast<char>(high * 16 + digit);
				high = -1;
			}
		}
		ok = high < 0;
		return out;
	}
	ok = false;
	return out;
}

void Workspace::runTextTool(const std::string &tool) {
	const long start = s(SCI_GETSELECTIONSTART), end = s(SCI_GETSELECTIONEND);
	if (start == end) {
		host.flash("Select some text first");
		return;
	}
	bool ok = true;
	const std::string out = applyTextTool(tool, rangeOf(start, end), ok);
	if (!ok) {
		host.flash("The selection isn't valid for that");
		return;
	}
	replaceRange(start, end, out, true);
}

void Workspace::conversionPanel() {
	std::vector<FormField> fields;
	FormField base = simpleField(FormField::Kind::Choice, "base", "The value is", "dec");
	base.choices = {{"ascii", "A character"}, {"dec", "Decimal"}, {"hex", "Hexadecimal"}, {"oct", "Octal"}, {"bin", "Binary"}};
	fields.push_back(base);
	fields.push_back(simpleField(FormField::Kind::Text, "value", "Value"));
	if (!host.runForm("Conversion Panel", fields)) return;
	const std::string kind = valueOf(fields, "base"), text = valueOf(fields, "value");
	if (text.empty()) return;
	unsigned long value = 0;
	if (kind == "ascii") {
		const std::u32string codes = utf8ToCodePoints(text);
		value = codes.empty() ? 0 : codes[0];
	} else {
		char *stop = nullptr;
		value = std::strtoul(text.c_str(), &stop, kind == "hex" ? 16 : kind == "oct" ? 8 : kind == "bin" ? 2 : 10);
		if (*stop) {
			host.flash("That isn't a valid number");
			return;
		}
	}
	std::string binary = formatNumber(static_cast<long>(value), 2, 8), character;
	if (value >= 32 && value != 127 && value <= 0x10FFFF) appendUtf8(character, value);
	const std::string report = "Character: " + character + "\nDecimal: " + std::to_string(value) + "\nHexadecimal: " + formatNumber(static_cast<long>(value), 16, 2) + "\nOctal: " + formatNumber(static_cast<long>(value), 8, 3) + "\nBinary: " + binary;
	if (host.ask("Conversion Panel", report, {"Close", "Copy"}) == 1) host.setClipboard(report);
}

namespace {

std::string colourHex(long bgr) {
	char out[8];
	snprintf(out, sizeof out, "%02X%02X%02X", static_cast<int>(bgr & 0xFF), static_cast<int>((bgr >> 8) & 0xFF), static_cast<int>((bgr >> 16) & 0xFF));
	return out;
}

std::string htmlEscaped(const std::string &text) {
	std::string out;
	for (const char c : text) {
		switch (c) {
		case '&': out += "&amp;"; break;
		case '<': out += "&lt;"; break;
		case '>': out += "&gt;"; break;
		case '"': out += "&quot;"; break;
		default: out += c; break;
		}
	}
	return out;
}

}  // namespace

std::string Workspace::exportHtml() const {
	SciView &v = view();
	v.send(SCI_COLOURISE, 0, -1);
	const long length = v.send(SCI_GETLENGTH);
	const char *pointer = reinterpret_cast<const char *>(v.send(SCI_GETCHARACTERPOINTER));
	const std::string text = pointer ? std::string(pointer, static_cast<size_t>(length)) : std::string();
	std::map<int, bool> used;
	for (long i = 0; i < length; i++) used[static_cast<int>(v.send(SCI_GETSTYLEAT, i))] = true;
	std::ostringstream out;
	char fontName[128] = {};
	v.send(SCI_STYLEGETFONT, STYLE_DEFAULT, reinterpret_cast<intptr_t>(fontName));
	out << "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"UTF-8\">\n<title>" << htmlEscaped(activeTitle()) << "</title>\n<style>\n";
	out << "body { background: #" << colourHex(v.send(SCI_STYLEGETBACK, STYLE_DEFAULT)) << "; color: #" << colourHex(v.send(SCI_STYLEGETFORE, STYLE_DEFAULT)) << "; }\n";
	out << "pre { font-family: '" << fontName << "', monospace; font-size: " << v.send(SCI_STYLEGETSIZE, STYLE_DEFAULT) << "pt; }\n";
	for (const auto &style : used) {
		const int id = style.first;
		out << ".s" << id << " { color: #" << colourHex(v.send(SCI_STYLEGETFORE, id)) << "; background: #" << colourHex(v.send(SCI_STYLEGETBACK, id)) << ";";
		if (v.send(SCI_STYLEGETBOLD, id)) out << " font-weight: bold;";
		if (v.send(SCI_STYLEGETITALIC, id)) out << " font-style: italic;";
		if (v.send(SCI_STYLEGETUNDERLINE, id)) out << " text-decoration: underline;";
		out << " }\n";
	}
	out << "</style>\n</head>\n<body>\n<pre>";
	int current = -1;
	std::string run;
	for (long i = 0; i <= length; i++) {
		const int style = i < length ? static_cast<int>(v.send(SCI_GETSTYLEAT, i)) : -2;
		if (style != current) {
			if (current >= 0) out << "<span class=\"s" << current << "\">" << htmlEscaped(run) << "</span>";
			run.clear();
			current = style;
		}
		if (i < length) run += text[static_cast<size_t>(i)];
	}
	out << "</pre>\n</body>\n</html>\n";
	return out.str();
}

std::string Workspace::exportRtf() const {
	SciView &v = view();
	v.send(SCI_COLOURISE, 0, -1);
	const long length = v.send(SCI_GETLENGTH);
	const char *pointer = reinterpret_cast<const char *>(v.send(SCI_GETCHARACTERPOINTER));
	const std::string text = pointer ? std::string(pointer, static_cast<size_t>(length)) : std::string();
	std::vector<long> colours;
	const auto colourIndex = [&](long bgr) {
		const auto found = std::find(colours.begin(), colours.end(), bgr);
		if (found != colours.end()) return static_cast<int>(found - colours.begin()) + 1;
		colours.push_back(bgr);
		return static_cast<int>(colours.size());
	};
	std::ostringstream body;
	int current = -1;
	for (size_t i = 0; i < text.size();) {
		const int style = static_cast<int>(v.send(SCI_GETSTYLEAT, static_cast<long>(i)));
		if (style != current) {
			current = style;
			body << "\\cf" << colourIndex(v.send(SCI_STYLEGETFORE, style)) << "\\highlight" << colourIndex(v.send(SCI_STYLEGETBACK, style));
			body << (v.send(SCI_STYLEGETBOLD, style) ? "\\b" : "\\b0") << (v.send(SCI_STYLEGETITALIC, style) ? "\\i" : "\\i0") << " ";
		}
		const unsigned char c = static_cast<unsigned char>(text[i]);
		if (c == '\r') {
			i++;
		} else if (c == '\n') {
			body << "\\line\n";
			i++;
		} else if (c == '\\' || c == '{' || c == '}') {
			body << '\\' << static_cast<char>(c);
			i++;
		} else if (c == '\t') {
			body << "\\tab ";
			i++;
		} else if (c < 0x80) {
			body << static_cast<char>(c);
			i++;
		} else {
			size_t width = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
			width = std::min(width, text.size() - i);
			for (const char32_t cp : utf8ToCodePoints(text.substr(i, width))) {
				if (cp > 0xFFFF) {
					const unsigned long value = cp - 0x10000;
					body << "\\u" << static_cast<short>(0xD800 + (value >> 10)) << "?\\u" << static_cast<short>(0xDC00 + (value & 0x3FF)) << "?";
				} else {
					body << "\\u" << static_cast<short>(cp) << "?";
				}
			}
			i += width;
		}
	}
	char fontName[128] = {};
	v.send(SCI_STYLEGETFONT, STYLE_DEFAULT, reinterpret_cast<intptr_t>(fontName));
	std::ostringstream out;
	out << "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0\\fmodern " << fontName << ";}}\n{\\colortbl;";
	for (const long colour : colours) out << "\\red" << (colour & 0xFF) << "\\green" << ((colour >> 8) & 0xFF) << "\\blue" << ((colour >> 16) & 0xFF) << ";";
	out << "}\n\\f0\\fs" << v.send(SCI_STYLEGETSIZE, STYLE_DEFAULT) * 2 << "\n" << body.str() << "}\n";
	return out.str();
}

void Workspace::runExport(const std::string &action) {
	const fs::path title(activeTitle());
	if (action == "html" || action == "rtf") {
		std::string path, error;
		const std::string name = fs::path(title).replace_extension(action == "html" ? ".html" : ".rtf").string();
		if (!host.chooseSavePath(name, std::string(), path)) return;
		if (!atomicWrite(action == "html" ? exportHtml() : exportRtf(), path, 0644, error)) host.ask("The file could not be written.", error, {"OK"});
	} else if (action == "copyHtml") {
		host.setClipboardData("text/html", exportHtml());
	} else if (action == "copyRtf") {
		host.setClipboardData("text/rtf", exportRtf());
	} else if (action == "copyAll") {
		host.setClipboardData("text/html+rtf", exportHtml() + std::string(1, '\0') + exportRtf());
	}
}

#pragma mark - Localization

void Workspace::loadMenuLanguage() {
	menuTranslations.clear();
	const std::string file = settings.pref("uiLanguage");
	if (file.empty()) return;
	const std::string name = fs::path(file).filename().string();
	pugi::xml_document english, translated;
	if (!english.load_file((fs::path(catalogDirectory()) / "nativeLang" / "english.xml").c_str())) return;
	const fs::path chosen = fs::path(catalogDirectory()) / "nativeLang" / (fs::path(name).extension().empty() ? name + ".xml" : name);
	if (!translated.load_file(chosen.c_str())) {
		host.flash("That menu language wasn't found");
		return;
	}
	const auto strip = [](std::string text) {
		text.erase(std::remove(text.begin(), text.end(), '&'), text.end());
		for (const char *ending : {"...", "…"}) {
			const size_t length = std::strlen(ending);
			if (text.size() >= length && text.compare(text.size() - length, length, ending) == 0) text.erase(text.size() - length);
		}
		return text;
	};
	const auto collect = [&](pugi::xml_document &doc, std::map<std::string, std::string> &byId) {
		const pugi::xml_node menu = doc.child("NotepadPlus").child("Native-Langue").child("Menu").child("Main");
		for (const char *group : {"Entries", "SubEntries", "Commands"}) {
			for (const pugi::xml_node item : menu.child(group).children("Item")) {
				const std::string id = item.attribute("id") ? item.attribute("id").as_string() : item.attribute("menuId") ? item.attribute("menuId").as_string() : item.attribute("subMenuId").as_string();
				const std::string title = item.attribute("name").as_string();
				if (!id.empty() && !title.empty()) byId[id] = title;
			}
		}
	};
	std::map<std::string, std::string> source, target;
	collect(english, source);
	collect(translated, target);
	for (const auto &entry : source) {
		const auto found = target.find(entry.first);
		if (found != target.end()) menuTranslations[strip(entry.second)] = strip(found->second);
	}
}

void Workspace::translateMenu(std::vector<MenuItem> &items) const {
	if (menuTranslations.empty()) return;
	for (MenuItem &item : items) {
		std::string title = item.title;
		std::string ending;
		const std::string ellipsis = "…";
		if (title.size() > ellipsis.size() && title.compare(title.size() - ellipsis.size(), ellipsis.size(), ellipsis) == 0) {
			title.erase(title.size() - ellipsis.size());
			ending = ellipsis;
		}
		const auto found = menuTranslations.find(title);
		if (found != menuTranslations.end()) item.title = found->second + ending;
		translateMenu(item.children);
	}
}

#pragma mark - Dispatch

bool Workspace::runPanelCommand(Cmd cmd, int arg, const std::string &argument) {
	switch (cmd) {
	case Cmd::TogglePanel: togglePanel(arg); return true;
	case Cmd::FindInProjects: findInProjects(); return true;
	case Cmd::ToggleOvertype: s(SCI_EDITTOGGLEOVERTYPE); updateStatus(); return true;
	case Cmd::ResultsAction: resultsAction(argument); return true;
	case Cmd::TabAction: tabAction(arg / 10000, arg % 10000, argument); return true;
	case Cmd::PanelCommand: panelCommand(arg / 100000, arg % 100000, argument); return true;
	case Cmd::IncrementalSearch: host.setIncrementalBar(true); return true;
	case Cmd::PostIt: togglePostIt(); return true;
	case Cmd::DistractionFree: toggleDistractionFree(); return true;
	case Cmd::StyleConfigurator: styleConfigurator(); return true;
	case Cmd::ImportTheme: importTheme(); return true;
	case Cmd::EditContextMenu: editContextMenu(); return true;
	case Cmd::PluginsAdmin: pluginsAdmin(); return true;
	case Cmd::ImportPlugin: importPlugin(); return true;
	case Cmd::OpenPluginsFolder: {
		std::error_code error;
		fs::create_directories(pluginFolder(), error);
		host.reveal(pluginFolder());
		return true;
	}
	case Cmd::PluginCommand: {
		const size_t plugin = static_cast<size_t>(arg / 1000);
		const int index = arg % 1000;
		if (plugin < plugins.size() && plugins[plugin].info && index < plugins[plugin].info->commandCount && plugins[plugin].info->commands[index].run)
			plugins[plugin].info->commands[index].run();
		return true;
	}
	case Cmd::TextTool: runTextTool(argument); return true;
	case Cmd::ConversionPanel: conversionPanel(); return true;
	case Cmd::ExportDocument: runExport(argument); return true;
	default:
		return false;
	}
}

std::vector<MenuItem> Workspace::pluginMenu() const {
	const auto item = [](const std::string &title, Cmd cmd, int arg = 0, const std::string &text = std::string()) {
		MenuItem m;
		m.title = title;
		m.cmd = cmd;
		m.arg = arg;
		m.text = text;
		return m;
	};
	const auto sub = [](const std::string &title, std::vector<MenuItem> children) {
		MenuItem m;
		m.title = title;
		m.children = std::move(children);
		return m;
	};
	MenuItem separator;
	separator.separator = true;
	std::vector<MenuItem> out = {
		sub("MIME Tools", {
			item("Base64 Encode", Cmd::TextTool, 0, "base64"),
			item("Base64 Encode with Padding", Cmd::TextTool, 0, "base64Padded"),
			item("Base64 Encode with Unix EOL", Cmd::TextTool, 0, "base64UnixEol"),
			item("Base64 Encode by Line", Cmd::TextTool, 0, "base64ByLine"),
			item("Base64 Encode with Line Breaks", Cmd::TextTool, 0, "base64Lines"),
			item("Base64 Decode", Cmd::TextTool, 0, "base64Decode"),
			item("Base64 Decode Strict", Cmd::TextTool, 0, "base64DecodeStrict"),
			item("Base64 Decode by Line", Cmd::TextTool, 0, "base64DecodeByLine"),
			separator,
			item("Quoted-printable Encode", Cmd::TextTool, 0, "qpEncode"),
			item("Quoted-printable Decode", Cmd::TextTool, 0, "qpDecode"),
			separator,
			item("URL Encode (RFC1738)", Cmd::TextTool, 0, "urlEncodeRfc1738"),
			item("URL Encode (RFC1738) by Line", Cmd::TextTool, 0, "urlEncodeRfc1738ByLine"),
			item("URL Encode (Extended)", Cmd::TextTool, 0, "urlEncode"),
			item("URL Encode (Extended) by Line", Cmd::TextTool, 0, "urlEncodeByLine"),
			item("URL Encode (Full)", Cmd::TextTool, 0, "urlEncodeFull"),
			item("URL Encode (Full) by Line", Cmd::TextTool, 0, "urlEncodeFullByLine"),
			item("URL Decode", Cmd::TextTool, 0, "urlDecode"),
			separator,
			item("SAML Decode", Cmd::TextTool, 0, "samlDecode"),
		}),
		sub("Converter", {
			item("ASCII -> HEX", Cmd::TextTool, 0, "asciiToHex"),
			item("HEX -> ASCII", Cmd::TextTool, 0, "hexToAscii"),
			item("Conversion Panel…", Cmd::ConversionPanel),
		}),
		sub("NppExport", {
			item("Export to RTF…", Cmd::ExportDocument, 0, "rtf"),
			item("Export to HTML…", Cmd::ExportDocument, 0, "html"),
			separator,
			item("Copy RTF to Clipboard", Cmd::ExportDocument, 0, "copyRtf"),
			item("Copy HTML to Clipboard", Cmd::ExportDocument, 0, "copyHtml"),
			item("Copy All Formats to Clipboard", Cmd::ExportDocument, 0, "copyAll"),
		}),
	};
	for (size_t i = 0; i < plugins.size(); i++) {
		const LoadedPlugin &plugin = plugins[i];
		if (!plugin.info) continue;
		std::vector<MenuItem> commands;
		for (int c = 0; c < plugin.info->commandCount; c++) {
			MenuItem command = item(plugin.info->commands[c].title ? plugin.info->commands[c].title : "Command", Cmd::PluginCommand, static_cast<int>(i) * 1000 + c);
			if (plugin.info->commands[c].shortcut) command.shortcut = plugin.info->commands[c].shortcut;
			commands.push_back(command);
		}
		out.push_back(sub(plugin.name, commands));
	}
	out.push_back(separator);
	out.push_back(item("Plugins Admin…", Cmd::PluginsAdmin));
	out.push_back(item("Import Plugin…", Cmd::ImportPlugin));
	out.push_back(item("Open Plugins Folder", Cmd::OpenPluginsFolder));
	return out;
}

}  // namespace omapad

namespace omapad {

#pragma mark - Status bar, results window and tab menus

std::vector<MenuItem> Workspace::statusMenu(bool rightSide) const {
	std::vector<MenuItem> out;
	for (const MenuItem &top : mainMenu()) {
		if (rightSide && top.title == "Encoding") {
			MenuItem copy = top;
			copy.title = "Encoding";
			out.push_back(copy);
		} else if (!rightSide && top.title == "Language") {
			return top.children;
		}
	}
	if (rightSide) {
		const auto item = [](const char *title, Cmd cmd, int arg = 0) {
			MenuItem m;
			m.title = title;
			m.cmd = cmd;
			m.arg = arg;
			return m;
		};
		MenuItem eol;
		eol.title = "EOL Conversion";
		eol.children = {item("Windows (CR LF)", Cmd::EolConvert, static_cast<int>(Eol::CRLF)), item("Unix (LF)", Cmd::EolConvert, static_cast<int>(Eol::LF)), item("Macintosh (CR)", Cmd::EolConvert, static_cast<int>(Eol::CR))};
		out.insert(out.begin(), eol);
		out.insert(out.begin(), item("Go to…", Cmd::GoToDialog));
		out.push_back(item("Insert or Overwrite Mode", Cmd::ToggleOvertype));
	}
	return out;
}

std::vector<MenuItem> Workspace::resultsMenu() const {
	std::vector<MenuItem> out;
	const auto add = [&](const char *title, const char *action) {
		MenuItem item;
		item.title = title;
		item.cmd = Cmd::ResultsAction;
		item.text = action;
		out.push_back(item);
	};
	add("Fold All", "foldAll");
	add("Unfold All", "unfoldAll");
	MenuItem separator;
	separator.separator = true;
	out.push_back(separator);
	add("Copy Selected Line(s)", "copyLines");
	add("Copy Selected Pathname(s)", "copyPaths");
	add("Select All", "selectAll");
	add("Clear All", "clear");
	out.push_back(separator);
	add("Search Only in Found Lines…", "searchFound");
	add("Open Selected Pathname(s)", "openPaths");
	add("Word Wrap", "wrap");
	return out;
}

void Workspace::resultsAction(const std::string &action) {
	SciView &results = host.resultsView();
	const long first = results.send(SCI_LINEFROMPOSITION, results.send(SCI_GETSELECTIONSTART));
	const long last = results.send(SCI_LINEFROMPOSITION, results.send(SCI_GETSELECTIONEND));
	const auto lineText = [&](long line) {
		const long start = results.send(SCI_POSITIONFROMLINE, line), end = results.send(SCI_GETLINEENDPOSITION, line);
		const char *pointer = reinterpret_cast<const char *>(results.send(SCI_GETRANGEPOINTER, start, end - start));
		return pointer && end > start ? std::string(pointer, static_cast<size_t>(end - start)) : std::string();
	};
	if (action == "searchFound") {
		findInResultsDialog();
		return;
	}
	if (action == "foldAll") results.send(SCI_FOLDALL, SC_FOLDACTION_CONTRACT);
	else if (action == "unfoldAll") results.send(SCI_FOLDALL, SC_FOLDACTION_EXPAND);
	else if (action == "selectAll") results.send(SCI_SELECTALL);
	else if (action == "wrap") results.send(SCI_SETWRAPMODE, results.send(SCI_GETWRAPMODE) == SC_WRAP_NONE ? SC_WRAP_WORD : SC_WRAP_NONE);
	else if (action == "clear") setResults(std::string(), std::vector<ResultEntry>(), false);
	else if (action == "copyLines") {
		std::string out;
		for (long line = first; line <= last; line++) {
			std::string text = lineText(line);
			// Result lines start "  Line 12: ", which isn't part of the text.
			const size_t colon = text.find(": ");
			if (text.rfind("  Line ", 0) == 0 && colon != std::string::npos) out += text.substr(colon + 2) + "\n";
		}
		if (!out.empty()) host.setClipboard(out);
	} else if (action == "copyPaths" || action == "openPaths") {
		std::vector<std::string> paths;
		for (long line = first; line <= last && line < static_cast<long>(resultEntries.size()); line++) {
			const std::string &path = resultEntries[static_cast<size_t>(line)].path;
			if (!path.empty() && std::find(paths.begin(), paths.end(), path) == paths.end()) paths.push_back(path);
		}
		if (action == "openPaths") {
			openFiles(paths);
		} else {
			std::string out;
			for (const std::string &path : paths) out += path + "\n";
			if (!out.empty()) host.setClipboard(out);
		}
	}
}

void Workspace::tabAction(int pane, int index, const std::string &action) {
	if (index < 0 || index >= static_cast<int>(paneAt(pane).tabs.size())) return;
	clickTab(pane, index);
	if (action == "save") run(Cmd::Save);
	else if (action == "saveAs") run(Cmd::SaveAs);
	else if (action == "rename") run(Cmd::Rename);
	else if (action == "reload") run(Cmd::Reload);
	else if (action == "print") run(Cmd::Print);
	else if (action == "readOnly") run(Cmd::ToggleReadOnly);
	else if (action == "trash") run(Cmd::Trash);
	else if (action.rfind("colour", 0) == 0) setTabColour(pane, index, std::atoi(action.c_str() + 6));
	else if (action == "newInstance") run(Cmd::OpenInNewInstance);
	else if (action == "moveInstance") run(Cmd::MoveToNewInstance);
	else if (action == "copyName") run(Cmd::CopyPath, 1);
	else if (action == "copyFolder") run(Cmd::CopyPath, 2);
}

#pragma mark - Scintilla's own commands in the Shortcut Mapper

namespace {

struct SciCommand { const char *name; int message; int key; int modifiers; };

const std::vector<SciCommand> &sciCommands() {
	static const std::vector<SciCommand> table = {
		{"Line Down", SCI_LINEDOWN, SCK_DOWN, 0}, {"Line Up", SCI_LINEUP, SCK_UP, 0},
		{"Extend Selection Line Down", SCI_LINEDOWNEXTEND, SCK_DOWN, SCMOD_SHIFT}, {"Extend Selection Line Up", SCI_LINEUPEXTEND, SCK_UP, SCMOD_SHIFT},
		{"Column Select Down", SCI_LINEDOWNRECTEXTEND, SCK_DOWN, SCMOD_ALT | SCMOD_SHIFT}, {"Column Select Up", SCI_LINEUPRECTEXTEND, SCK_UP, SCMOD_ALT | SCMOD_SHIFT},
		{"Character Left", SCI_CHARLEFT, SCK_LEFT, 0}, {"Character Right", SCI_CHARRIGHT, SCK_RIGHT, 0},
		{"Extend Selection Left", SCI_CHARLEFTEXTEND, SCK_LEFT, SCMOD_SHIFT}, {"Extend Selection Right", SCI_CHARRIGHTEXTEND, SCK_RIGHT, SCMOD_SHIFT},
		{"Column Select Left", SCI_CHARLEFTRECTEXTEND, SCK_LEFT, SCMOD_ALT | SCMOD_SHIFT}, {"Column Select Right", SCI_CHARRIGHTRECTEXTEND, SCK_RIGHT, SCMOD_ALT | SCMOD_SHIFT},
		{"Word Left", SCI_WORDLEFT, SCK_LEFT, SCMOD_CTRL}, {"Word Right", SCI_WORDRIGHT, SCK_RIGHT, SCMOD_CTRL},
		{"Extend Selection Word Left", SCI_WORDLEFTEXTEND, SCK_LEFT, SCMOD_CTRL | SCMOD_SHIFT}, {"Extend Selection Word Right", SCI_WORDRIGHTEXTEND, SCK_RIGHT, SCMOD_CTRL | SCMOD_SHIFT},
		{"Line Start", SCI_VCHOME, SCK_HOME, 0}, {"Line End", SCI_LINEEND, SCK_END, 0},
		{"Extend Selection to Line Start", SCI_VCHOMEEXTEND, SCK_HOME, SCMOD_SHIFT}, {"Extend Selection to Line End", SCI_LINEENDEXTEND, SCK_END, SCMOD_SHIFT},
		{"Document Start", SCI_DOCUMENTSTART, SCK_HOME, SCMOD_CTRL}, {"Document End", SCI_DOCUMENTEND, SCK_END, SCMOD_CTRL},
		{"Extend Selection to Document Start", SCI_DOCUMENTSTARTEXTEND, SCK_HOME, SCMOD_CTRL | SCMOD_SHIFT}, {"Extend Selection to Document End", SCI_DOCUMENTENDEXTEND, SCK_END, SCMOD_CTRL | SCMOD_SHIFT},
		{"Page Up", SCI_PAGEUP, SCK_PRIOR, 0}, {"Page Down", SCI_PAGEDOWN, SCK_NEXT, 0},
		{"Extend Selection Page Up", SCI_PAGEUPEXTEND, SCK_PRIOR, SCMOD_SHIFT}, {"Extend Selection Page Down", SCI_PAGEDOWNEXTEND, SCK_NEXT, SCMOD_SHIFT},
		{"Scroll Line Down", SCI_LINESCROLLDOWN, SCK_DOWN, SCMOD_CTRL}, {"Scroll Line Up", SCI_LINESCROLLUP, SCK_UP, SCMOD_CTRL},
		{"Delete Right", SCI_CLEAR, SCK_DELETE, 0}, {"Delete Left", SCI_DELETEBACK, SCK_BACK, 0},
		{"Delete Word Left", SCI_DELWORDLEFT, SCK_BACK, SCMOD_CTRL}, {"Delete Word Right", SCI_DELWORDRIGHT, SCK_DELETE, SCMOD_CTRL},
		{"Delete to Line Start", SCI_DELLINELEFT, SCK_BACK, SCMOD_CTRL | SCMOD_SHIFT}, {"Delete to Line End", SCI_DELLINERIGHT, SCK_DELETE, SCMOD_CTRL | SCMOD_SHIFT},
		{"Toggle Overtype", SCI_EDITTOGGLEOVERTYPE, SCK_INSERT, 0}, {"Indent", SCI_TAB, SCK_TAB, 0}, {"Unindent", SCI_BACKTAB, SCK_TAB, SCMOD_SHIFT},
		{"New Line", SCI_NEWLINE, SCK_RETURN, 0}, {"Cancel", SCI_CANCEL, SCK_ESCAPE, 0},
		{"Line Cut", SCI_LINECUT, 'L', SCMOD_CTRL}, {"Line Copy", SCI_LINECOPY, 'T', SCMOD_CTRL | SCMOD_SHIFT},
		{"Line Delete", SCI_LINEDELETE, 'L', SCMOD_CTRL | SCMOD_SHIFT}, {"Line Transpose", SCI_LINETRANSPOSE, 'T', SCMOD_CTRL},
	};
	return table;
}

bool sciKeyFromShortcut(const std::string &shortcut, int &key, int &modifiers) {
	key = 0;
	modifiers = 0;
	std::stringstream parts(shortcut);
	std::string part;
	static const std::map<std::string, int> named = {
		{"Up", SCK_UP}, {"Down", SCK_DOWN}, {"Left", SCK_LEFT}, {"Right", SCK_RIGHT}, {"Home", SCK_HOME}, {"End", SCK_END},
		{"PageUp", SCK_PRIOR}, {"PageDown", SCK_NEXT}, {"Delete", SCK_DELETE}, {"Insert", SCK_INSERT}, {"Escape", SCK_ESCAPE},
		{"BackSpace", SCK_BACK}, {"Tab", SCK_TAB}, {"Return", SCK_RETURN},
	};
	while (std::getline(parts, part, '+')) {
		if (part == "Mod" || part == "Ctrl") modifiers |= SCMOD_CTRL;
		else if (part == "Alt") modifiers |= SCMOD_ALT;
		else if (part == "Shift") modifiers |= SCMOD_SHIFT;
		else {
			const auto found = named.find(part);
			if (found != named.end()) key = found->second;
			else if (part.size() == 1) key = std::toupper(static_cast<unsigned char>(part[0]));
		}
	}
	return key != 0;
}

std::string shortcutFromSciKey(int key, int modifiers) {
	static const std::map<int, std::string> named = {
		{SCK_UP, "Up"}, {SCK_DOWN, "Down"}, {SCK_LEFT, "Left"}, {SCK_RIGHT, "Right"}, {SCK_HOME, "Home"}, {SCK_END, "End"},
		{SCK_PRIOR, "PageUp"}, {SCK_NEXT, "PageDown"}, {SCK_DELETE, "Delete"}, {SCK_INSERT, "Insert"}, {SCK_ESCAPE, "Escape"},
		{SCK_BACK, "BackSpace"}, {SCK_TAB, "Tab"}, {SCK_RETURN, "Return"},
	};
	std::string out;
	if (modifiers & SCMOD_CTRL) out += "Mod+";
	if (modifiers & SCMOD_ALT) out += "Alt+";
	if (modifiers & SCMOD_SHIFT) out += "Shift+";
	const auto found = named.find(key);
	return out + (found != named.end() ? found->second : std::string(1, static_cast<char>(key)));
}

}  // namespace

void Workspace::appendScintillaShortcuts(std::vector<ShortcutEntry> &entries) const {
	const size_t scintillaFrom = entries.size();
	struct Tag {
		std::vector<ShortcutEntry> &list;
		size_t from;
		~Tag() { for (size_t i = from; i < list.size(); i++) list[i].category = "Scintilla commands"; }
	} tag{entries, scintillaFrom};
	// A key the menus take never reaches Scintilla, so it doesn't count as this command's.
	std::set<std::string> menuKeys;
	for (const ShortcutEntry &entry : entries) menuKeys.insert(entry.defaultShortcut);
	for (const SciCommand &command : sciCommands()) {
		ShortcutEntry entry;
		entry.id = std::string("Scintilla/") + command.name;
		entry.defaultShortcut = normalizeShortcut(shortcutFromSciKey(command.key, command.modifiers));
		if (menuKeys.count(entry.defaultShortcut)) entry.defaultShortcut.clear();
		const auto custom = settings.shortcuts.find(entry.id);
		entry.shortcut = custom != settings.shortcuts.end() ? custom->second : entry.defaultShortcut;
		entries.push_back(entry);
	}
}

void Workspace::applyScintillaShortcuts(int pane) {
	SciView &v = view(pane);
	for (const SciCommand &command : sciCommands()) {
		const auto custom = settings.shortcuts.find(std::string("Scintilla/") + command.name);
		if (custom == settings.shortcuts.end()) continue;
		v.send(SCI_CLEARCMDKEY, static_cast<uintptr_t>(command.key) | (static_cast<uintptr_t>(command.modifiers) << 16));
		int key, modifiers;
		if (sciKeyFromShortcut(custom->second, key, modifiers))
			v.send(SCI_ASSIGNCMDKEY, static_cast<uintptr_t>(key) | (static_cast<uintptr_t>(modifiers) << 16), command.message);
	}
}

}  // namespace omapad
