// User-defined languages, in the XML format Notepad++ calls UDL. Pure data: nothing here touches Scintilla.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace omapad {

struct FormField;

constexpr int kUserKeywordLists = 28;  // SCE_USER_KWLIST_TOTAL.
constexpr int kUserStyles = 24;        // SCE_USER_STYLE_TOTAL_STYLES.
constexpr int kUserKeywordGroups = 8;

struct UserStyle {
	std::string fg = "000000", bg = "FFFFFF";  // RRGGBB.
	int colorStyle = 0;                        // Bit 1 uses fg, bit 2 uses bg. Off means the theme's colour.
	std::string fontName;
	int fontStyle = 0;  // Bit 1 bold, bit 2 italic, bit 4 underline.
	int fontSize = 0;   // 0 means the editor's size.
	int nesting = 0;
};

struct UserLanguage {
	std::string name;
	std::string extensions;  // Space separated, without dots.
	bool darkModeTheme = false;
	bool caseIgnored = false;
	bool allowFoldOfComments = false;
	bool foldCompact = false;
	int forcePureLC = 0;  // 0 anywhere, 1 at line start, 2 after whitespace.
	int decimalSeparator = 0;  // 0 dot, 1 comma, 2 both.
	bool prefix[kUserKeywordGroups] = {};
	std::string keywords[kUserKeywordLists];
	UserStyle styles[kUserStyles];
};

// Every language in the XML, including the older formats, which come back in the current form.
std::vector<UserLanguage> parseUserLanguages(const std::string &xml);
std::vector<UserLanguage> loadUserLanguageFile(const std::string &path);
std::string writeUserLanguages(const std::vector<UserLanguage> &languages);

// Comment and delimiter lists hold numbered groups, like "00// 01 02((EOL)) 03/* 04*/".
std::vector<std::string> userGroups(const std::string &encoded, int count);  // Space separated alternatives per group.
std::string encodeUserGroups(const std::vector<std::string> &groups);
// The alternatives of a group. "((a b))" counts as one.
std::vector<std::string> userGroupTokens(const std::string &group);

// What the lexer wants for a keyword list: quoted phrases joined by its \v and \b markers.
std::string lexerKeywords(const std::string &words);
// The lexer property for a list, or null for the lists it takes as keywords instead.
const char *lexerPropertyForKeywordList(int list);

// Line and block comment markers for the comment commands. Empty if the language has none.
void userCommentTokens(const UserLanguage &language, std::string &line, std::string &open, std::string &close);

// The form that edits a language, and reading the answers back. False means the name was empty.
std::vector<FormField> userLanguageFields(const UserLanguage &language);
bool applyUserLanguageFields(const std::vector<FormField> &fields, UserLanguage &language);

// A safe file name for a language, like "My_Language.udl.xml".
std::string userLanguageFileName(const std::string &name);

}  // namespace omapad
