#pragma once

// A document together with the session versions, as one .tbcad image. The versions travel in the
// optional "VERS" chunk, so older programs open the file and ignore them.

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "tbcad/collab/versions.h"
#include "tbcad/core/document.h"

namespace tbcad::collab {

Result<std::vector<std::uint8_t>> saveSnapshot(const Document& doc, const VersionMap& versions,
                                               std::uint64_t sequence);

struct Snapshot {
    std::unique_ptr<Document> document{};
    VersionMap versions{};
    std::uint64_t sequence = 0;
};
// A file without the chunk loads with empty versions and sequence 0.
Result<Snapshot> loadSnapshot(std::span<const std::uint8_t> bytes);

}  // namespace tbcad::collab
