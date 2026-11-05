#include "Notes.h"
#include "TextCore.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <sstream>

namespace omapad {
namespace {
std::string trim(const std::string &s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
std::string folded(const std::string &s) {
    std::string out;
    for (char32_t cp : utf8ToCodePoints(s)) appendUtf8(out, foldCodePoint(cp));
    return out;
}
std::string shortText(const std::string &s, size_t limit) {
    if (s.size() <= limit) return s;
    while (limit && (static_cast<unsigned char>(s[limit]) & 0xc0) == 0x80) --limit;
    return s.substr(0, limit) + "…";
}
std::vector<NoteSpan> inlineText(const std::string &s, unsigned inherited = 0, int depth = 0) {
    std::vector<NoteSpan> out;
    std::string plain;
    const auto flush = [&]() { if (!plain.empty()) { out.push_back({plain, inherited, {}}); plain.clear(); } };
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '\\' && i + 1 < s.size()) { plain += s[i + 1]; i += 2; continue; }
        if (depth < 8 && s[i] == '`') {
            const auto end = s.find('`', i + 1);
            if (end != std::string::npos) { flush(); out.push_back({s.substr(i + 1, end - i - 1), inherited | 4, {}}); i = end + 1; continue; }
        }
        if (depth < 8 && (s[i] == '*' || s[i] == '_')) {
            const size_t width = i + 1 < s.size() && s[i + 1] == s[i] ? 2 : 1;
            const std::string marker(width, s[i]);
            const auto end = s.find(marker, i + width);
            const bool boundary = s[i] != '_' || i == 0 || !std::isalnum(static_cast<unsigned char>(s[i - 1]));
            if (boundary && end != std::string::npos && end > i + width) {
                flush(); auto nested = inlineText(s.substr(i + width, end - i - width), inherited | (width == 2 ? 1 : 2), depth + 1);
                out.insert(out.end(), nested.begin(), nested.end()); i = end + width; continue;
            }
        }
        if (depth < 8 && s[i] == '[') {
            const auto mid = s.find("](", i + 1);
            const auto end = mid == std::string::npos ? mid : s.find(')', mid + 2);
            if (end != std::string::npos) {
                flush(); const std::string href = s.substr(mid + 2, end - mid - 2);
                const bool safe = href.rfind("https://", 0) == 0 || href.rfind("http://", 0) == 0;
                out.push_back({s.substr(i + 1, mid - i - 1), inherited | (safe ? 8u : 0u), safe ? href : std::string()});
                i = end + 1; continue;
            }
        }
        plain += s[i++];
    }
    flush(); return out;
}
std::string plainText(const std::string &s) {
    std::string out; for (const auto &span : inlineText(s)) out += span.text; return out;
}
}

NoteSummary describeNote(const std::string &path, const std::string &text, int64_t modified) {
    NoteSummary note; note.path = path; note.content = text; note.modified = modified;
    std::istringstream lines(text.compare(0, 3, "\xef\xbb\xbf") == 0 ? text.substr(3) : text); std::string line, fence; std::set<std::string> tags;
    bool haveTitle = false;
    while (std::getline(lines, line)) {
        std::string clean = trim(line);
        if (clean.rfind("```", 0) == 0 || clean.rfind("~~~", 0) == 0) {
            const std::string mark = clean.substr(0, 3);
            if (fence.empty()) fence = mark; else if (fence == mark) fence.clear();
            continue;
        }
        if (!fence.empty()) continue;
        if (!clean.empty()) {
            if (!haveTitle) {
                const auto first = clean.find_first_not_of('#');
                if (first != std::string::npos && first <= 6 && first > 0 && clean[first] == ' ') clean = trim(clean.substr(first));
                note.title = shortText(plainText(clean), 120); haveTitle = !note.title.empty();
            } else if (note.preview.empty()) note.preview = shortText(plainText(clean), 180);
        }
        bool code = false;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '\\') { if (i + 1 < line.size()) ++i; continue; }
            if (line[i] == '`') { code = !code; continue; }
            if (code || line[i] != '#' || (i && !std::isspace(static_cast<unsigned char>(line[i - 1])) && line[i - 1] != '(')) continue;
            size_t end = i + 1;
            while (end < line.size()) {
                const unsigned char c = line[end];
                if (!(std::isalnum(c) || c >= 128 || c == '_' || c == '-' || c == '/')) break;
                ++end;
            }
            if (end > i + 1) {
                std::string tag = line.substr(i + 1, end - i - 1);
                while (!tag.empty() && tag.back() == '/') tag.pop_back();
                if (!tag.empty() && tag.front() != '/') tags.insert(folded(tag));
                i = end - 1;
            }
        }
    }
    if (note.title.empty()) note.title = "Untitled note";
    note.tags.assign(tags.begin(), tags.end());
    return note;
}

bool noteMatches(const NoteSummary &note, const std::string &query, const std::string &tag) {
    const std::string wanted = folded(tag);
    if (!wanted.empty() && std::none_of(note.tags.begin(), note.tags.end(), [&](const std::string &t) { return t == wanted || t.rfind(wanted + "/", 0) == 0; })) return false;
    const std::string haystack = folded(note.title + "\n" + note.content);
    std::istringstream terms(folded(query)); std::string term;
    while (terms >> term) if (haystack.find(term) == std::string::npos) return false;
    return true;
}

size_t noteWordCount(const std::string &text) {
    size_t count = 0; bool word = false;
    for (char32_t cp : utf8ToCodePoints(text)) {
        const bool current = cp > 127 || std::isalnum(static_cast<unsigned char>(cp));
        if (current && !word) ++count;
        word = current;
    }
    return count;
}

std::vector<NoteBlock> renderNote(const std::string &text) {
    std::vector<NoteBlock> blocks;
    std::istringstream input(text.compare(0, 3, "\xef\xbb\xbf") == 0 ? text.substr(3) : text); std::string line, fence, code, paragraph;
    const auto flush = [&]() { if (!paragraph.empty()) { NoteBlock b; b.spans = inlineText(paragraph); blocks.push_back(std::move(b)); paragraph.clear(); } };
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string t = trim(line);
        if (!fence.empty()) {
            if (t.rfind(fence, 0) == 0) { NoteBlock b; b.kind = NoteBlock::Kind::Code; b.spans.push_back({code, 4, {}}); blocks.push_back(std::move(b)); code.clear(); fence.clear(); }
            else { code += line; code += '\n'; }
            continue;
        }
        if (t.rfind("```", 0) == 0 || t.rfind("~~~", 0) == 0) { flush(); fence = t.substr(0, 3); continue; }
        if (t.empty()) { flush(); continue; }
        NoteBlock b; std::string body = t; bool block = false;
        size_t hashes = 0; while (hashes < t.size() && t[hashes] == '#') ++hashes;
        if (hashes > 0 && hashes <= 6 && hashes < t.size() && t[hashes] == ' ') {
            b.kind = NoteBlock::Kind::Heading; b.level = static_cast<int>(hashes); body = trim(t.substr(hashes)); block = true;
        } else if (t == "---" || t == "***" || t == "___") { b.kind = NoteBlock::Kind::Rule; body = ""; block = true; }
        else if (t[0] == '>') { b.kind = NoteBlock::Kind::Quote; body = trim(t.substr(1)); block = true; }
        else if (t.size() >= 2 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ') {
            b.kind = NoteBlock::Kind::List; body = t.substr(2); block = true;
            if (body.size() >= 4 && body[0] == '[' && body[2] == ']' && body[3] == ' ' && (body[1] == ' ' || body[1] == 'x' || body[1] == 'X')) {
                b.kind = NoteBlock::Kind::Task; b.checked = body[1] != ' '; body = body.substr(4);
            }
        } else {
            size_t number = 0; while (number < t.size() && std::isdigit(static_cast<unsigned char>(t[number]))) ++number;
            if (number && number + 1 < t.size() && t[number] == '.' && t[number + 1] == ' ') { b.kind = NoteBlock::Kind::List; body = t; block = true; }
        }
        if (block) { flush(); b.spans = inlineText(body); blocks.push_back(std::move(b)); }
        else { if (!paragraph.empty()) paragraph += '\n'; paragraph += line; }
    }
    flush();
    if (!fence.empty()) { NoteBlock b; b.kind = NoteBlock::Kind::Code; b.spans.push_back({code, 4, {}}); blocks.push_back(std::move(b)); }
    return blocks;
}
}
