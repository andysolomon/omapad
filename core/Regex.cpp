#define PCRE2_CODE_UNIT_WIDTH 8
#include "Regex.h"

#include "TextCore.h"
#include "pcre2.h"

#include <cctype>

namespace omapad {

Regex::Regex(const std::string &pattern, const Options &options) {
	uint32_t flags = PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_INVALID_UTF | PCRE2_MULTILINE;
	if (options.ignoreCase) flags |= PCRE2_CASELESS;
	if (options.dotMatchesNewline) flags |= PCRE2_DOTALL;
	const std::string source = options.wholeWord ? "\\b(?:" + pattern + ")\\b" : pattern;
	pcre2_compile_context *compileContext = pcre2_compile_context_create(nullptr);
	pcre2_set_newline(compileContext, PCRE2_NEWLINE_ANYCRLF);
	// Notepad++'s parser files use \K inside lookarounds, which PCRE2 only allows on request.
	pcre2_set_compile_extra_options(compileContext, PCRE2_EXTRA_ALLOW_LOOKAROUND_BSK);
	int errorCode = 0;
	PCRE2_SIZE errorOffset = 0;
	pcre2_code *compiled = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(source.data()), source.size(), flags, &errorCode, &errorOffset, compileContext);
	pcre2_compile_context_free(compileContext);
	if (!compiled) {
		PCRE2_UCHAR buffer[256];
		pcre2_get_error_message(errorCode, buffer, sizeof(buffer));
		errorText = reinterpret_cast<const char *>(buffer);
		return;
	}
	code = compiled;
	matchData = pcre2_match_data_create_from_pattern(compiled, nullptr);
	pcre2_match_context *context = pcre2_match_context_create(nullptr);
	// A catastrophic pattern gives up rather than freezing the app.
	pcre2_set_match_limit(context, 200000000);
	pcre2_set_heap_limit(context, 1024 * 1024);  // In KiB, so long lines can still match.
	matchContext = context;
}

Regex::~Regex() {
	pcre2_match_context_free(static_cast<pcre2_match_context *>(matchContext));
	pcre2_match_data_free(static_cast<pcre2_match_data *>(matchData));
	pcre2_code_free(static_cast<pcre2_code *>(code));
}

Regex::Result Regex::search(const char *subject, size_t length, size_t offset, Match &match) const {
	if (!code || offset > length) return Result::NotFound;
	auto *data = static_cast<pcre2_match_data *>(matchData);
	const int rc = pcre2_match(static_cast<pcre2_code *>(code), reinterpret_cast<PCRE2_SPTR>(subject), length, offset, 0, data,
	                           static_cast<pcre2_match_context *>(matchContext));
	if (rc == PCRE2_ERROR_NOMATCH) return Result::NotFound;
	if (rc < 0) return Result::Failed;
	const PCRE2_SIZE *vector = pcre2_get_ovector_pointer(data);
	const uint32_t count = pcre2_get_ovector_count(data);
	match.groups.clear();
	for (uint32_t i = 0; i < count; i++) match.groups.emplace_back(vector[i * 2], vector[i * 2 + 1]);
	match.start = vector[0];
	match.end = vector[1];
	// \K in a lookbehind can put the start after the end. Treat that as an empty match.
	if (match.start > match.end) match.start = match.end;
	return Result::Found;
}

std::string expandReplacement(const std::string &replacement, const char *subject, const Regex::Match &match) {
	enum class Case { None, Upper, Lower } mode = Case::None;
	std::string out;
	const auto append = [&](const std::string &text) {
		if (mode == Case::Upper) out += convertCase(text, CaseOp::Upper);
		else if (mode == Case::Lower) out += convertCase(text, CaseOp::Lower);
		else out += text;
	};
	const auto group = [&](size_t index) {
		if (index >= match.groups.size()) return;
		const auto &range = match.groups[index];
		if (range.first == static_cast<size_t>(-1) || range.first > range.second) return;
		append(std::string(subject + range.first, range.second - range.first));
	};
	for (size_t i = 0; i < replacement.size(); i++) {
		const char c = replacement[i];
		const char next = i + 1 < replacement.size() ? replacement[i + 1] : '\0';
		if (c == '\\' && next) {
			i++;
			if (std::isdigit(static_cast<unsigned char>(next))) group(static_cast<size_t>(next - '0'));
			else if (next == 'n') append("\n");
			else if (next == 'r') append("\r");
			else if (next == 't') append("\t");
			else if (next == 'U') mode = Case::Upper;
			else if (next == 'L') mode = Case::Lower;
			else if (next == 'E') mode = Case::None;
			else append(std::string(1, next));
		} else if (c == '$' && std::isdigit(static_cast<unsigned char>(next))) {
			i++;
			group(static_cast<size_t>(next - '0'));
		} else if (c == '$' && next == '{') {
			const size_t close = replacement.find('}', i + 2);
			const std::string digits = close == std::string::npos ? std::string() : replacement.substr(i + 2, close - i - 2);
			bool numeric = !digits.empty();
			for (char d : digits) numeric = numeric && std::isdigit(static_cast<unsigned char>(d));
			if (numeric && digits.size() < 4) {
				group(static_cast<size_t>(std::stoul(digits)));
				i = close;
			} else {
				append("$");
			}
		} else {
			append(std::string(1, c));
		}
	}
	return out;
}

}  // namespace omapad
