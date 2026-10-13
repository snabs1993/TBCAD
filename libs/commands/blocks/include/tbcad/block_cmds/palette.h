#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::block_cmds {

// A small line drawing of a block or a drawing file for the block palette. Points are scaled so
// the longer side of the drawing is 1; x runs 0..width and y 0..height with y up.
struct Thumbnail {
    std::vector<std::vector<Vec2>> strokes{};
    double width = 0;
    double height = 0;
    bool empty() const { return strokes.empty(); }
};

// The geometry of a block as one Insert shows it. Text and hatches are included when their
// display generators are registered. At most about 20,000 points are kept.
Thumbnail thumbnailOfBlock(const Document& doc, BlockId block);
// The model space of a drawing, for files in the block library.
Thumbnail thumbnailOfModel(const Document& doc);

// ---- library: a folder of drawing files used as blocks -----------------------------------------

struct LibraryItem {
    std::filesystem::path path{};
    std::string name{};  // file name without extension
};

// *.tbcad and *.dxf files below `folder` (up to three levels), sorted by name; names that contain
// `filter` (case-insensitive) only. At most 2,000 items.
std::vector<LibraryItem> scanLibrary(const std::filesystem::path& folder,
                                     const std::string& filter = {});

// Thumbnails of library files, loaded one at a time so the panel stays responsive. A file that
// changed since it was loaded is loaded again.
class LibraryThumbnails {
public:
    // The thumbnail if it is loaded (also an empty one for a file that could not be read).
    const Thumbnail* find(const std::filesystem::path& file) const;
    // Loads the first item without a current thumbnail; false when all are loaded.
    bool loadNext(const std::vector<LibraryItem>& items);

private:
    struct Entry {
        std::int64_t stamp = 0;
        Thumbnail thumbnail{};
    };
    std::map<std::filesystem::path, Entry> entries_{};
};

// The file a typed block name stands for, or empty. A name that is a path to a .tbcad or .dxf file
// (absolute, or relative to the drawing's folder or to the library folder; the extension may be
// left out for .tbcad) names a library file.
std::filesystem::path libraryFileFor(const std::string& typed,
                                     const std::filesystem::path& drawingFile,
                                     const std::filesystem::path& libraryFolder);

// ---- block count -------------------------------------------------------------------------------

struct BlockCountRow {
    std::string name{};
    std::size_t direct = 0;  // inserts placed in model space
    std::size_t total = 0;   // including the blocks nested in other inserted blocks
};

// One row per named block that is used at least once, sorted by name. Nested inserts count once
// per instance of the block they sit in.
std::vector<BlockCountRow> blockCount(const Document& doc);
// "Block,Count,Total" lines with RFC 4180 quoting.
std::string blockCountCsv(const std::vector<BlockCountRow>& rows);

}  // namespace tbcad::block_cmds
