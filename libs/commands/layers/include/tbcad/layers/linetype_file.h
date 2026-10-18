#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::layers {

// Parses a linetype file: either the AutoCAD .lin format (*NAME,description then an A,... line;
// acad.lin and acadiso.lin, with ["TEXT",STYLE,S=..,R=..,X=..,Y=..] and [SHAPE,file.shx,...]
// elements), or TBCAD's own format (see assets/linetypes/tbcad.lintypes):
//     name ; description ; pattern in mm
// Positive = dash, negative = gap, zero = dot. Bad lines are skipped and reported in `problems`
// ("line 7: ...").
struct LinetypeFile {
    std::vector<Linetype> linetypes{};
    std::vector<std::string> problems{};
};
LinetypeFile parseLinetypes(std::string_view text);

// The set that ships with TBCAD (compiled in, so it works without the assets folder).
const std::string& builtinLinetypeText();

enum class LoadMode { SkipExisting, Replace };

// Adds the linetypes (all, or the named ones; "*" or an empty list = all) that are not in the
// document yet. With Replace an existing linetype of the same name gets the new pattern. Returns
// the names that were added or replaced.
Result<std::vector<std::string>> loadLinetypes(const Document& doc, Transaction& tx,
                                               const LinetypeFile& file,
                                               const std::vector<std::string>& names = {},
                                               LoadMode mode = LoadMode::SkipExisting);

// Reads a user file from disk.
Result<LinetypeFile> readLinetypeFile(const std::filesystem::path& path);

}  // namespace tbcad::layers
