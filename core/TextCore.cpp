#include "TextCore.h"

#include "CaseConvert.h"
#include "CharacterCategoryMap.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iconv.h>
#include <random>
#include <unordered_set>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <glib.h>
#endif

using Scintilla::Internal::CaseConversion;
using Scintilla::Internal::CaseConvert;
using Scintilla::Internal::CaseConvertString;
using Scintilla::Internal::CategoriseCharacter;

namespace omapad {

const char *eolString(Eol eol) {
	switch (eol) {
	case Eol::CRLF: return "\r\n";
	case Eol::CR: return "\r";
	case Eol::LF: return "\n";
	}
	return "\n";
}

const char *eolName(Eol eol) {
	switch (eol) {
	case Eol::CRLF: return "Windows (CR LF)";
	case Eol::CR: return "Macintosh (CR)";
	case Eol::LF: return "Unix (LF)";
	}
	return "Unix (LF)";
}

Eol detectEol(const std::string &text, Eol fallback) {
	for (size_t i = 0; i < text.size(); i++) {
		if (text[i] == '\n')
			return Eol::LF;
		if (text[i] == '\r')
			return (i + 1 < text.size() && text[i + 1] == '\n') ? Eol::CRLF : Eol::CR;
	}
	return fallback;
}

void appendUtf8(std::string &out, unsigned long cp) {
	if (cp < 0x80) {
		out += static_cast<char>(cp);
	} else if (cp < 0x800) {
		out += static_cast<char>(0xC0 | (cp >> 6));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		out += static_cast<char>(0xE0 | (cp >> 12));
		out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	} else {
		out += static_cast<char>(0xF0 | (cp >> 18));
		out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
		out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	}
}

std::u32string utf8ToCodePoints(const std::string &text) {
	std::u32string out;
	out.reserve(text.size());
	const unsigned char *s = reinterpret_cast<const unsigned char *>(text.data());
	const size_t n = text.size();
	size_t i = 0;
	while (i < n) {
		const unsigned char c = s[i];
		size_t extra = 0;
		char32_t cp = c;
		if (c >= 0xC2 && c <= 0xDF) { extra = 1; cp = c & 0x1F; }
		else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
		else if (c >= 0xF0 && c <= 0xF4) { extra = 3; cp = c & 0x07; }
		else if (c >= 0x80) { out += U'\xFFFD'; i++; continue; }
		if (i + extra >= n && extra) { out += U'\xFFFD'; i++; continue; }
		bool ok = true;
		for (size_t j = 1; j <= extra; j++) {
			if ((s[i + j] & 0xC0) != 0x80) { ok = false; break; }
			cp = (cp << 6) | (s[i + j] & 0x3F);
		}
		if (!ok) { out += U'\xFFFD'; i++; continue; }
		out += cp;
		i += extra + 1;
	}
	return out;
}

char32_t foldCodePoint(char32_t cp) {
	const char *folded = CaseConvert(static_cast<int>(cp), CaseConversion::fold);
	if (!folded) return cp;
	const std::u32string decoded = utf8ToCodePoints(folded);
	return decoded.size() == 1 ? decoded[0] : cp;
}

bool isWordCodePoint(char32_t cp) {
	switch (CategoriseCharacter(static_cast<int>(cp))) {
	case Scintilla::Internal::ccLu: case Scintilla::Internal::ccLl: case Scintilla::Internal::ccLt:
	case Scintilla::Internal::ccLm: case Scintilla::Internal::ccLo: case Scintilla::Internal::ccMn:
	case Scintilla::Internal::ccMc: case Scintilla::Internal::ccMe: case Scintilla::Internal::ccNd:
	case Scintilla::Internal::ccNl: case Scintilla::Internal::ccNo:
		return true;
	default:
		return false;
	}
}

namespace {

// Splits on any line ending. `trailing` reports whether the text ended with one.
std::vector<std::string> splitLines(const std::string &text, bool &trailing) {
	std::vector<std::string> lines;
	size_t start = 0;
	trailing = false;
	for (size_t i = 0; i < text.size(); i++) {
		const char c = text[i];
		if (c != '\n' && c != '\r')
			continue;
		lines.push_back(text.substr(start, i - start));
		if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
			i++;
		start = i + 1;
		if (start == text.size())
			trailing = true;
	}
	if (start < text.size())
		lines.push_back(text.substr(start));
	return lines;
}

std::string joinLines(const std::vector<std::string> &lines, Eol eol, bool trailing) {
	const char *sep = eolString(eol);
	std::string out;
	for (size_t i = 0; i < lines.size(); i++) {
		if (i)
			out += sep;
		out += lines[i];
	}
	if (trailing && !lines.empty())
		out += sep;
	return out;
}

bool isBlankChar(char c) { return c == ' ' || c == '\t'; }

std::string trimLeft(const std::string &s) {
	size_t i = 0;
	while (i < s.size() && isBlankChar(s[i])) i++;
	return s.substr(i);
}

std::string trimRight(const std::string &s) {
	size_t e = s.size();
	while (e > 0 && isBlankChar(s[e - 1])) e--;
	return s.substr(0, e);
}

size_t utf8Length(const std::string &s) {
	size_t n = 0;
	for (unsigned char c : s)
		if ((c & 0xC0) != 0x80) n++;
	return n;
}

// Sorts numerically. Lines with no leading number go last, in their original order.
template <typename Parse>
void sortNumeric(std::vector<std::string> &lines, bool ascending, Parse parse) {
	struct Keyed { bool ok; double value; size_t index; };
	std::vector<Keyed> keys;
	keys.reserve(lines.size());
	for (size_t i = 0; i < lines.size(); i++) {
		double v = 0;
		const bool ok = parse(lines[i], v);
		keys.push_back({ok, v, i});
	}
	std::stable_sort(keys.begin(), keys.end(), [&](const Keyed &a, const Keyed &b) {
		if (a.ok != b.ok) return a.ok;
		if (!a.ok) return false;
		return ascending ? a.value < b.value : a.value > b.value;
	});
	std::vector<std::string> out;
	out.reserve(lines.size());
	for (const Keyed &k : keys) out.push_back(std::move(lines[k.index]));
	lines = std::move(out);
}

bool parseInteger(const std::string &line, double &value) {
	const std::string t = trimLeft(line);
	if (t.empty()) return false;
	char *end = nullptr;
	errno = 0;
	const long long v = std::strtoll(t.c_str(), &end, 10);
	if (end == t.c_str() || errno == ERANGE) return false;
	value = static_cast<double>(v);
	return true;
}

bool parseDecimal(const std::string &line, double &value) {
	const std::string t = trimLeft(line);
	if (t.empty()) return false;
	char *end = nullptr;
	value = std::strtod(t.c_str(), &end);
	return end != t.c_str();
}

// Strict conversion. Any input the target can't hold exactly fails.
bool iconvConvert(const char *from, const char *to, const char *input, size_t length, std::string &out) {
	out.clear();
	if (length == 0) return true;
	iconv_t cd = iconv_open(to, from);
	if (cd == reinterpret_cast<iconv_t>(-1)) return false;
	char *in = const_cast<char *>(input);
	size_t inLeft = length;
	std::string buffer(length * 2 + 64, '\0');
	size_t written = 0;
	bool ok = true;
	for (;;) {
		char *outPtr = &buffer[written];
		size_t outLeft = buffer.size() - written;
		// A null input flushes any state once all bytes have been consumed.
		const size_t result = iconv(cd, inLeft ? &in : nullptr, inLeft ? &inLeft : nullptr, &outPtr, &outLeft);
		written = buffer.size() - outLeft;
		if (result == static_cast<size_t>(-1)) {
			if (errno == E2BIG) {
				buffer.resize(buffer.size() * 2);
				continue;
			}
			ok = false;
			break;
		}
		// A positive count means characters were converted inexactly.
		if (result > 0 && inLeft == 0) {
			ok = false;
			break;
		}
		if (inLeft == 0) break;
	}
	iconv_close(cd);
	if (ok) {
		buffer.resize(written);
		out = std::move(buffer);
	}
	return ok;
}

}  // namespace

std::string convertEol(const std::string &text, Eol eol) {
	const char *sep = eolString(eol);
	std::string out;
	out.reserve(text.size());
	for (size_t i = 0; i < text.size(); i++) {
		const char c = text[i];
		if (c == '\r') {
			if (i + 1 < text.size() && text[i + 1] == '\n') i++;
			out += sep;
		} else if (c == '\n') {
			out += sep;
		} else {
			out += c;
		}
	}
	return out;
}

const std::vector<Encoding> &allEncodings() {
	static const std::vector<Encoding> list = {
		{"utf8", "UTF-8", "UTF-8", false},
		{"utf8bom", "UTF-8-BOM", "UTF-8", true},
		{"utf16le", "UTF-16 LE BOM", "UTF-16LE", true},
		{"utf16be", "UTF-16 BE BOM", "UTF-16BE", true},
		{"cp1252", "ANSI (Windows-1252)", "CP1252", false},
		{"latin1", "ISO-8859-1 (Latin-1)", "ISO-8859-1", false},
		{"latin2", "ISO-8859-2 (Central European)", "ISO-8859-2", false},
		{"cp1250", "Windows-1250 (Central European)", "CP1250", false},
		{"cp1251", "Windows-1251 (Cyrillic)", "CP1251", false},
		{"koi8r", "KOI8-R (Cyrillic)", "KOI8-R", false},
		{"shiftjis", "Shift-JIS (Japanese)", "SHIFT_JIS", false},
		{"eucjp", "EUC-JP (Japanese)", "EUC-JP", false},
		{"gb18030", "GB18030 (Simplified Chinese)", "GB18030", false},
		{"big5", "Big5 (Traditional Chinese)", "BIG5", false},
		{"euckr", "EUC-KR (Korean)", "EUC-KR", false},
	};
	return list;
}

const Encoding *encodingWithIdentifier(const std::string &identifier) {
	for (const Encoding &e : allEncodings())
		if (identifier == e.identifier) return &e;
	return nullptr;
}

bool isUtf8Encoding(const Encoding *encoding) { return std::strcmp(encoding->iconvName, "UTF-8") == 0; }

bool isValidUtf8(const char *bytes, size_t length) {
	const unsigned char *s = reinterpret_cast<const unsigned char *>(bytes);
	size_t i = 0;
	while (i < length) {
		const unsigned char c = s[i];
		size_t extra;
		unsigned long cp;
		if (c < 0x80) { i++; continue; }
		if (c >= 0xC2 && c <= 0xDF) { extra = 1; cp = c & 0x1F; }
		else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
		else if (c >= 0xF0 && c <= 0xF4) { extra = 3; cp = c & 0x07; }
		else return false;
		if (i + extra >= length) return false;
		for (size_t j = 1; j <= extra; j++) {
			if ((s[i + j] & 0xC0) != 0x80) return false;
			cp = (cp << 6) | (s[i + j] & 0x3F);
		}
		if ((extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000)) return false;  // Overlong.
		if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;  // Out of range, surrogates.
		i += extra + 1;
	}
	return true;
}

bool decodeAs(const std::string &data, const Encoding *encoding, std::string &utf8Out) {
	const char *bytes = data.data();
	size_t length = data.size();
	const unsigned char *u = reinterpret_cast<const unsigned char *>(bytes);
	if (encoding->bom) {
		const bool isUtf8 = isUtf8Encoding(encoding);
		const bool le = std::strcmp(encoding->iconvName, "UTF-16LE") == 0;
		const bool be = std::strcmp(encoding->iconvName, "UTF-16BE") == 0;
		if (isUtf8 && length >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF) {
			bytes += 3; length -= 3;
		} else if (le && length >= 2 && u[0] == 0xFF && u[1] == 0xFE) {
			bytes += 2; length -= 2;
		} else if (be && length >= 2 && u[0] == 0xFE && u[1] == 0xFF) {
			bytes += 2; length -= 2;
		}
	}
	if (isUtf8Encoding(encoding)) {
		if (!isValidUtf8(bytes, length)) return false;
		utf8Out.assign(bytes, length);
		return true;
	}
	return iconvConvert(encoding->iconvName, "UTF-8", bytes, length, utf8Out);
}

Decoded decodeAuto(const std::string &data) {
	Decoded out;
	const unsigned char *u = reinterpret_cast<const unsigned char *>(data.data());
	const size_t n = data.size();
	const char *id = nullptr;
	if (n >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF) id = "utf8bom";
	else if (n >= 2 && u[0] == 0xFF && u[1] == 0xFE) id = "utf16le";
	else if (n >= 2 && u[0] == 0xFE && u[1] == 0xFF) id = "utf16be";
	if (id) {
		const Encoding *e = encodingWithIdentifier(id);
		if (decodeAs(data, e, out.utf8)) {
			out.encoding = e;
			return out;
		}
	}
	for (const char *candidate : {"utf8", "cp1252", "latin1"}) {
		const Encoding *e = encodingWithIdentifier(candidate);
		if (!decodeAs(data, e, out.utf8)) continue;
		// Only accept a legacy encoding if writing it back gives the original bytes.
		if (std::strcmp(candidate, "cp1252") == 0) {
			std::string again;
			if (!encode(out.utf8, e, again) || again != data) continue;
		}
		out.encoding = e;
		return out;
	}
	// Reached only if the system's iconv lacks Latin-1. Keep the bytes rather than lose them.
	out.utf8.clear();
	for (unsigned char c : data) appendUtf8(out.utf8, c);
	out.encoding = encodingWithIdentifier("latin1");
	return out;
}

bool encode(const std::string &utf8, const Encoding *encoding, std::string &out) {
	out.clear();
	std::string prefix;
	if (encoding->bom) {
		if (isUtf8Encoding(encoding)) prefix = "\xEF\xBB\xBF";
		else if (std::strcmp(encoding->iconvName, "UTF-16LE") == 0) prefix = "\xFF\xFE";
		else if (std::strcmp(encoding->iconvName, "UTF-16BE") == 0) prefix = "\xFE\xFF";
	}
	if (isUtf8Encoding(encoding)) {
		out = prefix + utf8;
		return true;
	}
	if (!isValidUtf8(utf8.data(), utf8.size())) return false;
	std::string body;
	if (!iconvConvert("UTF-8", encoding->iconvName, utf8.data(), utf8.size(), body)) return false;
	out = prefix + body;
	return true;
}

std::string applyLineOp(const std::string &text, LineOp op, Eol eol) {
	bool trailing = false;
	std::vector<std::string> lines = splitLines(text, trailing);
	switch (op) {
	case LineOp::SortLexAsc:
		std::stable_sort(lines.begin(), lines.end());
		break;
	case LineOp::SortLexDesc:
		std::stable_sort(lines.begin(), lines.end(), std::greater<std::string>());
		break;
	case LineOp::SortFoldAsc:
	case LineOp::SortFoldDesc: {
		const bool ascending = op == LineOp::SortFoldAsc;
		std::vector<std::pair<std::string, size_t>> keys;
		keys.reserve(lines.size());
		for (size_t i = 0; i < lines.size(); i++)
			keys.emplace_back(CaseConvertString(lines[i], CaseConversion::fold), i);
		std::stable_sort(keys.begin(), keys.end(), [&](const auto &a, const auto &b) {
			return ascending ? a.first < b.first : a.first > b.first;
		});
		std::vector<std::string> out;
		out.reserve(lines.size());
		for (const auto &k : keys) out.push_back(std::move(lines[k.second]));
		lines = std::move(out);
		break;
	}
	case LineOp::SortIntAsc: sortNumeric(lines, true, parseInteger); break;
	case LineOp::SortIntDesc: sortNumeric(lines, false, parseInteger); break;
	case LineOp::SortDecimalAsc: sortNumeric(lines, true, parseDecimal); break;
	case LineOp::SortDecimalDesc: sortNumeric(lines, false, parseDecimal); break;
	case LineOp::SortLengthAsc:
		std::stable_sort(lines.begin(), lines.end(), [](const std::string &a, const std::string &b) {
			return utf8Length(a) < utf8Length(b);
		});
		break;
	case LineOp::SortLengthDesc:
		std::stable_sort(lines.begin(), lines.end(), [](const std::string &a, const std::string &b) {
			return utf8Length(a) > utf8Length(b);
		});
		break;
	case LineOp::Reverse:
		std::reverse(lines.begin(), lines.end());
		break;
	case LineOp::Randomize: {
		std::random_device device;
		std::mt19937 generator(device());
		std::shuffle(lines.begin(), lines.end(), generator);
		break;
	}
	case LineOp::RemoveDuplicates: {
		std::unordered_set<std::string> seen;
		std::vector<std::string> out;
		for (auto &line : lines)
			if (seen.insert(line).second) out.push_back(std::move(line));
		lines = std::move(out);
		break;
	}
	case LineOp::RemoveConsecutiveDuplicates:
		lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
		break;
	case LineOp::RemoveEmpty:
		lines.erase(std::remove_if(lines.begin(), lines.end(), [](const std::string &l) { return l.empty(); }), lines.end());
		break;
	case LineOp::RemoveBlank:
		lines.erase(std::remove_if(lines.begin(), lines.end(), [](const std::string &l) { return trimLeft(l).empty(); }), lines.end());
		break;
	case LineOp::TrimTrailing:
		for (auto &line : lines) line = trimRight(line);
		break;
	case LineOp::TrimLeading:
		for (auto &line : lines) line = trimLeft(line);
		break;
	case LineOp::TrimBoth:
		for (auto &line : lines) line = trimRight(trimLeft(line));
		break;
	}
	return joinLines(lines, eol, trailing);
}

std::string joinLinesWithSpace(const std::string &text) {
	bool trailing = false;
	const std::vector<std::string> lines = splitLines(text, trailing);
	std::string out;
	for (size_t i = 0; i < lines.size(); i++) {
		if (i) out += ' ';
		out += lines[i];
	}
	return out;
}

std::string tabsToSpaces(const std::string &text, int tabWidth) {
	if (tabWidth < 1) tabWidth = 1;
	std::string out;
	size_t column = 0;
	for (size_t i = 0; i < text.size(); i++) {
		const unsigned char c = static_cast<unsigned char>(text[i]);
		if (c == '\t') {
			const size_t pad = tabWidth - (column % tabWidth);
			out.append(pad, ' ');
			column += pad;
		} else {
			out += static_cast<char>(c);
			if (c == '\n' || c == '\r') column = 0;
			else if ((c & 0xC0) != 0x80) column++;
		}
	}
	return out;
}

std::string spacesToTabs(const std::string &text, int tabWidth, bool leadingOnly) {
	if (tabWidth < 2) return text;
	std::string out;
	size_t i = 0;
	size_t column = 0;
	bool leading = true;
	while (i < text.size()) {
		const unsigned char c = static_cast<unsigned char>(text[i]);
		if (c == '\n' || c == '\r') {
			out += static_cast<char>(c);
			column = 0;
			leading = true;
			i++;
		} else if (c == ' ' && (leading || !leadingOnly)) {
			size_t run = 0;
			while (i + run < text.size() && text[i + run] == ' ') run++;
			// Emit a tab only for spaces that reach a tab stop. The rest stay spaces.
			size_t pos = column;
			const size_t endPos = column + run;
			while (pos < endPos) {
				const size_t stop = pos + (tabWidth - pos % tabWidth);
				if (stop <= endPos) {
					// A single space ending on a stop stays a space when it isn't indentation.
					if (stop - pos == 1 && !leading) out += ' ';
					else out += '\t';
					pos = stop;
				} else {
					out.append(endPos - pos, ' ');
					pos = endPos;
				}
			}
			column = endPos;
			i += run;
		} else if (c == '\t') {
			out += '\t';
			column += tabWidth - column % tabWidth;
			i++;
		} else {
			out += static_cast<char>(c);
			if ((c & 0xC0) != 0x80) column++;
			leading = false;
			i++;
		}
	}
	return out;
}

std::string convertCase(const std::string &text, CaseOp op) {
	if (op == CaseOp::Upper) return CaseConvertString(text, CaseConversion::upper);
	if (op == CaseOp::Lower) return CaseConvertString(text, CaseConversion::lower);

	const auto upper = [](std::string &out, char32_t cp) {
		const char *converted = CaseConvert(static_cast<int>(cp), CaseConversion::upper);
		if (converted) out += converted;
		else appendUtf8(out, cp);
	};
	const auto lower = [](std::string &out, char32_t cp) {
		const char *converted = CaseConvert(static_cast<int>(cp), CaseConversion::lower);
		if (converted) out += converted;
		else appendUtf8(out, cp);
	};
	const auto isLetter = [](char32_t cp) {
		const auto category = CategoriseCharacter(static_cast<int>(cp));
		return category <= Scintilla::Internal::ccLo;
	};
	std::random_device device;
	std::mt19937 generator(device());
	std::string out;
	out.reserve(text.size());
	bool wordStart = true;
	bool sentenceStart = true;
	for (char32_t cp : utf8ToCodePoints(text)) {
		const bool letter = isLetter(cp);
		const bool word = isWordCodePoint(cp);
		// An apostrophe inside a word doesn't start a new one ("don't" becomes "Don't").
		const bool joined = (cp == U'\'' || cp == U'’') && !wordStart;
		switch (op) {
		case CaseOp::Proper:
			if (letter && wordStart) upper(out, cp); else lower(out, cp);
			break;
		case CaseOp::ProperBlend:
			if (letter && wordStart) upper(out, cp); else appendUtf8(out, cp);
			break;
		case CaseOp::Sentence:
			if (letter && sentenceStart) upper(out, cp); else lower(out, cp);
			break;
		case CaseOp::SentenceBlend:
			if (letter && sentenceStart) upper(out, cp); else appendUtf8(out, cp);
			break;
		case CaseOp::Invert:
			if (CategoriseCharacter(static_cast<int>(cp)) == Scintilla::Internal::ccLu) lower(out, cp); else upper(out, cp);
			break;
		case CaseOp::Random:
			if (generator() & 1) upper(out, cp); else lower(out, cp);
			break;
		default:
			appendUtf8(out, cp);
			break;
		}
		wordStart = !(word || joined);
		if (word) sentenceStart = false;
		else if (cp == U'.' || cp == U'!' || cp == U'?' || cp == U'\n' || cp == U'\r') sentenceStart = true;
	}
	return out;
}

std::string unescapeExtended(const std::string &text) {
	std::string out;
	const auto parseDigits = [&](size_t start, size_t count, int base, unsigned long &value) {
		if (start + count > text.size()) return false;
		const std::string digits = text.substr(start, count);
		for (char c : digits) {
			const bool ok = base == 16 ? std::isxdigit(static_cast<unsigned char>(c)) != 0
			                           : (c >= '0' && c < '0' + base);
			if (!ok) return false;
		}
		value = std::strtoul(digits.c_str(), nullptr, base);
		return true;
	};
	for (size_t i = 0; i < text.size(); i++) {
		if (text[i] != '\\' || i + 1 >= text.size()) {
			out += text[i];
			continue;
		}
		const char c = text[i + 1];
		unsigned long value = 0;
		switch (c) {
		case 'n': out += '\n'; i++; break;
		case 'r': out += '\r'; i++; break;
		case 't': out += '\t'; i++; break;
		case 'a': out += '\a'; i++; break;
		case 'f': out += '\f'; i++; break;
		case 'v': out += '\v'; i++; break;
		case '0': out += '\0'; i++; break;
		case '\\': out += '\\'; i++; break;
		case 'b':
			if (parseDigits(i + 2, 8, 2, value) && value < 256) { appendUtf8(out, value); i += 9; }
			else { out += '\b'; i++; }
			break;
		case 'x':
			if (parseDigits(i + 2, 2, 16, value)) { appendUtf8(out, value); i += 3; }
			else out += '\\';
			break;
		case 'd':
			if (parseDigits(i + 2, 3, 10, value) && value < 256) { appendUtf8(out, value); i += 4; }
			else out += '\\';
			break;
		case 'o':
			if (parseDigits(i + 2, 3, 8, value) && value < 256) { appendUtf8(out, value); i += 4; }
			else out += '\\';
			break;
		case 'u':
			if (parseDigits(i + 2, 4, 16, value)) { appendUtf8(out, value); i += 5; }
			else out += '\\';
			break;
		default:
			out += '\\';
			break;
		}
	}
	return out;
}

std::string normalizeRegexReplacement(const std::string &text) {
	std::string out;
	for (size_t i = 0; i < text.size(); i++) {
		const char c = text[i];
		if (c == '\\' && i + 1 < text.size()) {
			if (text[i + 1] == '$') {
				out += '$';
			} else {
				out += c;
				out += text[i + 1];
			}
			i++;
		} else if (c == '$' && i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1]))) {
			out += '\\';
		} else {
			out += c;
		}
	}
	return out;
}

std::vector<std::string> commentLines(const std::vector<std::string> &lines, const std::string &token, CommentMode mode) {
	const auto indentOf = [](const std::string &line) {
		size_t i = 0;
		while (i < line.size() && isBlankChar(line[i])) i++;
		return i;
	};
	const auto isBlankLine = [&](const std::string &line) { return indentOf(line) == line.size(); };
	const auto isCommented = [&](const std::string &line) {
		return line.compare(indentOf(line), token.size(), token) == 0;
	};
	bool uncomment = mode == CommentMode::Uncomment;
	if (mode == CommentMode::Toggle) {
		bool anyCode = false;
		bool allCommented = true;
		for (const std::string &line : lines) {
			if (isBlankLine(line)) continue;
			anyCode = true;
			if (!isCommented(line)) allCommented = false;
		}
		uncomment = anyCode && allCommented;
	}
	std::vector<std::string> out;
	out.reserve(lines.size());
	for (const std::string &line : lines) {
		if (isBlankLine(line)) {
			out.push_back(line);
			continue;
		}
		const size_t indent = indentOf(line);
		if (uncomment) {
			if (!isCommented(line)) {
				out.push_back(line);
				continue;
			}
			size_t after = indent + token.size();
			if (after < line.size() && line[after] == ' ') after++;
			out.push_back(line.substr(0, indent) + line.substr(after));
		} else {
			out.push_back(line.substr(0, indent) + token + " " + line.substr(indent));
		}
	}
	return out;
}

std::string hashHex(const void *bytes, size_t length, HashKind kind) {
	static const char *hex = "0123456789abcdef";
	std::string out;
#ifdef __APPLE__
	unsigned char digest[CC_SHA512_DIGEST_LENGTH];
	size_t size = 0;
	switch (kind) {
	case HashKind::MD5: CC_MD5(bytes, static_cast<CC_LONG>(length), digest); size = CC_MD5_DIGEST_LENGTH; break;
	case HashKind::SHA1: CC_SHA1(bytes, static_cast<CC_LONG>(length), digest); size = CC_SHA1_DIGEST_LENGTH; break;
	case HashKind::SHA256: CC_SHA256(bytes, static_cast<CC_LONG>(length), digest); size = CC_SHA256_DIGEST_LENGTH; break;
	case HashKind::SHA512: CC_SHA512(bytes, static_cast<CC_LONG>(length), digest); size = CC_SHA512_DIGEST_LENGTH; break;
	}
	for (size_t i = 0; i < size; i++) {
		out += hex[digest[i] >> 4];
		out += hex[digest[i] & 0x0F];
	}
#else
	(void)hex;
	GChecksumType type = G_CHECKSUM_MD5;
	switch (kind) {
	case HashKind::MD5: type = G_CHECKSUM_MD5; break;
	case HashKind::SHA1: type = G_CHECKSUM_SHA1; break;
	case HashKind::SHA256: type = G_CHECKSUM_SHA256; break;
	case HashKind::SHA512: type = G_CHECKSUM_SHA512; break;
	}
	char *digest = g_compute_checksum_for_data(type, static_cast<const guchar *>(bytes), length);
	out = digest;
	g_free(digest);
#endif
	return out;
}

}  // namespace omapad
