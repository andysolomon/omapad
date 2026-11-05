#include "UserLanguage.h"

#include "Workspace.h"

#include "SciLexer.h"
#include "pugixml.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace omapad {

namespace {

const std::map<std::string, int> &keywordIds() {
	static const std::map<std::string, int> ids = {
		// Before 2.0.
		{"Operators", SCE_USER_KWLIST_OPERATORS1},
		{"Folder+", SCE_USER_KWLIST_FOLDERS_IN_CODE1_OPEN},
		{"Folder-", SCE_USER_KWLIST_FOLDERS_IN_CODE1_CLOSE},
		{"Words1", SCE_USER_KWLIST_KEYWORDS1},
		{"Words2", SCE_USER_KWLIST_KEYWORDS2},
		{"Words3", SCE_USER_KWLIST_KEYWORDS3},
		{"Words4", SCE_USER_KWLIST_KEYWORDS4},
		// 2.0.
		{"Numbers, additional", SCE_USER_KWLIST_NUMBER_RANGE},
		{"Numbers, prefixes", SCE_USER_KWLIST_NUMBER_PREFIX2},
		{"Numbers, extras with prefixes", SCE_USER_KWLIST_NUMBER_EXTRAS2},
		{"Numbers, suffixes", SCE_USER_KWLIST_NUMBER_SUFFIX2},
		// Current.
		{"Comments", SCE_USER_KWLIST_COMMENTS},
		{"Numbers, prefix1", SCE_USER_KWLIST_NUMBER_PREFIX1},
		{"Numbers, prefix2", SCE_USER_KWLIST_NUMBER_PREFIX2},
		{"Numbers, extras1", SCE_USER_KWLIST_NUMBER_EXTRAS1},
		{"Numbers, extras2", SCE_USER_KWLIST_NUMBER_EXTRAS2},
		{"Numbers, suffix1", SCE_USER_KWLIST_NUMBER_SUFFIX1},
		{"Numbers, suffix2", SCE_USER_KWLIST_NUMBER_SUFFIX2},
		{"Numbers, range", SCE_USER_KWLIST_NUMBER_RANGE},
		{"Operators1", SCE_USER_KWLIST_OPERATORS1},
		{"Operators2", SCE_USER_KWLIST_OPERATORS2},
		{"Folders in code1, open", SCE_USER_KWLIST_FOLDERS_IN_CODE1_OPEN},
		{"Folders in code1, middle", SCE_USER_KWLIST_FOLDERS_IN_CODE1_MIDDLE},
		{"Folders in code1, close", SCE_USER_KWLIST_FOLDERS_IN_CODE1_CLOSE},
		{"Folders in code2, open", SCE_USER_KWLIST_FOLDERS_IN_CODE2_OPEN},
		{"Folders in code2, middle", SCE_USER_KWLIST_FOLDERS_IN_CODE2_MIDDLE},
		{"Folders in code2, close", SCE_USER_KWLIST_FOLDERS_IN_CODE2_CLOSE},
		{"Folders in comment, open", SCE_USER_KWLIST_FOLDERS_IN_COMMENT_OPEN},
		{"Folders in comment, middle", SCE_USER_KWLIST_FOLDERS_IN_COMMENT_MIDDLE},
		{"Folders in comment, close", SCE_USER_KWLIST_FOLDERS_IN_COMMENT_CLOSE},
		{"Keywords1", SCE_USER_KWLIST_KEYWORDS1},
		{"Keywords2", SCE_USER_KWLIST_KEYWORDS2},
		{"Keywords3", SCE_USER_KWLIST_KEYWORDS3},
		{"Keywords4", SCE_USER_KWLIST_KEYWORDS4},
		{"Keywords5", SCE_USER_KWLIST_KEYWORDS5},
		{"Keywords6", SCE_USER_KWLIST_KEYWORDS6},
		{"Keywords7", SCE_USER_KWLIST_KEYWORDS7},
		{"Keywords8", SCE_USER_KWLIST_KEYWORDS8},
		{"Delimiters", SCE_USER_KWLIST_DELIMITERS},
	};
	return ids;
}

// In id order, for writing.
const char *const kKeywordNames[kUserKeywordLists] = {
	"Comments", "Numbers, prefix1", "Numbers, prefix2", "Numbers, extras1", "Numbers, extras2",
	"Numbers, suffix1", "Numbers, suffix2", "Numbers, range", "Operators1", "Operators2",
	"Folders in code1, open", "Folders in code1, middle", "Folders in code1, close",
	"Folders in code2, open", "Folders in code2, middle", "Folders in code2, close",
	"Folders in comment, open", "Folders in comment, middle", "Folders in comment, close",
	"Keywords1", "Keywords2", "Keywords3", "Keywords4", "Keywords5", "Keywords6", "Keywords7", "Keywords8",
	"Delimiters",
};

const char *const kStyleNames[kUserStyles] = {
	"DEFAULT", "COMMENTS", "LINE COMMENTS", "NUMBERS",
	"KEYWORDS1", "KEYWORDS2", "KEYWORDS3", "KEYWORDS4", "KEYWORDS5", "KEYWORDS6", "KEYWORDS7", "KEYWORDS8",
	"OPERATORS", "FOLDER IN CODE1", "FOLDER IN CODE2", "FOLDER IN COMMENT",
	"DELIMITERS1", "DELIMITERS2", "DELIMITERS3", "DELIMITERS4", "DELIMITERS5", "DELIMITERS6", "DELIMITERS7", "DELIMITERS8",
};

const char *const kStyleLabels[kUserStyles] = {
	"Default", "Comment", "Line comment", "Number",
	"Keywords 1", "Keywords 2", "Keywords 3", "Keywords 4", "Keywords 5", "Keywords 6", "Keywords 7", "Keywords 8",
	"Operators", "Folding in code 1", "Folding in code 2", "Folding in comment",
	"Delimiter 1", "Delimiter 2", "Delimiter 3", "Delimiter 4", "Delimiter 5", "Delimiter 6", "Delimiter 7", "Delimiter 8",
};

int styleIdForName(const std::string &name) {
	for (int i = 0; i < kUserStyles; i++)
		if (name == kStyleNames[i]) return i;
	static const std::map<std::string, int> older = {
		{"FOLDEROPEN", SCE_USER_STYLE_FOLDER_IN_CODE1}, {"FOLDERCLOSE", SCE_USER_STYLE_FOLDER_IN_CODE1},
		{"KEYWORD1", SCE_USER_STYLE_KEYWORD1}, {"KEYWORD2", SCE_USER_STYLE_KEYWORD2},
		{"KEYWORD3", SCE_USER_STYLE_KEYWORD3}, {"KEYWORD4", SCE_USER_STYLE_KEYWORD4},
		{"COMMENT", SCE_USER_STYLE_COMMENT}, {"COMMENT LINE", SCE_USER_STYLE_COMMENTLINE},
		{"NUMBER", SCE_USER_STYLE_NUMBER}, {"OPERATOR", SCE_USER_STYLE_OPERATOR},
		{"DELIMINER1", SCE_USER_STYLE_DELIMITER1}, {"DELIMINER2", SCE_USER_STYLE_DELIMITER2},
		{"DELIMINER3", SCE_USER_STYLE_DELIMITER3},
	};
	const auto found = older.find(name);
	return found != older.end() ? found->second : -1;
}

bool isYes(const pugi::xml_attribute &attribute) { return std::strcmp(attribute.as_string(), "yes") == 0; }

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Delimiters written before 2.0 are six single characters: open and close for three pairs.
std::string convertOldDelimiters(const std::string &old) {
	std::vector<std::string> groups(24);
	const auto setIf = [&](size_t at, int group) {
		if (at < old.size() && old[at] != '0') groups[group] = std::string(1, old[at]);
	};
	setIf(0, 0);
	setIf(3, 2);
	setIf(1, 3);
	setIf(4, 5);
	setIf(2, 6);
	setIf(5, 8);
	return encodeUserGroups(groups);
}

// Comments written before 2.0 are "0line 1open 2close".
std::string convertOldComments(const std::string &old) {
	std::vector<std::string> groups(5);
	std::istringstream words(old);
	std::string word;
	while (words >> word) {
		if (word.size() < 2) continue;
		const int group = word[0] == '0' ? 0 : word[0] == '1' ? 3 : word[0] == '2' ? 4 : -1;
		if (group < 0) continue;
		if (!groups[group].empty()) groups[group] += ' ';
		groups[group] += word.substr(1);
	}
	return encodeUserGroups(groups);
}

UserLanguage parseLanguage(const pugi::xml_node &node) {
	UserLanguage language;
	language.name = node.attribute("name").as_string();
	language.extensions = node.attribute("ext").as_string();
	language.darkModeTheme = isYes(node.attribute("darkModeTheme"));
	const std::string version = node.attribute("udlVersion").as_string();
	const bool current = version == "2.0" || version == "2.1";

	const pugi::xml_node settings = node.child("Settings");
	const pugi::xml_node global = settings.child("Global");
	language.caseIgnored = isYes(global.attribute("caseIgnored"));
	language.allowFoldOfComments = isYes(global.attribute("allowFoldOfComments"));
	language.foldCompact = isYes(global.attribute("foldCompact"));
	language.forcePureLC = global.attribute("forcePureLC").as_int();
	language.decimalSeparator = global.attribute("decimalSeparator").as_int();
	const pugi::xml_node prefix = settings.child("Prefix");
	for (int i = 0; i < kUserKeywordGroups; i++) {
		const std::string attribute = (current ? "Keywords" : "words") + std::to_string(i + 1);
		language.prefix[i] = isYes(prefix.attribute(attribute.c_str()));
	}

	for (const pugi::xml_node keywords : node.child("KeywordLists").children("Keywords")) {
		const std::string name = keywords.attribute("name").as_string();
		const std::string value = keywords.text().as_string();
		if (name == "Comment") {
			language.keywords[SCE_USER_KWLIST_COMMENTS] = convertOldComments(value);
		} else if (name == "Delimiters" && version.empty()) {
			language.keywords[SCE_USER_KWLIST_DELIMITERS] = convertOldDelimiters(value);
		} else {
			const auto found = keywordIds().find(name);
			if (found != keywordIds().end()) language.keywords[found->second] = value;
		}
	}

	for (const pugi::xml_node style : node.child("Styles").children("WordsStyle")) {
		const int id = styleIdForName(style.attribute("name").as_string());
		if (id < 0) continue;
		UserStyle &out = language.styles[id];
		out.fg = style.attribute("fgColor").as_string("000000");
		out.bg = style.attribute("bgColor").as_string("FFFFFF");
		out.colorStyle = style.attribute("colorStyle").as_int(3);
		out.fontName = style.attribute("fontName").as_string();
		out.fontStyle = style.attribute("fontStyle").as_int();
		out.fontSize = style.attribute("fontSize").as_int();
		out.nesting = style.attribute("nesting").as_int();
	}
	return language;
}

std::vector<UserLanguage> languagesIn(const pugi::xml_document &document) {
	std::vector<UserLanguage> out;
	for (const pugi::xml_node node : document.child("NotepadPlus").children("UserLang")) {
		UserLanguage language = parseLanguage(node);
		if (!language.name.empty()) out.push_back(std::move(language));
	}
	return out;
}

constexpr auto kKeepEverything = pugi::parse_default | pugi::parse_ws_pcdata_single;

}  // namespace

std::vector<UserLanguage> parseUserLanguages(const std::string &xml) {
	pugi::xml_document document;
	if (!document.load_buffer(xml.data(), xml.size(), kKeepEverything)) return {};
	return languagesIn(document);
}

std::vector<UserLanguage> loadUserLanguageFile(const std::string &path) {
	pugi::xml_document document;
	if (!document.load_file(path.c_str(), kKeepEverything)) return {};
	return languagesIn(document);
}

std::string writeUserLanguages(const std::vector<UserLanguage> &languages) {
	pugi::xml_document document;
	pugi::xml_node declaration = document.append_child(pugi::node_declaration);
	declaration.append_attribute("version") = "1.0";
	declaration.append_attribute("encoding") = "UTF-8";
	pugi::xml_node root = document.append_child("NotepadPlus");
	for (const UserLanguage &language : languages) {
		pugi::xml_node node = root.append_child("UserLang");
		node.append_attribute("name") = language.name.c_str();
		node.append_attribute("ext") = language.extensions.c_str();
		if (language.darkModeTheme) node.append_attribute("darkModeTheme") = "yes";
		node.append_attribute("udlVersion") = "2.1";
		const auto yesNo = [](bool on) { return on ? "yes" : "no"; };

		pugi::xml_node settings = node.append_child("Settings");
		pugi::xml_node global = settings.append_child("Global");
		global.append_attribute("caseIgnored") = yesNo(language.caseIgnored);
		global.append_attribute("allowFoldOfComments") = yesNo(language.allowFoldOfComments);
		global.append_attribute("foldCompact") = yesNo(language.foldCompact);
		global.append_attribute("forcePureLC") = language.forcePureLC;
		global.append_attribute("decimalSeparator") = language.decimalSeparator;
		pugi::xml_node prefix = settings.append_child("Prefix");
		for (int i = 0; i < kUserKeywordGroups; i++)
			prefix.append_attribute(("Keywords" + std::to_string(i + 1)).c_str()) = yesNo(language.prefix[i]);

		pugi::xml_node lists = node.append_child("KeywordLists");
		for (int i = 0; i < kUserKeywordLists; i++) {
			pugi::xml_node keywords = lists.append_child("Keywords");
			keywords.append_attribute("name") = kKeywordNames[i];
			if (!language.keywords[i].empty()) keywords.text().set(language.keywords[i].c_str());
		}

		pugi::xml_node styles = node.append_child("Styles");
		for (int i = 0; i < kUserStyles; i++) {
			const UserStyle &style = language.styles[i];
			pugi::xml_node out = styles.append_child("WordsStyle");
			out.append_attribute("name") = kStyleNames[i];
			out.append_attribute("fgColor") = style.fg.c_str();
			out.append_attribute("bgColor") = style.bg.c_str();
			out.append_attribute("colorStyle") = style.colorStyle;
			out.append_attribute("fontName") = style.fontName.c_str();
			out.append_attribute("fontStyle") = style.fontStyle;
			if (style.fontSize > 0) out.append_attribute("fontSize") = style.fontSize;
			out.append_attribute("nesting") = style.nesting;
		}
	}
	std::ostringstream out;
	document.save(out, "    ", pugi::format_default, pugi::encoding_utf8);
	return out.str();
}

std::vector<std::string> userGroupTokens(const std::string &group) {
	std::vector<std::string> tokens;
	std::string token;
	bool inParens = false;
	for (size_t i = 0; i < group.size(); i++) {
		const char c = group[i];
		if (!inParens && c == '(' && i + 1 < group.size() && group[i + 1] == '(') inParens = true;
		if (inParens && c == ')' && i > 0 && group[i - 1] == ')') {
			token += c;
			inParens = false;
			continue;
		}
		if (c == ' ' && !inParens) {
			if (!token.empty()) tokens.push_back(token);
			token.clear();
			continue;
		}
		token += c;
	}
	if (!token.empty()) tokens.push_back(token);
	return tokens;
}

std::vector<std::string> userGroups(const std::string &encoded, int count) {
	std::vector<std::string> groups(count);
	for (const std::string &token : userGroupTokens(encoded)) {
		if (token.size() < 2 || !isDigit(token[0]) || !isDigit(token[1])) continue;
		const int group = (token[0] - '0') * 10 + (token[1] - '0');
		const std::string value = token.substr(2);
		if (group >= count || value.empty()) continue;
		if (!groups[group].empty()) groups[group] += ' ';
		groups[group] += value;
	}
	return groups;
}

std::string encodeUserGroups(const std::vector<std::string> &groups) {
	std::string out;
	const auto add = [&](const std::string &token) {
		if (!out.empty()) out += ' ';
		out += token;
	};
	for (size_t i = 0; i < groups.size(); i++) {
		char number[8];
		std::snprintf(number, sizeof number, "%02d", static_cast<int>(i));
		const std::vector<std::string> tokens = userGroupTokens(groups[i]);
		if (tokens.empty()) add(number);
		for (const std::string &token : tokens) add(number + token);
	}
	return out;
}

std::string lexerKeywords(const std::string &words) {
	std::string out;
	bool inDouble = false, inSingle = false, nonSpaceFound = false;
	for (size_t i = 0; i < words.size(); i++) {
		const char c = words[i];
		const char next = i + 1 < words.size() ? words[i + 1] : '\0';
		const char previous = i > 0 ? words[i - 1] : '\0';
		if (!inSingle && c == '"') {
			inDouble = !inDouble;
			continue;
		}
		if (!inDouble && c == '\'') {
			inSingle = !inSingle;
			continue;
		}
		if (c == '\\' && (next == '"' || next == '\'' || next == '\\')) {
			out += next;
			i++;
			continue;
		}
		if (inDouble || inSingle) {
			if (static_cast<unsigned char>(c) > ' ') {
				out += c;
				nonSpaceFound = true;
			} else if (nonSpaceFound && previous != '"' && next != '"' && static_cast<unsigned char>(next) > ' ') {
				out += inDouble ? '\v' : '\b';
			}
		} else {
			out += c;
		}
	}
	return out;
}

const char *lexerPropertyForKeywordList(int list) {
	switch (list) {
	case SCE_USER_KWLIST_COMMENTS: return "userDefine.comments";
	case SCE_USER_KWLIST_DELIMITERS: return "userDefine.delimiters";
	case SCE_USER_KWLIST_OPERATORS1: return "userDefine.operators1";
	case SCE_USER_KWLIST_NUMBER_PREFIX1: return "userDefine.numberPrefix1";
	case SCE_USER_KWLIST_NUMBER_PREFIX2: return "userDefine.numberPrefix2";
	case SCE_USER_KWLIST_NUMBER_EXTRAS1: return "userDefine.numberExtras1";
	case SCE_USER_KWLIST_NUMBER_EXTRAS2: return "userDefine.numberExtras2";
	case SCE_USER_KWLIST_NUMBER_SUFFIX1: return "userDefine.numberSuffix1";
	case SCE_USER_KWLIST_NUMBER_SUFFIX2: return "userDefine.numberSuffix2";
	case SCE_USER_KWLIST_NUMBER_RANGE: return "userDefine.numberRange";
	case SCE_USER_KWLIST_FOLDERS_IN_CODE1_OPEN: return "userDefine.foldersInCode1Open";
	case SCE_USER_KWLIST_FOLDERS_IN_CODE1_MIDDLE: return "userDefine.foldersInCode1Middle";
	case SCE_USER_KWLIST_FOLDERS_IN_CODE1_CLOSE: return "userDefine.foldersInCode1Close";
	default: return nullptr;
	}
}

void userCommentTokens(const UserLanguage &language, std::string &line, std::string &open, std::string &close) {
	const std::vector<std::string> groups = userGroups(language.keywords[SCE_USER_KWLIST_COMMENTS], 5);
	// A marker of "((EOL))" and the like isn't something to type.
	const auto first = [](const std::string &group) {
		const std::vector<std::string> tokens = userGroupTokens(group);
		return !tokens.empty() && tokens[0].rfind("((", 0) != 0 ? tokens[0] : std::string();
	};
	line = first(groups[0]);
	open = first(groups[3]);
	close = first(groups[4]);
	if (open.empty() || close.empty()) open = close = std::string();
}

#pragma mark - The editing form

namespace {

struct NestingFlag {
	int mask;
	const char *label;
};

const NestingFlag kNestingFlags[] = {
	{SCE_USER_MASK_NESTING_DELIMITER1, "Delimiter 1"}, {SCE_USER_MASK_NESTING_DELIMITER2, "Delimiter 2"},
	{SCE_USER_MASK_NESTING_DELIMITER3, "Delimiter 3"}, {SCE_USER_MASK_NESTING_DELIMITER4, "Delimiter 4"},
	{SCE_USER_MASK_NESTING_DELIMITER5, "Delimiter 5"}, {SCE_USER_MASK_NESTING_DELIMITER6, "Delimiter 6"},
	{SCE_USER_MASK_NESTING_DELIMITER7, "Delimiter 7"}, {SCE_USER_MASK_NESTING_DELIMITER8, "Delimiter 8"},
	{SCE_USER_MASK_NESTING_COMMENT, "Comment"}, {SCE_USER_MASK_NESTING_COMMENT_LINE, "Line comment"},
	{SCE_USER_MASK_NESTING_KEYWORD1, "Keywords 1"}, {SCE_USER_MASK_NESTING_KEYWORD2, "Keywords 2"},
	{SCE_USER_MASK_NESTING_KEYWORD3, "Keywords 3"}, {SCE_USER_MASK_NESTING_KEYWORD4, "Keywords 4"},
	{SCE_USER_MASK_NESTING_KEYWORD5, "Keywords 5"}, {SCE_USER_MASK_NESTING_KEYWORD6, "Keywords 6"},
	{SCE_USER_MASK_NESTING_KEYWORD7, "Keywords 7"}, {SCE_USER_MASK_NESTING_KEYWORD8, "Keywords 8"},
	{SCE_USER_MASK_NESTING_FOLDERS_IN_CODE2_OPEN, "Code 2 open"}, {SCE_USER_MASK_NESTING_FOLDERS_IN_CODE2_MIDDLE, "Code 2 middle"},
	{SCE_USER_MASK_NESTING_FOLDERS_IN_CODE2_CLOSE, "Code 2 close"}, {SCE_USER_MASK_NESTING_FOLDERS_IN_COMMENT_OPEN, "Comment open"},
	{SCE_USER_MASK_NESTING_FOLDERS_IN_COMMENT_MIDDLE, "Comment middle"}, {SCE_USER_MASK_NESTING_FOLDERS_IN_COMMENT_CLOSE, "Comment close"},
	{SCE_USER_MASK_NESTING_OPERATORS1, "Operators 1"}, {SCE_USER_MASK_NESTING_OPERATORS2, "Operators 2"},
	{SCE_USER_MASK_NESTING_NUMBERS, "Numbers"},
};

const int kNestableStyles[] = {
	SCE_USER_STYLE_COMMENT, SCE_USER_STYLE_COMMENTLINE,
	SCE_USER_STYLE_DELIMITER1, SCE_USER_STYLE_DELIMITER2, SCE_USER_STYLE_DELIMITER3, SCE_USER_STYLE_DELIMITER4,
	SCE_USER_STYLE_DELIMITER5, SCE_USER_STYLE_DELIMITER6, SCE_USER_STYLE_DELIMITER7, SCE_USER_STYLE_DELIMITER8,
};

const char *const kCommentGroupKeys[5] = {"commentLineOpen", "commentLineContinue", "commentLineClose", "commentOpen", "commentClose"};
const char *const kCommentGroupLabels[5] = {"Line comment open", "Line comment continue", "Line comment close", "Comment open", "Comment close"};

std::string styleValue(const UserStyle &style) {
	return ((style.colorStyle & 1) ? style.fg : std::string()) + "," + ((style.colorStyle & 2) ? style.bg : std::string()) + "," + std::to_string(style.fontStyle);
}

void applyStyleValue(const std::string &value, UserStyle &style) {
	const size_t comma = value.find(',');
	const size_t second = comma == std::string::npos ? comma : value.find(',', comma + 1);
	if (second == std::string::npos) return;
	const std::string fg = value.substr(0, comma);
	const std::string bg = value.substr(comma + 1, second - comma - 1);
	style.colorStyle = (fg.empty() ? 0 : 1) | (bg.empty() ? 0 : 2);
	// A colour that's switched off stays stored, like Notepad++ keeps it.
	if (!fg.empty()) style.fg = fg;
	if (!bg.empty()) style.bg = bg;
	style.fontStyle = std::atoi(value.c_str() + second + 1) & 7;
}

}  // namespace

std::vector<FormField> userLanguageFields(const UserLanguage &language) {
	std::vector<FormField> fields;
	const auto add = [&](FormField::Kind kind, const std::string &key, const std::string &label, const std::string &value = std::string()) {
		FormField field;
		field.kind = kind;
		field.key = key;
		field.label = label;
		field.value = value;
		fields.push_back(field);
	};
	const auto page = [&](const char *label) { add(FormField::Kind::Page, "", label); };
	const auto heading = [&](const std::string &label) { add(FormField::Kind::Heading, "", label); };
	const auto toggle = [&](const std::string &key, const std::string &label, bool on) { add(FormField::Kind::Bool, key, label, on ? "1" : "0"); };
	const auto text = [&](const std::string &key, const std::string &label, const std::string &value) { add(FormField::Kind::Text, key, label, value); };
	const auto longText = [&](const std::string &key, const std::string &label, const std::string &value) { add(FormField::Kind::LongText, key, label, value); };
	const auto choice = [&](const std::string &key, const std::string &label, int value, std::vector<std::pair<std::string, std::string>> choices) {
		add(FormField::Kind::Choice, key, label, std::to_string(value));
		fields.back().choices = std::move(choices);
	};
	const auto list = [&](int id) { return "list" + std::to_string(id); };

	page("Language");
	text("name", "Name", language.name);
	text("extensions", "File extensions (like: foo bar)", language.extensions);
	toggle("caseIgnored", "Ignore case in keywords", language.caseIgnored);
	heading("Folding");
	toggle("foldCompact", "Fold blank lines with the block above", language.foldCompact);
	toggle("allowFoldOfComments", "Allow folding of comments", language.allowFoldOfComments);
	longText(list(SCE_USER_KWLIST_FOLDERS_IN_CODE1_OPEN), "Code 1 open", language.keywords[SCE_USER_KWLIST_FOLDERS_IN_CODE1_OPEN]);
	longText(list(SCE_USER_KWLIST_FOLDERS_IN_CODE1_MIDDLE), "Code 1 middle", language.keywords[SCE_USER_KWLIST_FOLDERS_IN_CODE1_MIDDLE]);
	longText(list(SCE_USER_KWLIST_FOLDERS_IN_CODE1_CLOSE), "Code 1 close", language.keywords[SCE_USER_KWLIST_FOLDERS_IN_CODE1_CLOSE]);
	longText(list(SCE_USER_KWLIST_FOLDERS_IN_CODE2_OPEN), "Code 2 open", language.keywords[SCE_USER_KWLIST_FOLDERS_IN_CODE2_OPEN]);
	longText(list(SCE_USER_KWLIST_FOLDERS_IN_CODE2_MIDDLE), "Code 2 middle", language.keywords[SCE_USER_KWLIST_FOLDERS_IN_CODE2_MIDDLE]);
	longText(list(SCE_USER_KWLIST_FOLDERS_IN_CODE2_CLOSE), "Code 2 close", language.keywords[SCE_USER_KWLIST_FOLDERS_IN_CODE2_CLOSE]);
	longText(list(SCE_USER_KWLIST_FOLDERS_IN_COMMENT_OPEN), "Comment open", language.keywords[SCE_USER_KWLIST_FOLDERS_IN_COMMENT_OPEN]);
	longText(list(SCE_USER_KWLIST_FOLDERS_IN_COMMENT_MIDDLE), "Comment middle", language.keywords[SCE_USER_KWLIST_FOLDERS_IN_COMMENT_MIDDLE]);
	longText(list(SCE_USER_KWLIST_FOLDERS_IN_COMMENT_CLOSE), "Comment close", language.keywords[SCE_USER_KWLIST_FOLDERS_IN_COMMENT_CLOSE]);

	page("Keywords");
	for (int i = 0; i < kUserKeywordGroups; i++) {
		const std::string number = std::to_string(i + 1);
		longText(list(SCE_USER_KWLIST_KEYWORDS1 + i), "Keywords " + number, language.keywords[SCE_USER_KWLIST_KEYWORDS1 + i]);
		toggle("prefix" + number, "Match keywords " + number + " as prefixes", language.prefix[i]);
	}

	page("Comments and Numbers");
	heading("Comments");
	const std::vector<std::string> comments = userGroups(language.keywords[SCE_USER_KWLIST_COMMENTS], 5);
	for (int i = 0; i < 5; i++) text(kCommentGroupKeys[i], kCommentGroupLabels[i], comments[i]);
	choice("forcePureLC", "Line comments are allowed", language.forcePureLC,
	       {{"0", "Anywhere"}, {"1", "Only at the start of a line"}, {"2", "Only after whitespace"}});
	heading("Numbers");
	text(list(SCE_USER_KWLIST_NUMBER_PREFIX1), "Prefix 1", language.keywords[SCE_USER_KWLIST_NUMBER_PREFIX1]);
	text(list(SCE_USER_KWLIST_NUMBER_PREFIX2), "Prefix 2", language.keywords[SCE_USER_KWLIST_NUMBER_PREFIX2]);
	text(list(SCE_USER_KWLIST_NUMBER_EXTRAS1), "Extras 1", language.keywords[SCE_USER_KWLIST_NUMBER_EXTRAS1]);
	text(list(SCE_USER_KWLIST_NUMBER_EXTRAS2), "Extras 2", language.keywords[SCE_USER_KWLIST_NUMBER_EXTRAS2]);
	text(list(SCE_USER_KWLIST_NUMBER_SUFFIX1), "Suffix 1", language.keywords[SCE_USER_KWLIST_NUMBER_SUFFIX1]);
	text(list(SCE_USER_KWLIST_NUMBER_SUFFIX2), "Suffix 2", language.keywords[SCE_USER_KWLIST_NUMBER_SUFFIX2]);
	text(list(SCE_USER_KWLIST_NUMBER_RANGE), "Range", language.keywords[SCE_USER_KWLIST_NUMBER_RANGE]);
	choice("decimalSeparator", "Decimal separator", language.decimalSeparator, {{"0", "Dot"}, {"1", "Comma"}, {"2", "Both"}});

	page("Operators and Delimiters");
	heading("Operators");
	longText(list(SCE_USER_KWLIST_OPERATORS1), "Operators 1", language.keywords[SCE_USER_KWLIST_OPERATORS1]);
	longText(list(SCE_USER_KWLIST_OPERATORS2), "Operators 2 (need a space around them)", language.keywords[SCE_USER_KWLIST_OPERATORS2]);
	const std::vector<std::string> delimiters = userGroups(language.keywords[SCE_USER_KWLIST_DELIMITERS], 24);
	for (int i = 0; i < 8; i++) {
		const std::string number = std::to_string(i + 1);
		heading("Delimiter " + number);
		text("delimiter" + std::to_string(i * 3), "Open", delimiters[i * 3]);
		text("delimiter" + std::to_string(i * 3 + 1), "Escape", delimiters[i * 3 + 1]);
		text("delimiter" + std::to_string(i * 3 + 2), "Close", delimiters[i * 3 + 2]);
	}

	page("Styles");
	for (int i = 0; i < kUserStyles; i++) add(FormField::Kind::StyleRow, "style" + std::to_string(i), kStyleLabels[i], styleValue(language.styles[i]));

	page("Nesting");
	std::vector<std::pair<std::string, std::string>> flags;
	for (const NestingFlag &flag : kNestingFlags) flags.emplace_back(std::to_string(flag.mask), flag.label);
	for (const int id : kNestableStyles) {
		add(FormField::Kind::Flags, "nesting" + std::to_string(id), std::string(kStyleLabels[id]) + " can contain", std::to_string(language.styles[id].nesting));
		fields.back().choices = flags;
	}
	return fields;
}

bool applyUserLanguageFields(const std::vector<FormField> &fields, UserLanguage &language) {
	std::vector<std::string> comments(5), delimiters(24);
	for (const FormField &field : fields) {
		const std::string &key = field.key;
		const bool on = field.value == "1";
		const auto number = [&](const char *prefix) { return std::atoi(key.c_str() + std::strlen(prefix)); };
		const auto starts = [&](const char *prefix) { return key.rfind(prefix, 0) == 0; };
		if (key == "name") language.name = field.value;
		else if (key == "extensions") language.extensions = field.value;
		else if (key == "caseIgnored") language.caseIgnored = on;
		else if (key == "foldCompact") language.foldCompact = on;
		else if (key == "allowFoldOfComments") language.allowFoldOfComments = on;
		else if (key == "forcePureLC") language.forcePureLC = std::atoi(field.value.c_str());
		else if (key == "decimalSeparator") language.decimalSeparator = std::atoi(field.value.c_str());
		else if (starts("prefix")) {
			const int group = number("prefix") - 1;
			if (group >= 0 && group < kUserKeywordGroups) language.prefix[group] = on;
		} else if (starts("list")) {
			const int list = number("list");
			if (list >= 0 && list < kUserKeywordLists) language.keywords[list] = field.value;
		} else if (starts("delimiter")) {
			const int group = number("delimiter");
			if (group >= 0 && group < 24) delimiters[group] = field.value;
		} else if (starts("style")) {
			const int style = number("style");
			if (style >= 0 && style < kUserStyles) applyStyleValue(field.value, language.styles[style]);
		} else if (starts("nesting")) {
			const int style = number("nesting");
			if (style >= 0 && style < kUserStyles) language.styles[style].nesting = std::atoi(field.value.c_str());
		} else {
			for (int i = 0; i < 5; i++)
				if (key == kCommentGroupKeys[i]) comments[i] = field.value;
		}
	}
	language.keywords[SCE_USER_KWLIST_COMMENTS] = encodeUserGroups(comments);
	language.keywords[SCE_USER_KWLIST_DELIMITERS] = encodeUserGroups(delimiters);
	return !language.name.empty();
}

std::string userLanguageFileName(const std::string &name) {
	std::string out;
	for (const char c : name) {
		const bool safe = isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
		out += safe ? c : '_';
	}
	if (out.empty()) out = "language";
	return out + ".udl.xml";
}

}  // namespace omapad
