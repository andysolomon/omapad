// Pure text logic with no UI or operating system toolkit dependency.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace omapad {

enum class Eol { CRLF = 0, CR = 1, LF = 2 };  // Values match SC_EOL_*.
const char *eolString(Eol eol);
const char *eolName(Eol eol);
// The first line ending decides, as in most editors. No line ending yields `fallback`.
Eol detectEol(const std::string &text, Eol fallback = Eol::LF);
std::string convertEol(const std::string &text, Eol eol);

struct Encoding {
	const char *identifier;  // Stable identifier, stored in sessions.
	const char *name;        // Display name.
	const char *iconvName;
	bool bom;
	const char *group = "";          // The Character Sets submenu it belongs in. Empty for the main list.
	const char16_t *table = nullptr; // Set for OEM code pages, which not every system's iconv has.
};
const std::vector<Encoding> &allEncodings();
const Encoding *encodingWithIdentifier(const std::string &identifier);
bool isUtf8Encoding(const Encoding *encoding);

struct Decoded {
	std::string utf8;
	const Encoding *encoding = nullptr;
};
// Detects the encoding from a BOM, then valid UTF-8, then falls back to Windows-1252
// or Latin-1. Latin-1 maps every byte, so an unknown file always round-trips exactly.
Decoded decodeAuto(const std::string &bytes);
// Reads bytes as one specific encoding. Returns false if the bytes aren't valid for it.
bool decodeAs(const std::string &bytes, const Encoding *encoding, std::string &utf8Out);
// Returns false if any character can't be represented. Callers must never silently lose text.
bool encode(const std::string &utf8, const Encoding *encoding, std::string &bytesOut);
bool isValidUtf8(const char *bytes, size_t length);
// What a statistical detector makes of bytes that aren't Unicode. Null if it has no useful answer.
const Encoding *detectLegacyEncoding(const std::string &bytes);
// What an XML declaration or HTML meta tag says the file is. Null if it says nothing.
const Encoding *declaredEncoding(const std::string &bytes);

enum class LineOp {
	SortLexAsc, SortLexDesc, SortFoldAsc, SortFoldDesc,
	SortIntAsc, SortIntDesc, SortDecimalAsc, SortDecimalDesc, SortDecimalCommaAsc, SortDecimalCommaDesc,
	SortLengthAsc, SortLengthDesc,
	Reverse, Randomize,
	RemoveDuplicates, RemoveConsecutiveDuplicates,
	RemoveEmpty, RemoveBlank,
	TrimTrailing, TrimLeading, TrimBoth,
	SortLocaleAsc, SortLocaleDesc,
};
// Applies `op` to the lines of `text`. A final line ending stays final rather than
// being treated as an empty line, so sorting never moves it to the top.
std::string applyLineOp(const std::string &text, LineOp op, Eol eol);
std::string joinLinesWithSpace(const std::string &text);
std::string tabsToSpaces(const std::string &text, int tabWidth);
std::string spacesToTabs(const std::string &text, int tabWidth, bool leadingOnly);

enum class CaseOp { Upper, Lower, Proper, ProperBlend, Sentence, SentenceBlend, Invert, Random };
std::string convertCase(const std::string &text, CaseOp op);

// Turns \n \r \t \0 \\ \a \b \f \v \xHH \dNNN \oNNN \uNNNN into characters for Extended
// search mode. Invalid or incomplete sequences stay literal.
std::string unescapeExtended(const std::string &text);

// Formats a time with Notepad++'s date tokens: yyyy MM MMM MMMM dd ddd dddd HH hh mm ss tt.
std::string formatDateTime(const std::string &format, long long secondsSinceEpoch);
std::string percentEncode(const std::string &text);

// Comments or uncomments lines. If every non-blank line is already commented it
// uncomments them, otherwise it comments them. Blank lines are left alone.
enum class CommentMode { Toggle, Comment, Uncomment };
std::vector<std::string> commentLines(const std::vector<std::string> &lines, const std::string &token, CommentMode mode);

enum class HashKind { MD5, SHA1, SHA256, SHA512 };
std::string hashHex(const void *bytes, size_t length, HashKind kind);

// Puts a shortcut in the one form stored and compared: Mod, Ctrl, Alt, Shift, then the key.
std::string normalizeShortcut(const std::string &shortcut);
// A number for the Column Editor. `base` is 2, 8, 10 or 16, and `width` pads with zeros.
std::string formatNumber(long value, int base, size_t width);

// UTF-8 helpers shared with Find in Files. Invalid bytes decode to U+FFFD.
std::u32string utf8ToCodePoints(const std::string &text);
void appendUtf8(std::string &out, unsigned long codePoint);
char32_t foldCodePoint(char32_t codePoint);
bool isWordCodePoint(char32_t codePoint);

}  // namespace omapad
