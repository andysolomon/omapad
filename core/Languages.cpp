#include "Languages.h"

#include "ILexer.h"
#include "Lexilla.h"
#include "SciLexer.h"
#include "Scintilla.h"
#include "pugixml.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace omapad {

namespace {

struct LangRow { const char *name; const char *display; const char *lexer; };
const LangRow kLangTable[] = {
#include "LangTable.inc"
};

std::string lowercase(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

// Colours arrive as RRGGBB text. Scintilla wants 0xBBGGRR.
bool parseColour(const std::string &hex, int *out) {
	if (hex.size() != 6) return false;
	for (char c : hex)
		if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
	const unsigned value = static_cast<unsigned>(std::stoul(hex, nullptr, 16));
	*out = static_cast<int>(((value & 0xFF) << 16) | (value & 0xFF00) | ((value >> 16) & 0xFF));
	return true;
}

ThemeStyle parseStyle(const pugi::xml_node &e) {
	ThemeStyle style;
	style.styleID = e.attribute("styleID").as_int();
	style.name = e.attribute("name").as_string();
	style.fg = e.attribute("fgColor").as_string();
	style.bg = e.attribute("bgColor").as_string();
	const std::string fontStyle = e.attribute("fontStyle").as_string();
	style.fontStyle = fontStyle.empty() ? -1 : e.attribute("fontStyle").as_int();
	style.fontName = e.attribute("fontName").as_string();
	style.fontSize = e.attribute("fontSize").as_int();
	return style;
}

std::unique_ptr<Theme> loadThemeFile(const std::string &path) {
	pugi::xml_document doc;
	if (!doc.load_file(path.c_str())) return nullptr;
	auto theme = std::make_unique<Theme>();
	const pugi::xml_node root = doc.first_element_by_path("NotepadPlus");
	for (const pugi::xml_node e : root.child("GlobalStyles").children("WidgetStyle")) {
		const std::string name = e.attribute("name").as_string();
		if (!name.empty()) theme->widgets[name] = parseStyle(e);
	}
	for (const pugi::xml_node lexer : root.child("LexerStyles").children("LexerType")) {
		const std::string name = lexer.attribute("name").as_string();
		if (name.empty()) continue;
		std::vector<ThemeStyle> styles;
		for (const pugi::xml_node e : lexer.children("WordsStyle")) styles.push_back(parseStyle(e));
		theme->lexers[name] = std::move(styles);
	}
	return theme;
}

const std::string &firstColour(const Palette &palette, std::initializer_list<const char *> keys) {
	static const std::string empty;
	for (const char *key : keys) {
		const auto found = palette.colors.find(key);
		if (found != palette.colors.end()) return found->second;
	}
	return empty;
}

bool nameHas(const std::string &name, std::initializer_list<const char *> needles) {
	for (const char *needle : needles)
		if (name.find(needle) != std::string::npos) return true;
	return false;
}

// Picks a palette colour from what a style is named. Every language in the data files names
// its styles in the same plain words, so this works across all of them.
const std::string &colourForStyleName(const std::string &rawName, const Palette &p) {
	const std::string name = lowercase(rawName);
	if (nameHas(name, {"comment", "doc tag", "doxygen"})) return firstColour(p, {"dark_foreground", "muted", "foreground"});
	if (nameHas(name, {"error", "unterminated", "invalid", "bad"})) return firstColour(p, {"red", "foreground"});
	if (nameHas(name, {"string", "character", "verbatim", "heredoc", "literal", "quote", "regex", "backtick", "cdata"}))
		return firstColour(p, {"green", "foreground"});
	if (nameHas(name, {"number", "integer", "float", "numeric", "constant", "hex", "date"}))
		return firstColour(p, {"orange", "yellow", "foreground"});
	if (nameHas(name, {"preprocessor", "macro", "directive", "pragma", "section", "shebang"}))
		return firstColour(p, {"yellow", "orange", "foreground"});
	if (nameHas(name, {"keyword", "instruction", "reserved", "statement", "word", "tag", "command", "module"}))
		return firstColour(p, {"magenta", "accent", "foreground"});
	if (nameHas(name, {"type", "class", "function", "def", "method", "attribute", "property", "key", "builtin", "label"}))
		return firstColour(p, {"accent", "blue", "foreground"});
	if (nameHas(name, {"operator", "delimiter", "punctuation", "bracket", "brace"}))
		return firstColour(p, {"cyan", "foreground"});
	return firstColour(p, {"foreground"});
}

}  // namespace

std::string Palette::get(const std::string &key, const std::string &fallbackKey) const {
	auto found = colors.find(key);
	if (found != colors.end()) return found->second;
	found = colors.find(fallbackKey);
	return found != colors.end() ? found->second : std::string();
}

Palette parsePalette(const std::string &toml) {
	Palette palette;
	std::istringstream in(toml);
	std::string line;
	while (std::getline(in, line)) {
		const size_t equals = line.find('=');
		if (equals == std::string::npos || line[0] == '#' || line[0] == '[') continue;
		const auto trim = [](std::string s) {
			const char *space = " \t\r\"'";
			const size_t a = s.find_first_not_of(space);
			if (a == std::string::npos) return std::string();
			return s.substr(a, s.find_last_not_of(space) - a + 1);
		};
		const std::string key = trim(line.substr(0, equals));
		std::string value = line.substr(equals + 1);
		const size_t commentAt = value.find(" #");
		if (commentAt != std::string::npos) value = value.substr(0, commentAt);
		value = trim(value);
		if (key == "mode") {
			palette.dark = value != "light";
			continue;
		}
		if (value.size() == 7 && value[0] == '#') {
			int ignored = 0;
			const std::string hex = value.substr(1);
			if (parseColour(hex, &ignored)) palette.colors[key] = hex;
		}
	}
	return palette;
}

LanguageCatalog::LanguageCatalog(const std::string &resourceDirectory) : directory(resourceDirectory) {
	std::map<std::string, std::pair<std::string, std::string>> table;
	for (const LangRow &row : kLangTable) table[row.name] = {row.display, row.lexer};

	pugi::xml_document doc;
	if (doc.load_file((fs::path(directory) / "langs.model.xml").c_str())) {
		for (const pugi::xml_node node : doc.first_element_by_path("NotepadPlus/Languages").children("Language")) {
			const std::string name = node.attribute("name").as_string();
			if (name.empty()) continue;
			auto language = std::make_unique<Language>();
			language->name = name;
			const auto row = table.find(name);
			language->displayName = row != table.end() ? row->second.first : name;
			language->lexerName = row != table.end() ? row->second.second : "null";
			std::istringstream extensions(lowercase(node.attribute("ext").as_string()));
			std::string extension;
			while (extensions >> extension) language->extensions.push_back(extension);
			language->commentLine = node.attribute("commentLine").as_string();
			language->commentStart = node.attribute("commentStart").as_string();
			language->commentEnd = node.attribute("commentEnd").as_string();
			const int tabSettings = node.attribute("tabSettings").as_int();
			if (tabSettings > 0) {
				language->tabSize = tabSettings & 0x7F;
				language->tabsToSpaces = (tabSettings & 0x80) != 0;
			}
			for (const pugi::xml_node kw : node.children("Keywords")) {
				const std::string cls = kw.attribute("name").as_string();
				const std::string words = kw.text().as_string();
				if (!cls.empty() && !words.empty()) language->keywords[cls] = words;
			}
			const Language *stored = language.get();
			owned.push_back(std::move(language));
			languagesByName[name] = stored;
			if (name == "normal") plain = stored;
			// Internal entries that make no sense as a choice a person would pick.
			if (name == "searchResult" || name == "javascript") continue;
			menuLanguages.push_back(stored);
			for (const std::string &e : stored->extensions)
				if (!languagesByExtension.count(e)) languagesByExtension[e] = stored;
		}
	}
	std::sort(menuLanguages.begin(), menuLanguages.end(), [](const Language *a, const Language *b) {
		return lowercase(a->displayName) < lowercase(b->displayName);
	});
	builtinExtensions = languagesByExtension;
	loadUserLanguages(std::string());

	defaultTheme = loadThemeFile((fs::path(directory) / "stylers.model.xml").string());
	std::error_code error;
	for (const auto &entry : fs::directory_iterator(fs::path(directory) / "themes", error))
		if (entry.path().extension() == ".xml") themes.push_back(entry.path().stem().string());
	std::sort(themes.begin(), themes.end(), [](const std::string &a, const std::string &b) { return lowercase(a) < lowercase(b); });
}

void LanguageCatalog::loadUserLanguages(const std::string &personalDirectory) {
	struct Found {
		std::shared_ptr<const UserLanguage> light, dark;
		std::string file;
	};
	std::map<std::string, Found> found;
	const auto readFolder = [&](const fs::path &folder, bool personal) {
		std::error_code error;
		std::vector<fs::path> files;
		for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
			if (it->path().extension() == ".xml") files.push_back(it->path());
		std::sort(files.begin(), files.end());
		for (const fs::path &file : files) {
			std::vector<UserLanguage> languages = loadUserLanguageFile(file.string());
			// A dark variant goes with the language that covers the same files, if there is one.
			std::map<std::string, std::string> lightByExtensions;
			for (const UserLanguage &language : languages)
				if (!language.darkModeTheme) lightByExtensions[language.extensions] = language.name;
			for (UserLanguage &language : languages) {
				const bool dark = language.darkModeTheme;
				auto data = std::make_shared<const UserLanguage>(std::move(language));
				std::string name = data->name;
				if (dark) {
					auto partner = lightByExtensions.find(data->extensions);
					if (partner == lightByExtensions.end()) {
						for (const auto &entry : found)
							if (entry.second.light && entry.second.light->extensions == data->extensions) name = entry.first;
					} else {
						name = partner->second;
					}
				}
				Found &slot = found[name];
				if (dark && name != data->name) slot.dark = data;
				else slot.light = data;
				if (personal) slot.file = file.string();
			}
		}
	};
	readFolder(fs::path(directory) / "userDefineLangs", false);
	if (!personalDirectory.empty()) readFolder(personalDirectory, true);

	// Anything still here at the end has gone. Its object stays, in case a document points at it.
	std::map<std::string, const Language *> gone;
	for (const Language *language : menuUserLanguages) gone[language->name] = language;
	menuUserLanguages.clear();
	languagesByExtension = builtinExtensions;
	for (const auto &entry : found) {
		const std::string name = "udl:" + entry.first;
		Language *language = nullptr;
		for (const std::unique_ptr<Language> &candidate : owned)
			if (candidate->name == name) language = candidate.get();
		if (!language) {
			owned.push_back(std::make_unique<Language>());
			language = owned.back().get();
			language->name = name;
		}
		gone.erase(name);
		const std::shared_ptr<const UserLanguage> &data = entry.second.light ? entry.second.light : entry.second.dark;
		language->displayName = entry.first;
		language->lexerName = "user";
		language->user = data;
		language->userDark = entry.second.light ? entry.second.dark : nullptr;
		language->userFile = entry.second.file;
		language->extensions.clear();
		userCommentTokens(*data, language->commentLine, language->commentStart, language->commentEnd);
		std::istringstream extensions(lowercase(data->extensions));
		std::string extension;
		while (extensions >> extension) {
			language->extensions.push_back(extension);
			// Like Notepad++, a user language comes before a built-in one.
			languagesByExtension[extension] = language;
		}
		languagesByName[name] = language;
		menuUserLanguages.push_back(language);
	}
	for (const auto &entry : gone) languagesByName.erase(entry.first);
	std::sort(menuUserLanguages.begin(), menuUserLanguages.end(), [](const Language *a, const Language *b) {
		return lowercase(a->displayName) < lowercase(b->displayName);
	});
}

const Language *LanguageCatalog::languageNamed(const std::string &name) const {
	const auto found = languagesByName.find(name);
	return found != languagesByName.end() ? found->second : plain;
}

const Language *LanguageCatalog::languageForPath(const std::string &path) const {
	const fs::path p(path);
	const std::string file = lowercase(p.filename().string());
	static const std::map<std::string, std::string> exactNames = {
		{"makefile", "makefile"}, {"gnumakefile", "makefile"}, {"pkgbuild", "bash"},
		{"cmakelists.txt", "cmake"},
		{".bashrc", "bash"}, {".bash_profile", "bash"}, {".zshrc", "bash"}, {".zprofile", "bash"}, {".profile", "bash"},
		{"gemfile", "ruby"}, {"rakefile", "ruby"},
		{"cargo.lock", "toml"}, {".gitconfig", "ini"},
	};
	const auto exact = exactNames.find(file);
	if (exact != exactNames.end() && languagesByName.count(exact->second)) return languagesByName.at(exact->second);
	// A user language can name a whole file instead of an extension.
	const auto wholeName = languagesByExtension.find(file);
	if (wholeName != languagesByExtension.end() && wholeName->second->user) return wholeName->second;
	std::string extension = p.extension().string();
	if (!extension.empty() && extension[0] == '.') extension = lowercase(extension.substr(1));
	const auto found = languagesByExtension.find(extension);
	return found != languagesByExtension.end() ? found->second : plain;
}

const Language *LanguageCatalog::languageForFirstLine(const std::string &line) const {
	const auto named = [&](const char *name) -> const Language * {
		const auto found = languagesByName.find(name);
		return found != languagesByName.end() ? found->second : nullptr;
	};
	if (line.rfind("<?xml", 0) == 0) return named("xml");
	if (line.rfind("<?php", 0) == 0) return named("php");
	if (lowercase(line).rfind("<!doctype html", 0) == 0) return named("html");
	if (line.rfind("#!", 0) != 0) return nullptr;
	static const std::pair<const char *, const char *> interpreters[] = {
		{"python", "python"}, {"bash", "bash"}, {"zsh", "bash"}, {"/sh", "bash"}, {" sh", "bash"},
		{"perl", "perl"}, {"ruby", "ruby"}, {"node", "javascript.js"}, {"lua", "lua"}, {"php", "php"}, {"tclsh", "tcl"},
	};
	for (const auto &entry : interpreters)
		if (line.find(entry.first) != std::string::npos) return named(entry.second);
	return nullptr;
}

const Theme *LanguageCatalog::themeNamed(const std::string &name) const {
	if (name.empty()) {
		// The default theme can be edited too, which leaves a personal copy called Default.
		if (!personalThemes.empty() && fs::exists(fs::path(personalThemes) / "Default.xml")) {
			const auto personal = loadedThemes.find("Default");
			if (personal != loadedThemes.end() && personal->second) return personal->second.get();
			auto theme = loadThemeFile((fs::path(personalThemes) / "Default.xml").string());
			if (theme) {
				const Theme *result = theme.get();
				loadedThemes["Default"] = std::move(theme);
				return result;
			}
		}
		return defaultTheme.get();
	}
	if (name == "Omarchy" && omarchyTheme) return omarchyTheme.get();
	const auto found = loadedThemes.find(name);
	if (found != loadedThemes.end()) return found->second ? found->second.get() : defaultTheme.get();
	// Names come from a directory listing or settings, so never let one leave the folder.
	const std::string safe = fs::path(name).filename().string();
	std::unique_ptr<Theme> theme;
	if (!personalThemes.empty()) theme = loadThemeFile((fs::path(personalThemes) / (safe + ".xml")).string());
	if (!theme) theme = loadThemeFile((fs::path(directory) / "themes" / (safe + ".xml")).string());
	const Theme *result = theme ? theme.get() : defaultTheme.get();
	loadedThemes[name] = std::move(theme);
	return result;
}

void LanguageCatalog::setPersonalThemeFolder(const std::string &folder) {
	personalThemes = folder;
	loadedThemes.clear();
	std::error_code error;
	for (const auto &entry : fs::directory_iterator(folder, error)) {
		if (entry.path().extension() != ".xml") continue;
		const std::string name = entry.path().stem().string();
		if (name != "Default" && std::find(themes.begin(), themes.end(), name) == themes.end()) themes.push_back(name);
	}
	std::sort(themes.begin(), themes.end(), [](const std::string &a, const std::string &b) { return lowercase(a) < lowercase(b); });
}

std::vector<ThemeStyle> LanguageCatalog::editableStyles(const std::string &themeName, const std::string &lexer) const {
	const Theme *theme = themeNamed(themeName == "Default" ? std::string() : themeName);
	std::vector<ThemeStyle> out;
	if (!theme) return out;
	if (lexer == "global") {
		for (const auto &widget : theme->widgets) out.push_back(widget.second);
		return out;
	}
	const auto found = theme->lexers.find(lexer);
	if (found != theme->lexers.end()) out = found->second;
	return out;
}

bool LanguageCatalog::saveStyles(const std::string &themeName, const std::string &lexer, const std::vector<ThemeStyle> &styles, std::string &error) {
	if (personalThemes.empty()) {
		error = "There's no folder to keep it in.";
		return false;
	}
	std::error_code fsError;
	fs::create_directories(personalThemes, fsError);
	const std::string safe = fs::path(themeName).filename().string();
	const fs::path personal = fs::path(personalThemes) / (safe + ".xml");
	pugi::xml_document doc;
	bool loaded = doc.load_file(personal.c_str());
	if (!loaded) loaded = doc.load_file((safe == "Default" ? fs::path(directory) / "stylers.model.xml" : fs::path(directory) / "themes" / (safe + ".xml")).c_str());
	if (!loaded) {
		error = "The theme file could not be read.";
		return false;
	}
	pugi::xml_node root = doc.first_element_by_path("NotepadPlus");
	const auto write = [](pugi::xml_node node, const ThemeStyle &style) {
		const auto set = [&](const char *name, const std::string &value) {
			pugi::xml_attribute attribute = node.attribute(name);
			if (!attribute) attribute = node.append_attribute(name);
			attribute.set_value(value.c_str());
		};
		if (!style.fg.empty()) set("fgColor", style.fg);
		if (!style.bg.empty()) set("bgColor", style.bg);
		if (style.fontStyle >= 0) set("fontStyle", std::to_string(style.fontStyle));
		set("fontName", style.fontName);
		set("fontSize", style.fontSize > 0 ? std::to_string(style.fontSize) : std::string());
	};
	if (lexer == "global") {
		for (const ThemeStyle &style : styles)
			write(root.child("GlobalStyles").find_child_by_attribute("WidgetStyle", "name", style.name.c_str()), style);
	} else {
		pugi::xml_node type = root.child("LexerStyles").find_child_by_attribute("LexerType", "name", lexer.c_str());
		for (const ThemeStyle &style : styles)
			write(type.find_child_by_attribute("WordsStyle", "styleID", std::to_string(style.styleID).c_str()), style);
	}
	if (!doc.save_file(personal.c_str())) {
		error = "The theme file could not be written.";
		return false;
	}
	loadedThemes.erase(safe);
	if (safe != "Default" && std::find(themes.begin(), themes.end(), safe) == themes.end()) themes.push_back(safe);
	return true;
}

bool LanguageCatalog::importTheme(const std::string &path, std::string &error) {
	pugi::xml_document doc;
	if (!doc.load_file(path.c_str()) || !doc.first_element_by_path("NotepadPlus/LexerStyles")) {
		error = "It isn't a Notepad++ theme file.";
		return false;
	}
	std::error_code fsError;
	fs::create_directories(personalThemes, fsError);
	const std::string name = fs::path(path).stem().string();
	if (!doc.save_file((fs::path(personalThemes) / (name + ".xml")).c_str())) {
		error = "The theme file could not be written.";
		return false;
	}
	loadedThemes.erase(name);
	if (std::find(themes.begin(), themes.end(), name) == themes.end()) themes.push_back(name);
	std::sort(themes.begin(), themes.end(), [](const std::string &a, const std::string &b) { return lowercase(a) < lowercase(b); });
	return true;
}

void LanguageCatalog::setLanguageCustomisation(const std::map<std::string, std::string> &extensions, const std::map<std::string, std::string> &keywords) {
	extraExtensions = extensions;
	extraKeywords = keywords;
	for (const auto &entry : extensions) {
		const auto language = languagesByName.find(entry.first);
		if (language == languagesByName.end()) continue;
		std::istringstream words(lowercase(entry.second));
		std::string word;
		while (words >> word) {
			languagesByExtension[word] = language->second;
			builtinExtensions[word] = language->second;
		}
	}
}

void LanguageCatalog::setOmarchyPalette(const Palette &p) {
	if (!p.valid() || !defaultTheme) {
		omarchyTheme.reset();
		return;
	}
	auto theme = std::make_unique<Theme>(*defaultTheme);
	const std::string background = p.get("background");
	const std::string foreground = p.get("foreground");
	for (auto &lexer : theme->lexers) {
		for (ThemeStyle &style : lexer.second) {
			style.bg = background;
			style.fg = colourForStyleName(style.name, p);
		}
	}
	for (auto &entry : theme->widgets) {
		entry.second.bg = background;
		entry.second.fg = foreground;
	}
	const auto set = [&](const char *name, const std::string &fg, const std::string &bg) {
		ThemeStyle &style = theme->widgets[name];
		if (!fg.empty()) style.fg = fg;
		if (!bg.empty()) style.bg = bg;
	};
	const std::string muted = p.get("muted", "dark_foreground");
	const std::string dim = p.get("dark_foreground", "foreground");
	const std::string selection = p.get("selection", "lighter_background");
	const std::string accent = p.get("accent", "blue");
	set("Line number margin", dim, background);
	set("Current line background colour", "", p.get("lighter_background", "selection"));
	set("Selected text colour", "", selection);
	set("Multi-selected text color", "", selection);
	set("Caret colour", p.get("bright_foreground", "foreground"), "");
	set("Multi-edit carets color", accent, "");
	set("Indent guideline style", muted, background);
	set("Brace highlight style", accent, background);
	set("Bad brace colour", p.get("red", "foreground"), background);
	set("Edge colour", muted, "");
	set("White space symbol", muted, "");
	set("Fold", dim, background);
	set("Fold active", accent, "");
	set("Fold margin", background, background);
	set("Bookmark margin", "", background);
	set("Smart Highlighting", "", accent);
	set("Find Mark Style", "", p.get("yellow", "accent"));
	set("Mark Style 1", "", p.get("blue", "accent"));
	set("Mark Style 2", "", p.get("green", "accent"));
	set("Mark Style 3", "", p.get("orange", "accent"));
	set("Mark Style 4", "", p.get("magenta", "accent"));
	set("Mark Style 5", "", p.get("cyan", "accent"));
	set("Change History modified", "", p.get("orange", "accent"));
	set("Change History saved", "", p.get("green", "accent"));
	set("Change History revert origin", "", p.get("cyan", "accent"));
	set("Change History revert modified", "", p.get("yellow", "accent"));
	omarchyTheme = std::move(theme);
}

std::string LanguageCatalog::backgroundOfTheme(const std::string &themeName) const {
	const Theme *theme = themeNamed(themeName);
	if (!theme) return "FFFFFF";
	const auto found = theme->widgets.find("Default Style");
	return found != theme->widgets.end() ? found->second.bg : "FFFFFF";
}

bool LanguageCatalog::themeIsDark(const std::string &themeName) const {
	int bgr = 0xFFFFFF;
	parseColour(backgroundOfTheme(themeName), &bgr);
	const double b = ((bgr >> 16) & 0xFF) / 255.0;
	const double g = ((bgr >> 8) & 0xFF) / 255.0;
	const double r = (bgr & 0xFF) / 255.0;
	return 0.2126 * r + 0.7152 * g + 0.0722 * b < 0.5;
}

void LanguageCatalog::applyStyle(const ThemeStyle &style, SciView &view) const {
	int colour = 0;
	const int id = style.styleID;
	if (id < 0 || id > 255) return;
	if (parseColour(style.fg, &colour)) view.send(SCI_STYLESETFORE, id, colour);
	if (parseColour(style.bg, &colour)) view.send(SCI_STYLESETBACK, id, colour);
	if (style.fontStyle >= 0) {
		view.send(SCI_STYLESETBOLD, id, (style.fontStyle & 1) != 0);
		view.send(SCI_STYLESETITALIC, id, (style.fontStyle & 2) != 0);
		view.send(SCI_STYLESETUNDERLINE, id, (style.fontStyle & 4) != 0);
	}
	// A theme's Default Style names a Windows font. The editor font in Preferences wins over that one.
	if (id != STYLE_DEFAULT) {
		if (!style.fontName.empty()) view.send(SCI_STYLESETFONT, id, reinterpret_cast<intptr_t>(style.fontName.c_str()));
		if (style.fontSize > 0) view.send(SCI_STYLESETSIZE, id, style.fontSize);
	}
}

void LanguageCatalog::applyLexerStyles(const std::string &lexer, const Theme &theme, SciView &view) const {
	// A theme may leave out a language. Its text then keeps the theme's default colours,
	// because mixing another theme's colours into a dark theme is unreadable.
	const auto found = theme.lexers.find(lexer);
	if (found == theme.lexers.end()) return;
	for (const ThemeStyle &style : found->second) applyStyle(style, view);
}

void LanguageCatalog::apply(const Language &language, SciView &view, const std::string &themeName,
                            const std::string &fontName, double fontSize) const {
	const Theme *themePointer = themeNamed(themeName);
	if (!themePointer) return;
	const Theme &theme = *themePointer;

	// Reset every style to the theme default, then layer specifics on top.
	const auto base = theme.widgets.find("Default Style");
	if (base != theme.widgets.end()) applyStyle(base->second, view);
	view.send(SCI_STYLESETFONT, STYLE_DEFAULT, reinterpret_cast<intptr_t>(fontName.c_str()));
	view.send(SCI_STYLESETSIZEFRACTIONAL, STYLE_DEFAULT, static_cast<intptr_t>(fontSize * SC_FONT_SIZE_MULTIPLIER));
	view.send(SCI_STYLECLEARALL);

	if (language.user) {
		const bool dark = language.userDark && themeIsDark(themeName);
		applyUserLanguage(dark ? *language.userDark : *language.user, view);
		applyChrome(theme, view);
		return;
	}

	Scintilla::ILexer5 *lexer = CreateLexer(language.lexerName.c_str());
	if (!lexer) lexer = CreateLexer("null");
	view.send(SCI_SETILEXER, 0, reinterpret_cast<intptr_t>(lexer));

	const auto setProperty = [&](const char *key, const char *value) {
		view.send(SCI_SETPROPERTY, reinterpret_cast<uintptr_t>(key), reinterpret_cast<intptr_t>(value));
	};
	const auto setKeywords = [&](const std::string *words, int index) {
		if (words && !words->empty()) view.send(SCI_SETKEYWORDS, index, reinterpret_cast<intptr_t>(words->c_str()));
	};
	const auto keywordOf = [&](const char *languageName, const char *cls) -> const std::string * {
		const auto foundLanguage = languagesByName.find(languageName);
		if (foundLanguage == languagesByName.end()) return nullptr;
		const auto foundWords = foundLanguage->second->keywords.find(cls);
		return foundWords != foundLanguage->second->keywords.end() ? &foundWords->second : nullptr;
	};
	const auto own = [&](const char *cls) -> const std::string * {
		const auto foundWords = language.keywords.find(cls);
		return foundWords != language.keywords.end() ? &foundWords->second : nullptr;
	};

	static const std::set<std::string> htmlFamily = {"html", "php", "asp", "jsp"};
	static const std::set<std::string> cppFamily = {"c", "cpp", "cs", "objc", "java", "rc", "actionscript",
	                                                "javascript", "javascript.js", "typescript", "swift", "go"};
	const std::string &name = language.name;
	if (htmlFamily.count(name)) {
		// One lexer handles a page and its embedded scripts, each with its own keywords and colours.
		setKeywords(keywordOf("html", "instre1"), 0);
		setKeywords(keywordOf("html", "instre2"), 5);
		setKeywords(keywordOf("javascript", "instre1"), 1);
		setKeywords(keywordOf("vb", "instre1"), 2);
		setKeywords(keywordOf("php", "instre1"), 4);
		for (const char *part : {"html", "javascript", "php", "asp"}) applyLexerStyles(part, theme, view);
		setProperty("fold.html", "1");
		setProperty("fold.hypertext.comment", "1");
	} else if (name == "xml") {
		setKeywords(own("instre1"), 5);
		applyLexerStyles(name, theme, view);
		setProperty("lexer.xml.allow.scripts", "0");
		setProperty("fold.html", "1");
	} else if (cppFamily.count(name)) {
		setKeywords(own("instre1"), 0);
		setKeywords(own("type1"), 1);
		setKeywords(keywordOf("cpp", "type2"), 2);
		setKeywords(own("instre2"), 3);
		if (name == "go") setProperty("lexer.cpp.backquoted.strings", "1");
		applyLexerStyles(name, theme, view);
	} else {
		static const char *classes[] = {"instre1", "instre2", "type1", "type2", "type3", "type4", "type5", "type6", "type7"};
		for (int i = 0; i < 9; i++) setKeywords(own(classes[i]), i);
		applyLexerStyles(name, theme, view);
	}
	for (const char *property : {"fold", "fold.comment", "fold.preprocessor"}) setProperty(property, "1");
	setProperty("fold.compact", "0");
	const auto extra = extraKeywords.find(name);
	if (extra != extraKeywords.end() && !extra->second.empty()) {
		const std::string *words = own("instre1");
		const std::string combined = (words ? *words + " " : std::string()) + extra->second;
		setKeywords(&combined, cppFamily.count(name) ? 0 : htmlFamily.count(name) ? 0 : 0);
	}

	if (globalOverride.any()) {
		const auto found = theme.widgets.find("Global override");
		if (found != theme.widgets.end()) {
			const ThemeStyle &style = found->second;
			int colour = 0;
			for (int id = 0; id <= STYLE_LASTPREDEFINED + 200 && id < 256; id++) {
				if (id >= STYLE_LINENUMBER && id <= STYLE_LASTPREDEFINED) continue;
				if (globalOverride.fg && parseColour(style.fg, &colour)) view.send(SCI_STYLESETFORE, id, colour);
				if (globalOverride.bg && parseColour(style.bg, &colour)) view.send(SCI_STYLESETBACK, id, colour);
				if (globalOverride.font) view.send(SCI_STYLESETFONT, id, reinterpret_cast<intptr_t>(fontName.c_str()));
				if (globalOverride.size) view.send(SCI_STYLESETSIZEFRACTIONAL, id, static_cast<intptr_t>(fontSize * SC_FONT_SIZE_MULTIPLIER));
				if (style.fontStyle >= 0) {
					if (globalOverride.bold) view.send(SCI_STYLESETBOLD, id, (style.fontStyle & 1) != 0);
					if (globalOverride.italic) view.send(SCI_STYLESETITALIC, id, (style.fontStyle & 2) != 0);
					if (globalOverride.underline) view.send(SCI_STYLESETUNDERLINE, id, (style.fontStyle & 4) != 0);
				}
			}
		}
	}

	applyChrome(theme, view);
}

void LanguageCatalog::applyUserLanguage(const UserLanguage &language, SciView &view) const {
	view.send(SCI_SETILEXER, 0, reinterpret_cast<intptr_t>(CreateLexer("user")));
	const auto setProperty = [&](const std::string &key, const std::string &value) {
		view.send(SCI_SETPROPERTY, reinterpret_cast<uintptr_t>(key.c_str()), reinterpret_cast<intptr_t>(value.c_str()));
	};
	setProperty("fold", "1");
	setProperty("userDefine.isCaseIgnored", language.caseIgnored ? "1" : "0");
	setProperty("userDefine.allowFoldOfComments", language.allowFoldOfComments ? "1" : "0");
	setProperty("userDefine.foldCompact", language.foldCompact ? "1" : "0");
	for (int i = 0; i < kUserKeywordGroups; i++) setProperty("userDefine.prefixKeywords" + std::to_string(i + 1), language.prefix[i] ? "1" : "0");
	int keywordSet = 0;
	for (int i = 0; i < kUserKeywordLists; i++) {
		if (const char *property = lexerPropertyForKeywordList(i)) {
			setProperty(property, language.keywords[i]);
		} else {
			const std::string words = lexerKeywords(language.keywords[i]);
			view.send(SCI_SETKEYWORDS, keywordSet++, reinterpret_cast<intptr_t>(words.c_str()));
		}
	}
	setProperty("userDefine.forcePureLC", std::to_string(language.forcePureLC));
	setProperty("userDefine.decimalSeparator", std::to_string(language.decimalSeparator));
	// The lexer keeps parsed keywords per language and nesting per document. It reads these as int, so they
	// can't be pointers on a 64-bit system, unlike in Notepad++.
	const auto smallId = [](size_t value) { return std::to_string(value & 0x7FFFFFFF); };
	setProperty("userDefine.udlName", smallId(std::hash<std::string>()(language.name)));
	setProperty("userDefine.currentBufferID", smallId(static_cast<size_t>(view.send(SCI_GETDOCPOINTER))));

	for (int id = 0; id < kUserStyles; id++) {
		const UserStyle &style = language.styles[id];
		char nestingKey[32];
		std::snprintf(nestingKey, sizeof nestingKey, "userDefine.nesting.%02d", id);
		setProperty(nestingKey, std::to_string(style.nesting));
		int colour = 0;
		if ((style.colorStyle & 1) && parseColour(style.fg, &colour)) view.send(SCI_STYLESETFORE, id, colour);
		if ((style.colorStyle & 2) && parseColour(style.bg, &colour)) view.send(SCI_STYLESETBACK, id, colour);
		if (!style.fontName.empty()) view.send(SCI_STYLESETFONT, id, reinterpret_cast<intptr_t>(style.fontName.c_str()));
		view.send(SCI_STYLESETBOLD, id, (style.fontStyle & 1) != 0);
		view.send(SCI_STYLESETITALIC, id, (style.fontStyle & 2) != 0);
		view.send(SCI_STYLESETUNDERLINE, id, (style.fontStyle & 4) != 0);
		if (style.fontSize > 0) view.send(SCI_STYLESETSIZE, id, style.fontSize);
	}
}

// Everything in a theme that isn't a language colour: caret, selection, margins, markers.
void LanguageCatalog::applyChrome(const Theme &theme, SciView &view) const {
	const auto widget = [&](const char *name) -> const ThemeStyle * {
		const auto found = theme.widgets.find(name);
		if (found != theme.widgets.end()) return &found->second;
		if (!defaultTheme) return nullptr;
		const auto fallback = defaultTheme->widgets.find(name);
		return fallback != defaultTheme->widgets.end() ? &fallback->second : nullptr;
	};
	const auto fg = [&](const char *name, int *out) {
		const ThemeStyle *style = widget(name);
		return style && parseColour(style->fg, out);
	};
	const auto bg = [&](const char *name, int *out) {
		const ThemeStyle *style = widget(name);
		return style && parseColour(style->bg, out);
	};
	const auto opaque = [](int bgr) { return static_cast<intptr_t>(static_cast<unsigned>(bgr) | 0xFF000000u); };
	int colour = 0;

	for (const char *name : {"Indent guideline style", "Brace highlight style", "Bad brace colour", "Line number margin"}) {
		const ThemeStyle *style = widget(name);
		if (style && style->styleID > 0) applyStyle(*style, view);
	}
	if (bg("Current line background colour", &colour)) {
		view.send(SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET_LINE_BACK, opaque(colour));
		view.send(SCI_SETCARETLINELAYER, SC_LAYER_BASE);
	}
	if (bg("Selected text colour", &colour)) {
		view.send(SCI_SETSELECTIONLAYER, SC_LAYER_UNDER_TEXT);
		view.send(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_BACK, opaque(colour));
		view.send(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_INACTIVE_BACK, opaque(colour));
		view.send(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_ADDITIONAL_BACK, opaque(colour));
	}
	if (fg("Caret colour", &colour)) view.send(SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET, opaque(colour));
	if (fg("Multi-edit carets color", &colour)) view.send(SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET_ADDITIONAL, opaque(colour));
	if (fg("Edge colour", &colour)) view.send(SCI_SETEDGECOLOUR, colour);
	if (fg("White space symbol", &colour)) view.send(SCI_SETELEMENTCOLOUR, SC_ELEMENT_WHITE_SPACE, opaque(colour));
	if (bg("Fold margin", &colour)) {
		view.send(SCI_SETFOLDMARGINCOLOUR, 1, colour);
		int hi = colour;
		fg("Fold margin", &hi);
		view.send(SCI_SETFOLDMARGINHICOLOUR, 1, hi);
	}
	if (bg("Bookmark margin", &colour)) view.send(SCI_SETMARGINBACKN, 1, colour);
	int foldFg = 0, foldBg = 0, active = 0;
	if (fg("Fold", &foldFg) && bg("Fold", &foldBg)) {
		const bool hasActive = fg("Fold active", &active);
		for (int marker = SC_MARKNUM_FOLDEREND; marker <= SC_MARKNUM_FOLDEROPEN; marker++) {
			view.send(SCI_MARKERSETFORE, marker, foldBg);
			view.send(SCI_MARKERSETBACK, marker, foldFg);
			if (hasActive) view.send(SCI_MARKERSETBACKSELECTED, marker, active);
		}
	}
	// Indicators 30 and 31 hold smart highlighting and Mark All. See Workspace.
	if (bg("Smart Highlighting", &colour)) view.send(SCI_INDICSETFORE, 30, colour);
	if (bg("Find Mark Style", &colour)) view.send(SCI_INDICSETFORE, 31, colour);
	// Token styles 1 to 5 use indicators 25 down to 21, as Notepad++ does.
	const char *markStyles[] = {"Mark Style 1", "Mark Style 2", "Mark Style 3", "Mark Style 4", "Mark Style 5"};
	for (int i = 0; i < 5; i++)
		if (bg(markStyles[i], &colour)) view.send(SCI_INDICSETFORE, 25 - i, colour);
	const std::pair<const char *, int> history[] = {
		{"Change History revert origin", SC_MARKNUM_HISTORY_REVERTED_TO_ORIGIN}, {"Change History saved", SC_MARKNUM_HISTORY_SAVED},
		{"Change History modified", SC_MARKNUM_HISTORY_MODIFIED}, {"Change History revert modified", SC_MARKNUM_HISTORY_REVERTED_TO_MODIFIED},
	};
	for (const auto &entry : history) {
		if (bg(entry.first, &colour)) {
			view.send(SCI_MARKERSETBACK, entry.second, colour);
			view.send(SCI_MARKERSETFORE, entry.second, colour);
		}
	}
}

}  // namespace omapad
