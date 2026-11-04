// Language definitions and colour themes, loaded from the XML data files.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "UserLanguage.h"

namespace omapad {

// One Scintilla view. Each platform wraps its own widget in this.
struct SciView {
	virtual ~SciView() = default;
	virtual intptr_t send(unsigned message, uintptr_t w = 0, intptr_t l = 0) = 0;
};

struct Language {
	std::string name;
	std::string displayName;
	std::string lexerName;
	std::vector<std::string> extensions;
	std::string commentLine;
	std::string commentStart;
	std::string commentEnd;
	std::map<std::string, std::string> keywords;
	int tabSize = 0;  // 0 means use the editor's setting.
	bool tabsToSpaces = false;
	// Set for a user-defined language. Dark is the variant for dark themes, if it has one.
	std::shared_ptr<const UserLanguage> user, userDark;
	std::string userFile;  // The file in the person's folder. Empty for one shipped with the app.
};

// One style row from a theme: colours and bold/italic/underline.
struct ThemeStyle {
	int styleID = 0;
	std::string name;
	std::string fg;
	std::string bg;
	int fontStyle = -1;  // Bit 1 bold, bit 2 italic, bit 4 underline. -1 means unset.
};

struct Theme {
	std::map<std::string, ThemeStyle> widgets;
	std::map<std::string, std::vector<ThemeStyle>> lexers;
};

// Omarchy's active colours, read from its colors.toml. Values are RRGGBB without the #.
struct Palette {
	std::map<std::string, std::string> colors;
	bool dark = true;
	bool valid() const { return colors.count("background") && colors.count("foreground"); }
	std::string get(const std::string &key, const std::string &fallbackKey = "foreground") const;
};
// Parses the small part of TOML that colors.toml uses. Never throws on bad input.
Palette parsePalette(const std::string &toml);

class LanguageCatalog {
public:
	explicit LanguageCatalog(const std::string &resourceDirectory);

	const std::vector<const Language *> &languages() const { return menuLanguages; }  // Sorted for menus.
	const Language *plainText() const { return plain; }
	const std::vector<const Language *> &userLanguages() const { return menuUserLanguages; }  // Sorted for menus.
	const std::vector<std::string> &themeNames() const { return themes; }
	const Language *languageNamed(const std::string &name) const;  // Falls back to plain text.
	// Matches by exact file name, then extension. Never returns null.
	const Language *languageForPath(const std::string &path) const;
	// Matches a shebang or XML/PHP header. Returns null if the line says nothing.
	const Language *languageForFirstLine(const std::string &line) const;

	// Reads user languages from the app's folder, then the person's, which win on a clash.
	// A language object stays valid afterwards, even if it has gone: check with languageNamed.
	void loadUserLanguages(const std::string &personalDirectory);

	// Builds the theme named "Omarchy" from a palette, replacing any earlier one.
	void setOmarchyPalette(const Palette &palette);
	bool hasOmarchyTheme() const { return omarchyTheme != nullptr; }

	// Installs the lexer, keywords and every colour. An empty theme name means the default.
	void apply(const Language &language, SciView &view, const std::string &themeName,
	           const std::string &fontName, double fontSize) const;
	std::string backgroundOfTheme(const std::string &themeName) const;  // RRGGBB.
	bool themeIsDark(const std::string &themeName) const;
	bool valid() const { return plain != nullptr && !languagesByName.empty(); }

private:
	const Theme *themeNamed(const std::string &name) const;
	void applyStyle(const ThemeStyle &style, SciView &view) const;
	void applyLexerStyles(const std::string &lexer, const Theme &theme, SciView &view) const;
	void applyChrome(const Theme &theme, SciView &view) const;
	void applyUserLanguage(const UserLanguage &language, SciView &view) const;

	std::string directory;
	std::vector<std::unique_ptr<Language>> owned;
	std::map<std::string, const Language *> languagesByName;
	std::map<std::string, const Language *> languagesByExtension;
	std::vector<const Language *> menuLanguages;
	std::vector<const Language *> menuUserLanguages;
	std::map<std::string, const Language *> builtinExtensions;
	const Language *plain = nullptr;
	std::vector<std::string> themes;
	std::unique_ptr<Theme> defaultTheme;
	std::unique_ptr<Theme> omarchyTheme;
	mutable std::map<std::string, std::unique_ptr<Theme>> loadedThemes;
};

}  // namespace omapad
