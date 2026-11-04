// Auto-completion data: each language's API file, read the first time it's needed.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace omapad {

struct ApiOverload {
	std::string returnValue;
	std::string description;
	std::vector<std::string> parameters;
};

struct ApiKeyword {
	std::string name;
	bool isFunction = false;
	std::vector<ApiOverload> overloads;
};

struct LanguageApi {
	bool ignoreCase = false;
	std::string startFunc = "(", stopFunc = ")", paramSeparator = ",", terminal = ";", additionalWordChars;
	std::vector<ApiKeyword> keywords;  // Sorted by name, ignoring case when the language does.
	const ApiKeyword *find(const std::string &name) const;
	// Names starting with `prefix`, in order. Functions only, if asked.
	std::vector<std::string> matching(const std::string &prefix, bool functionsOnly) const;
};

class CompletionCatalog {
public:
	explicit CompletionCatalog(const std::string &resourceDirectory) : directory(resourceDirectory) {}
	// Null if the language has no API file.
	const LanguageApi *forLanguage(const std::string &languageName) const;

private:
	std::string directory;
	mutable std::map<std::string, std::unique_ptr<LanguageApi>> loaded;
};

// The distinct words in `text` that start with `prefix`, without the word at `excludeAt`.
std::vector<std::string> documentWords(const std::string &text, const std::string &prefix, bool ignoreCase, size_t excludeAt,
                                       const std::string &additionalWordChars = std::string(), size_t limit = 500);

// The text of a function hint, like "int max(int a, int b)", with a description below.
std::string formatHint(const ApiKeyword &keyword, const ApiOverload &overload, const std::string &start, const std::string &stop,
                       const std::string &separator, size_t overloadIndex, size_t overloadCount);
// Where the `parameter`th parameter sits in that text.
std::pair<size_t, size_t> hintHighlight(const ApiKeyword &keyword, const ApiOverload &overload, const std::string &start,
                                        const std::string &separator, size_t overloadIndex, size_t overloadCount, size_t parameter);

}  // namespace omapad
