#pragma once

#include <string>
#include <vector>

// What panels hand to each other and to the drawing area while an item is dragged.
// Two ImGui payload types exist:
//   kCommandsPayload: a palette tool. First line "palette<TAB>tool" (where it came from), then
//                     the lines to run as typed; dropped on the drawing, or on a button of the
//                     same palette to reorder.
//   kContentPayload:  items of another drawing (content browser) or blocks of the open drawing
//                     (block palette, empty file); dropped on the drawing (copy and insert) or on
//                     a palette (add as tools).
namespace tbcad::ui {

inline constexpr const char* kCommandsPayload = "TBCAD_CMDS";
inline constexpr const char* kContentPayload = "TBCAD_CONTENT";

struct ContentDrag {
    std::string file;                // drawing the items come from
    std::string kind;                // Block, Layer, Linetype, Textstyle or Dimstyle
    std::vector<std::string> names;  // chosen items
};

// Payload text: file, kind, then one name per line.
inline std::string encodeContent(const ContentDrag& d) {
    std::string out = d.file + "\n" + d.kind;
    for (const std::string& n : d.names)
        out += "\n" + n;
    return out;
}

inline std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

inline ContentDrag decodeContent(const std::string& text) {
    ContentDrag d;
    const std::vector<std::string> lines = splitLines(text);
    if (lines.size() >= 2) {
        d.file = lines[0];
        d.kind = lines[1];
        d.names.assign(lines.begin() + 2, lines.end());
    }
    return d;
}

inline std::string encodeTool(const std::string& palette, const std::string& tool,
                              const std::vector<std::string>& lines) {
    std::string out = palette + "\t" + tool;
    for (const std::string& l : lines)
        out += "\n" + l;
    return out;
}

// Lines to run for a dropped tool (the origin line removed).
inline std::vector<std::string> toolDropLines(const std::string& payload) {
    std::vector<std::string> lines = splitLines(payload);
    lines.erase(lines.begin());
    return lines;
}

// Command lines that bring the dragged items into the drawing; a dropped block is also inserted.
// An empty file is the open drawing (a tile of the block palette): its block is just inserted.
inline std::vector<std::string> contentDropLines(const ContentDrag& d) {
    std::vector<std::string> out;
    if (d.names.empty())
        return out;
    if (d.file.empty()) {
        if (d.kind == "Block")
            out.push_back("INSERT " + d.names.front());
        return out;
    }
    std::string names;
    for (const std::string& n : d.names)
        names += (names.empty() ? "" : "|") + n;
    out = {"CONTENTIMPORT", d.file, d.kind, names};
    if (d.kind == "Block")
        out.push_back("INSERT " + d.names.front());
    return out;
}

}  // namespace tbcad::ui
