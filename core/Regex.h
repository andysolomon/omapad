// Regular expressions through PCRE2: the same syntax on every platform, with multi-line
// patterns, lookbehind and \K, and no recursion, so a long line can't overflow the stack.
#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace omapad {

class Regex {
public:
	struct Options {
		bool ignoreCase = false;
		bool dotMatchesNewline = false;
		bool wholeWord = false;
	};
	struct Match {
		size_t start = 0, end = 0;
		// Byte ranges of each group. Group 0 is the whole match, and an unmatched group is
		// marked by first > second.
		std::vector<std::pair<size_t, size_t>> groups;
	};
	enum class Result { Found, NotFound, Failed };

	Regex(const std::string &pattern, const Options &options);
	~Regex();
	Regex(const Regex &) = delete;
	Regex &operator=(const Regex &) = delete;

	bool valid() const { return code != nullptr; }
	const std::string &error() const { return errorText; }
	// Searches from `offset`, with the whole subject visible so lookbehind and ^ see what's
	// before it. Failed means the search gave up, usually by hitting its step limit.
	Result search(const char *subject, size_t length, size_t offset, Match &match) const;

private:
	void *code = nullptr;
	void *matchData = nullptr;
	void *matchContext = nullptr;
	std::string errorText;
};

// Expands \1, $1, ${1}, \n, \r, \t and \\ in a replacement. \U, \L and \E change case like Boost.
std::string expandReplacement(const std::string &replacement, const char *subject, const Regex::Match &match);

}  // namespace omapad
