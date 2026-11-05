// Finds a document's classes and functions using Notepad++'s parser rule files.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace omapad {

struct FunctionEntry {
	std::string name;
	size_t position = 0;   // Byte offset in the document.
	std::string className;  // Empty for a top-level function.
};

class FunctionListCatalog {
public:
	explicit FunctionListCatalog(const std::string &resourceDirectory) : directory(resourceDirectory) {}
	bool supports(const std::string &languageName) const;
	std::vector<FunctionEntry> parse(const std::string &languageName, const std::string &text) const;
	// Sorted in document order. The class order is the order of first appearance.

private:
	struct Rules;
	const Rules *rulesFor(const std::string &languageName) const;
	std::string directory;
	mutable std::map<std::string, std::shared_ptr<Rules>> loaded;
};

}  // namespace omapad
