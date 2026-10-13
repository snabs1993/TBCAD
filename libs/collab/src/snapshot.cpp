#include "tbcad/collab/snapshot.h"

#include "tbcad/io/tbcad.h"

namespace tbcad::collab {

Result<std::vector<std::uint8_t>> saveSnapshot(const Document& doc, const VersionMap& versions,
                                               std::uint64_t sequence) {
    io::WriteOptions o;
    o.extraChunks.push_back({kVersionsChunk, encodeVersions(versions, sequence)});
    return io::writeTbcadToMemory(doc, o);
}

Result<Snapshot> loadSnapshot(std::span<const std::uint8_t> bytes) {
    io::IoReport report;
    Result<std::unique_ptr<Document>> doc = io::readTbcad(bytes, report);
    if (!doc)
        return doc.error();
    Snapshot s;
    s.document = std::move(doc.value());
    if (auto chunk = io::takeOptionalChunk(*s.document, kVersionsChunk)) {
        Result<DecodedVersions> v = decodeVersions(*chunk);
        if (!v)
            return v.error();
        s.versions = std::move(v.value().versions);
        s.sequence = v.value().sequence;
    }
    return s;
}

}  // namespace tbcad::collab
