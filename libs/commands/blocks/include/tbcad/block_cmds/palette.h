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

// ---- placing from the palette -----------------------------------------------------------------

// The Options section of the block palette.
struct InsertOptions {
    double scale = 1;
    double rotationDeg = 0;
    bool repeat = false;   // keep placing copies until Enter
    bool explode = false;  // place the parts, not the block
};

// The INSERT script the palette runs for a block (or a drawing file): the name, then Scale and
// Rotate only when they differ from 1 and 0, then Multiple and Explode when asked for. The point
// is left to the user.
std::vector<std::string> paletteInsertLines(const std::string& name, const InsertOptions& o);

// ---- recently inserted blocks ------------------------------------------------------------------

// A block placed with INSERT, for the Recent tab. `file` is the drawing it came from (empty for
// the drawing it was placed in); `whole` is set when the whole file was inserted as a block.
struct RecentBlock {
    std::string name{};
    std::string file{};
    bool whole = false;
    bool operator==(const RecentBlock&) const = default;
};

constexpr std::size_t kMaxRecentBlocks = 24;

// The list kept in the "block.recent" setting, newest first: one record per block, fields split
// by \x1f and records by \x1e. Broken records are skipped.
std::vector<RecentBlock> parseRecent(const std::string& setting);
std::string formatRecent(const std::vector<RecentBlock>& list);
// Puts `b` first, drops an older entry for the same block and file, and keeps at most
// kMaxRecentBlocks.
std::string pushRecent(const std::string& setting, const RecentBlock& b);

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
