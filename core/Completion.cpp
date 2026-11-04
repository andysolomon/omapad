#include "Completion.h"

#include "TextCore.h"
#include "pugixml.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>

namespace fs = std::filesystem;

namespace omapad {

namespace {

std::string lowercase(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

std::string counterPrefix(size_t index, size_t count) {
	if (count < 2) return std::string();
	return "\001 " + std::to_string(index + 1) + " of " + std::to_string(count) + " \002 ";
}

std::string signaturePrefix(const ApiKeyword &keyword, const ApiOverload &overload, const std::string &start, size_t index, size_t count) {
	std::string out = counterPrefix(index, count);
	if (!overload.returnValue.empty()) out += overload.returnValue + " ";
	return out + keyword.name + start;
}

}  // namespace

const ApiKeyword *LanguageApi::find(const std::string &name) const {
	for (const ApiKeyword &keyword : keywords) {
		if (keyword.name == name || (ignoreCase && lowercase(keyword.name) == lowercase(name))) return &keyword;
	}
	return nullptr;
}

std::vector<std::string> LanguageApi::matching(const std::string &prefix, bool functionsOnly) const {
	std::vector<std::string> out;
	const std::string wanted = ignoreCase ? lowercase(prefix) : prefix;
	for (const ApiKeyword &keyword : keywords) {
		if (functionsOnly && !keyword.isFunction) continue;
		const std::string name = ignoreCase ? lowercase(keyword.name) : keyword.name;
		if (name.compare(0, wanted.size(), wanted) == 0) out.push_back(keyword.name);
	}
	return out;
}

const LanguageApi *CompletionCatalog::forLanguage(const std::string &languageName) const {
	const auto found = loaded.find(languageName);
	if (found != loaded.end()) return found->second.get();
	static const std::map<std::string, std::string> aliases = {
		{"javascript.js", "javascript"}, {"coffeescript", "coffee"}, {"baanc", "BaanC"}, {"bash", ""},
	};
	const auto alias = aliases.find(languageName);
	// Names come from the language table, but never let one leave the folder.
	const std::string file = fs::path(alias != aliases.end() ? alias->second : languageName).filename().string();
	std::unique_ptr<LanguageApi> api;
	pugi::xml_document doc;
	if (!file.empty() && doc.load_file((fs::path(directory) / "autoCompletion" / (file + ".xml")).c_str())) {
		const pugi::xml_node root = doc.first_element_by_path("NotepadPlus/AutoComplete");
		api = std::make_unique<LanguageApi>();
		const pugi::xml_node environment = root.child("Environment");
		api->ignoreCase = std::string(environment.attribute("ignoreCase").as_string("no")) == "yes";
		api->startFunc = environment.attribute("startFunc").as_string("(");
		api->stopFunc = environment.attribute("stopFunc").as_string(")");
		api->paramSeparator = environment.attribute("paramSeparator").as_string(",");
		api->terminal = environment.attribute("terminal").as_string(";");
		api->additionalWordChars = environment.attribute("additionalWordChar").as_string("");
		for (const pugi::xml_node node : root.children("KeyWord")) {
			ApiKeyword keyword;
			keyword.name = node.attribute("name").as_string();
			if (keyword.name.empty()) continue;
			keyword.isFunction = std::string(node.attribute("func").as_string("no")) == "yes";
			for (const pugi::xml_node overloadNode : node.children("Overload")) {
				ApiOverload overload;
				overload.returnValue = overloadNode.attribute("retVal").as_string();
				overload.description = overloadNode.attribute("descr").as_string();
				for (const pugi::xml_node parameter : overloadNode.children("Param")) overload.parameters.push_back(parameter.attribute("name").as_string());
				keyword.overloads.push_back(std::move(overload));
			}
			api->keywords.push_back(std::move(keyword));
		}
		const bool folded = api->ignoreCase;
		std::stable_sort(api->keywords.begin(), api->keywords.end(), [folded](const ApiKeyword &a, const ApiKeyword &b) {
			return folded ? lowercase(a.name) < lowercase(b.name) : a.name < b.name;
		});
	}
	const LanguageApi *result = api.get();
	loaded[languageName] = std::move(api);
	return result;
}

std::vector<std::string> documentWords(const std::string &text, const std::string &prefix, bool ignoreCase, size_t excludeAt,
                                       const std::string &additional, size_t limit) {
	const std::string wanted = ignoreCase ? lowercase(prefix) : prefix;
	std::set<std::string> words;
	const auto isWord = [&](unsigned char c) {
		return std::isalnum(c) || c == '_' || c >= 0x80 || additional.find(static_cast<char>(c)) != std::string::npos;
	};
	size_t i = 0;
	while (i < text.size() && words.size() < limit) {
		if (!isWord(static_cast<unsigned char>(text[i]))) {
			i++;
			continue;
		}
		size_t end = i;
		while (end < text.size() && isWord(static_cast<unsigned char>(text[end]))) end++;
		const bool covers = excludeAt >= i && excludeAt <= end;
		if (!covers && end - i > wanted.size() && end - i < 256) {
			const std::string word = text.substr(i, end - i);
			const std::string compared = ignoreCase ? lowercase(word) : word;
			if (compared.compare(0, wanted.size(), wanted) == 0) words.insert(word);
		}
		i = end;
	}
	return std::vector<std::string>(words.begin(), words.end());
}

std::string formatHint(const ApiKeyword &keyword, const ApiOverload &overload, const std::string &start, const std::string &stop,
                       const std::string &separator, size_t index, size_t count) {
	std::string out = signaturePrefix(keyword, overload, start, index, count);
	for (size_t i = 0; i < overload.parameters.size(); i++) {
		if (i) out += separator + " ";
		out += overload.parameters[i];
	}
	out += stop;
	if (!overload.description.empty()) out += "\n" + overload.description;
	return out;
}

std::pair<size_t, size_t> hintHighlight(const ApiKeyword &keyword, const ApiOverload &overload, const std::string &start,
                                        const std::string &separator, size_t index, size_t count, size_t parameter) {
	size_t position = signaturePrefix(keyword, overload, start, index, count).size();
	for (size_t i = 0; i < overload.parameters.size(); i++) {
		if (i == parameter) return {position, position + overload.parameters[i].size()};
		position += overload.parameters[i].size() + separator.size() + 1;
	}
	return {0, 0};
}

}  // namespace omapad
