// What Workspace.cpp and Features.cpp share. Nothing outside the core should need this.
#pragma once
#include "Workspace.h"
#include "Scintilla.h"
#include "Version.h"
#include "json.hpp"
#include "pugixml.hpp"

#include <memory>
#include <string>
#include <vector>

namespace omapad {

using nlohmann::json;

constexpr int kMarginNumber = 0;
constexpr int kMarginBookmark = 1;
constexpr int kMarginFold = 2;
constexpr int kMarginHistory = 3;
constexpr int kMarkerBookmark = 20;  // Change history uses markers 21 to 24.
constexpr int kIndicatorMap = 19;    // The Document Map's visible range. Only the map draws it.
constexpr int kIndicatorSmart = 30;
constexpr int kIndicatorMark = 31;

constexpr int kIndicatorLink = 18, kIndicatorTag = 17, kIndicatorTagAttribute = 16, kIndicatorIncremental = 15;
constexpr int kMarkerHiddenBegin = 18, kMarkerHiddenEnd = 19;  // Hidden lines, shown as arrows in the margin.

std::string baseName(const std::string &path);
std::string directoryOf(const std::string &path);
std::string expandPath(const std::string &raw);
// Writes via a temporary file and rename. `mode` -1 keeps the existing file's permissions.
bool atomicWrite(const std::string &bytes, const std::string &rawPath, int mode, std::string &errorOut);

inline std::string jsonString(const json &j, const char *key, const std::string &fallback = std::string()) {
	const auto found = j.find(key);
	return (found != j.end() && found->is_string()) ? found->get<std::string>() : fallback;
}

inline long jsonLong(const json &j, const char *key, long fallback = 0) {
	const auto found = j.find(key);
	return (found != j.end() && found->is_number_integer()) ? found->get<long>() : fallback;
}

inline bool jsonBool(const json &j, const char *key, bool fallback = false) {
	const auto found = j.find(key);
	return (found != j.end() && found->is_boolean()) ? found->get<bool>() : fallback;
}

inline std::vector<std::string> jsonStrings(const json &j, const char *key) {
	std::vector<std::string> out;
	const auto found = j.find(key);
	if (found != j.end() && found->is_array())
		for (const json &value : *found)
			if (value.is_string()) out.push_back(value.get<std::string>());
	return out;
}

struct Document {
	intptr_t sciDoc = 0;
	std::string path, untitledName;
	const Encoding *encoding = nullptr;
	Eol eol = Eol::LF;
	const Language *language = nullptr;
	bool languageChosen = false;
	bool modified = false;    // Differs from Scintilla's save point.
	bool forceDirty = false;  // Unsaved for another reason, like an encoding change.
	bool hasMtime = false;
	int64_t mtime = 0;
	bool missing = false;
	std::string backupId;
	unsigned long editGeneration = 0, backupGeneration = 0;
	bool monitoring = false;  // Follows the file like tail -f.
	bool rightToLeft = false;
	bool dirty() const { return modified || forceDirty; }
	std::string title() const { return path.empty() ? untitledName : baseName(path); }
};

struct Tab {
	std::shared_ptr<Document> document;
	long anchor = 0, caret = 0, firstLine = 0, xOffset = 0;
	std::vector<long> folds;
	bool pinned = false;
	int colour = 0;  // 1 to 5, or 0 for none.
	std::vector<std::pair<long, long>> hiddenLines;  // First and last line of each hidden run.
};

struct Pane {
	std::vector<Tab> tabs;
	int active = -1;
	int lineDigits = 0;
	bool smartActive = false;
};

// Points the hidden view at a document for as long as this lives. This reads and changes
// documents without disturbing what either visible pane is showing.
struct BackgroundDocument {
	SciView &view;
	BackgroundDocument(Host &host, const Document &document) : view(host.backgroundView()) {
		view.send(SCI_SETDOCPOINTER, 0, document.sciDoc);
	}
	~BackgroundDocument() { view.send(SCI_SETDOCPOINTER, 0, 0); }
};

struct ResultEntry {
	std::string path;
	std::weak_ptr<Document> document;
	long line = -1, offset = 0, length = 0;
};

}  // namespace omapad
