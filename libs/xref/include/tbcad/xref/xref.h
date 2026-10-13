#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::xref {

// An external reference is a BlockDef with `xrefPath` set. Its entities are a snapshot of the
// model space of the other drawing, kept in the file so the drawing opens without the source.
//
// * The source's layers live under a top-level layer named like the xref, so a layer "edge" of
//   the file "site" shows up as the sub-layer site~edge. Turning the xref layer off hides it all.
// * Blocks used by the source are copied as "<xref>$<block>".
// * Linetypes, text styles and dimension styles are matched by name and added when missing.

// A loaded source drawing.
struct Source {
    std::unique_ptr<Document> doc{};
    std::int64_t stamp = 0;  // last-write time in nanoseconds since the Unix epoch
    std::size_t warnings = 0;
    std::filesystem::path file{};  // where it was read from
};

// How the path of an xref is stored in the drawing.
enum class PathType {
    Relative,  // from the folder of the host drawing (falls back to Absolute when not possible)
    Absolute,
    None,  // only the file name: looked up in the folder of the host drawing
};
// The text to store for `file` seen from the drawing `hostFile` (empty = not saved yet).
std::string storedPath(const std::filesystem::path& file, const std::filesystem::path& hostFile,
                       PathType type);

// Readers for source formats beyond .tbcad and .dxf (the DWG reader registers ".dwg").
// `extension` is lower case with the dot.
using Loader = std::function<Result<Source>(const std::filesystem::path&)>;
void registerLoader(std::string extension, Loader loader);

// What a fill did.
struct Summary {
    std::size_t entities = 0;
    std::size_t blocks = 0;  // nested blocks copied
    std::size_t layers = 0;  // layers created
};

enum class State {
    UpToDate,
    Changed,  // the file is newer (or older) than the snapshot
    Missing,  // the file cannot be found
    NotXref,
};

// Resolves a stored path: absolute paths stay, relative ones are taken from the folder of the
// host drawing (empty `hostFile` = the current folder).
std::filesystem::path resolvePath(std::string_view stored, const std::filesystem::path& hostFile);
// Last-write time in nanoseconds since the Unix epoch; 0 when the file cannot be read.
std::int64_t fileStamp(const std::filesystem::path& file);
// A block name for a file: its stem with characters a block name cannot hold replaced.
std::string defaultName(const std::filesystem::path& file);

// Reads a .tbcad, .dxf or registered file by its extension. Xrefs inside the source are
// reloaded from their own files first (relative to the source), so nested xrefs stay live; one
// that cannot be read keeps its snapshot. A reference loop is cut where it closes.
Result<Source> loadSource(const std::filesystem::path& file);

// Creates the block `name` as an xref of `stored` (the path as the user typed it) and fills it
// from `source`. The block name must be free. `overlay`: an overlay xref is left out when this
// drawing is used as an xref by another one. Returns the new block.
Result<BlockId> attach(Transaction& tx, Document& doc, std::string_view name,
                       std::string_view stored, const Source& source, Summary* summary = nullptr,
                       bool overlay = false);
// Creates an ordinary block `name` from the model space of `source` (a drawing file used as a
// block, as in the block library). Layers are matched by name with the host's own and created
// when missing; layer 0 stays layer 0. The block name must be free.
Result<BlockId> importAsBlock(Transaction& tx, Document& doc, std::string_view name,
                              const Source& source, Summary* summary = nullptr);
// Replaces the content of an xref block by `source`; inserts of the block stay where they are.
Result<void> reload(Transaction& tx, Document& doc, BlockId block, const Source& source,
                    Summary* summary = nullptr);
// Removes the xref: its inserts, its block, the nested blocks and the layers no entity uses.
Result<void> detach(Transaction& tx, Document& doc, BlockId block);
// Turns the xref into an ordinary block (and its nested blocks and layers into ordinary ones).
Result<void> bind(Transaction& tx, Document& doc, BlockId block);

// Whether the file of an xref block changed since the snapshot.
State state(const Document& doc, BlockId block, const std::filesystem::path& hostFile);
// All xref blocks.
std::vector<BlockId> xrefs(const Document& doc);
// How many inserts reference the block (model space and inside other blocks).
std::size_t useCount(const Document& doc, BlockId block);

}  // namespace tbcad::xref
