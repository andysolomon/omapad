#include "FunctionList.h"

#include "Regex.h"
#include "pugixml.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>

namespace fs = std::filesystem;

namespace omapad {

namespace {

struct Rx {
	std::unique_ptr<Regex> regex;
	bool ok() const { return regex && regex->valid(); }
};

// Notepad++ runs these rules with . matching line endings and ^ $ matching at each line.
Rx compile(const char *pattern) {
	Rx out;
	if (!pattern || !*pattern) return out;
	Regex::Options options;
	options.dotMatchesNewline = true;
	out.regex = std::make_unique<Regex>(pattern, options);
	return out;
}

struct FunctionRule {
	Rx main;
	std::vector<Rx> names;
	std::vector<Rx> classNames;
};

// Narrows `start`..`end` by running each name rule in turn, like Notepad++'s parser.
std::string extractName(const std::vector<Rx> &rules, const std::string &text, size_t start, size_t end) {
	if (rules.empty()) return text.substr(start, end - start);
	size_t from = start, to = end;
	for (const Rx &rule : rules) {
		if (!rule.ok()) continue;
		Regex::Match match;
		// Each rule sees only what the previous one found.
		if (rule.regex->search(text.data(), to, from, match) != Regex::Result::Found) return std::string();
		from = match.start;
		to = match.end;
	}
	std::string name = text.substr(from, to - from);
	std::replace_if(name.begin(), name.end(), [](char c) { return c == '\n' || c == '\r' || c == '\t'; }, ' ');
	const size_t first = name.find_first_not_of(' ');
	if (first == std::string::npos) return std::string();
	return name.substr(first, name.find_last_not_of(' ') - first + 1);
}

// A hopelessly long scan shouldn't stall typing.
const size_t kMaxMatches = 20000;

}  // namespace

struct FunctionListCatalog::Rules {
	Rx comments;
	Rx classMain;
	std::string openSymbol, closeSymbol;
	std::vector<Rx> classNames;
	std::vector<FunctionRule> classFunctions;
	std::vector<FunctionRule> functions;
};

namespace {

void forEachMatch(const Rx &rule, const std::string &text, size_t start, size_t end, const std::vector<std::pair<size_t, size_t>> &skip,
                  const std::function<void(const Regex::Match &)> &visit) {
	if (!rule.ok()) return;
	size_t position = start, count = 0;
	Regex::Match match;
	while (position <= end && count++ < kMaxMatches) {
		if (rule.regex->search(text.data(), end, position, match) != Regex::Result::Found) break;
		bool inComment = false;
		for (const auto &zone : skip) {
			if (match.start >= zone.first && match.start < zone.second) {
				inComment = true;
				// Search again after the comment, since a match inside one hides what follows.
				position = zone.second;
				break;
			}
			if (zone.first > match.start) break;
		}
		if (inComment) continue;
		visit(match);
		position = match.end > position ? match.end : position + 1;
	}
}

// The end of the block opened after `from`, by counting open and close symbols outside comments.
size_t blockEnd(const std::string &text, size_t from, size_t limit, const Rx &open, const Rx &close, const std::vector<std::pair<size_t, size_t>> &skip) {
	if (!open.ok() || !close.ok()) return limit;
	const auto inComment = [&](size_t position) {
		for (const auto &zone : skip) {
			if (position >= zone.first && position < zone.second) return true;
			if (zone.first > position) break;
		}
		return false;
	};
	Regex::Match openMatch, closeMatch;
	size_t position = from;
	int depth = 0;
	bool opened = false;
	while (position <= limit) {
		const bool hasOpen = open.regex->search(text.data(), limit, position, openMatch) == Regex::Result::Found;
		const bool hasClose = close.regex->search(text.data(), limit, position, closeMatch) == Regex::Result::Found;
		if (!hasOpen && !hasClose) return limit;
		if (hasOpen && (!hasClose || openMatch.start < closeMatch.start)) {
			if (!inComment(openMatch.start)) {
				depth++;
				opened = true;
			}
			position = openMatch.end > position ? openMatch.end : position + 1;
		} else {
			if (!inComment(closeMatch.start) && opened && --depth == 0) return closeMatch.end;
			position = closeMatch.end > position ? closeMatch.end : position + 1;
		}
	}
	return limit;
}

FunctionRule readFunctionRule(const pugi::xml_node &node) {
	FunctionRule rule;
	rule.main = compile(node.attribute("mainExpr").as_string());
	for (const pugi::xml_node nameNode : node.child("functionName").children()) rule.names.push_back(compile(nameNode.attribute("expr").as_string()));
	for (const pugi::xml_node nameNode : node.child("className").children()) rule.classNames.push_back(compile(nameNode.attribute("expr").as_string()));
	return rule;
}

}  // namespace

const FunctionListCatalog::Rules *FunctionListCatalog::rulesFor(const std::string &languageName) const {
	const auto found = loaded.find(languageName);
	if (found != loaded.end()) return found->second.get();
	std::shared_ptr<Rules> rules;
	const std::string file = fs::path(languageName).filename().string();
	pugi::xml_document doc;
	// Keep the attributes' line breaks. These patterns use (?x), where a # comment runs to the
	// end of its line, so flattening them to spaces would comment out the rest of the pattern.
	if (!file.empty() && doc.load_file((fs::path(directory) / "functionList" / (file + ".xml")).c_str(), pugi::parse_default & ~pugi::parse_wconv_attribute)) {
		const pugi::xml_node parser = doc.first_element_by_path("NotepadPlus/functionList/parser");
		if (parser) {
			rules = std::make_shared<Rules>();
			rules->comments = compile(parser.attribute("commentExpr").as_string());
			const pugi::xml_node classRange = parser.child("classRange");
			if (classRange) {
				rules->classMain = compile(classRange.attribute("mainExpr").as_string());
				rules->openSymbol = classRange.attribute("openSymbole").as_string();
				rules->closeSymbol = classRange.attribute("closeSymbole").as_string();
				for (const pugi::xml_node nameNode : classRange.child("className").children()) rules->classNames.push_back(compile(nameNode.attribute("expr").as_string()));
				for (const pugi::xml_node node : classRange.children("function")) rules->classFunctions.push_back(readFunctionRule(node));
			}
			for (const pugi::xml_node node : parser.children("function")) rules->functions.push_back(readFunctionRule(node));
		}
	}
	const Rules *result = rules.get();
	loaded[languageName] = std::move(rules);
	return result;
}

bool FunctionListCatalog::supports(const std::string &languageName) const { return rulesFor(languageName) != nullptr; }

std::vector<FunctionEntry> FunctionListCatalog::parse(const std::string &languageName, const std::string &text) const {
	std::vector<FunctionEntry> out;
	const Rules *rules = rulesFor(languageName);
	if (!rules) return out;

	std::vector<std::pair<size_t, size_t>> comments;
	forEachMatch(rules->comments, text, 0, text.size(), {}, [&](const Regex::Match &match) {
		if (match.end > match.start) comments.emplace_back(match.start, match.end);
	});

	std::vector<std::pair<size_t, size_t>> classZones;
	const Rx open = compile(rules->openSymbol.c_str());
	const Rx close = compile(rules->closeSymbol.c_str());
	forEachMatch(rules->classMain, text, 0, text.size(), comments, [&](const Regex::Match &match) {
		const size_t end = (open.ok() && close.ok()) ? blockEnd(text, match.start, text.size(), open, close, comments) : match.end;
		std::string className = extractName(rules->classNames, text, match.start, std::min(match.end, end));
		classZones.emplace_back(match.start, end);
		for (const FunctionRule &rule : rules->classFunctions) {
			forEachMatch(rule.main, text, match.start, end, comments, [&](const Regex::Match &inner) {
				FunctionEntry entry;
				entry.name = extractName(rule.names, text, inner.start, inner.end);
				entry.position = inner.start;
				entry.className = className;
				if (!entry.name.empty()) out.push_back(std::move(entry));
			});
		}
	});

	for (const FunctionRule &rule : rules->functions) {
		forEachMatch(rule.main, text, 0, text.size(), comments, [&](const Regex::Match &match) {
			for (const auto &zone : classZones)
				if (match.start >= zone.first && match.start < zone.second) return;
			FunctionEntry entry;
			entry.name = extractName(rule.names, text, match.start, match.end);
			entry.position = match.start;
			if (!rule.classNames.empty()) entry.className = extractName(rule.classNames, text, match.start, match.end);
			if (!entry.name.empty()) out.push_back(std::move(entry));
		});
	}
	std::stable_sort(out.begin(), out.end(), [](const FunctionEntry &a, const FunctionEntry &b) { return a.position < b.position; });
	return out;
}

}  // namespace omapad
