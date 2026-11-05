// Portable notes metadata and a small, inert Markdown preview model.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace omapad {
struct NoteSummary {
    std::string path, title, preview, content;
    std::vector<std::string> tags;
    int64_t modified = 0;
};
struct NoteSpan {
    std::string text;
    unsigned style = 0; // 1 bold, 2 italic, 4 code, 8 link. No HTML is executed.
    std::string href;
};
struct NoteBlock {
    enum class Kind { Paragraph, Heading, Code, Quote, List, Task, Rule };
    Kind kind = Kind::Paragraph;
    int level = 0;
    bool checked = false;
    std::vector<NoteSpan> spans;
};
struct NotesState {
    bool enabled = false, focus = false, activeNote = false;
    int viewMode = 0; // 0 Source, 1 Split, 2 Preview.
    std::string folder, query, tag, selectedPath, title, status;
    bool saveError = false;
    std::vector<NoteSummary> notes; // Filtered, most recently edited first.
    std::vector<std::pair<std::string, int>> tags;
    size_t total = 0, words = 0;
    std::vector<NoteBlock> preview;
};
NoteSummary describeNote(const std::string &path, const std::string &text, int64_t modified = 0);
bool noteMatches(const NoteSummary &note, const std::string &query, const std::string &tag);
std::vector<NoteBlock> renderNote(const std::string &text);
size_t noteWordCount(const std::string &text);
}
