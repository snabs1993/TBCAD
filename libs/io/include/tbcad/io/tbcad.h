#pragma once

// Reading and writing the native .tbcad file format (see docs/file-format.md).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "tbcad/core/document.h"
#include "tbcad/core/result.h"

namespace tbcad::io {

// What a read found besides the document itself.
struct IoReport {
    std::uint32_t formatMajor = 0;
    std::uint32_t formatMinor = 0;
    std::size_t entityCount = 0;
    std::size_t unknownChunks = 0;        // kept as opaque blobs and written back unchanged
    std::size_t unknownEntityKinds = 0;   // kept as read-only ProxyData
    std::vector<std::string> warnings{};  // recoverable problems (skipped entities, ...)
};

struct ReadOptions {
    // Tests turn this off so that mutated files reach the parsers instead of failing the CRC.
    bool verifyCrc = true;
    // RECOVER: keep whatever can be read of a damaged file (a cut-off tail, chunks that fail
    // their checksum, tables or entity chunks that do not parse) and explain the losses in
    // IoReport::warnings. Fails only when the signature, header table or string table is gone.
    bool salvage = false;
};

// An optional chunk added by a layer above the document model (for example the entity versions
// of a shared session). Readers that do not know the type keep it and write it back unchanged.
struct ExtraChunk {
    std::string type{};  // four characters, e.g. "VERS"
    std::vector<std::uint8_t> payload{};
};

struct WriteOptions {
    // Written after the entities. An opaque chunk of the same type that came from a file we read
    // is dropped in their favour.
    std::vector<ExtraChunk> extraChunks{};
    bool compress = true;    // deflate every chunk that is large enough to benefit
    bool keepBackup = true;  // keep the previous file as <name>.tbcad.bak
    // Test hook: stop after the temporary file is complete, as if the process died before the
    // final replace. writeTbcad then returns an error and leaves `<name>.tbcad.tmp` behind.
    bool simulateCrashBeforeReplace = false;
};

// Parses a complete file image. A corrupt or unsupported file yields an error, never a crash.
Result<std::unique_ptr<Document>> readTbcad(std::span<const std::uint8_t> bytes, IoReport& report,
                                            const ReadOptions& options = {});
Result<std::unique_ptr<Document>> readTbcad(const std::filesystem::path& path, IoReport& report,
                                            const ReadOptions& options = {});

// Serialises to memory. The output is deterministic for a given document and options.
Result<std::vector<std::uint8_t>> writeTbcadToMemory(const Document& doc,
                                                     const WriteOptions& options = {});
// Saves atomically: <path>.tmp is written and synced first, the previous file becomes
// <path>.bak (when keepBackup), then the temporary file replaces <path>.
Result<void> writeTbcad(const Document& doc, const std::filesystem::path& path,
                        const WriteOptions& options = {});
// The second half of writeTbcad: saves a file image atomically (see writeTbcad).
Result<void> saveTbcadBytes(std::span<const std::uint8_t> bytes, const std::filesystem::path& path,
                            const WriteOptions& options = {});
// Saves without holding the caller up for long. The document is encoded on the calling thread
// (a few hundred milliseconds for a million objects); deflating the chunks, writing, syncing and
// the final replace then run on other threads. The document may be changed as soon as this
// returns. The future holds the same result writeTbcad would give.
std::future<Result<void>> writeTbcadAsync(const Document& doc, const std::filesystem::path& path,
                                          const WriteOptions& options = {});

// The (inflated) payload of the optional chunk `type` that readTbcad kept for a file with chunks
// this build does not interpret itself, or nothing.
std::optional<std::vector<std::uint8_t>> findOptionalChunk(const Document& doc,
                                                           std::string_view type);

// As findOptionalChunk, and removes the chunk from the document's header so that a later save
// does not write a stale copy back. Meant for a document that was just read.
std::optional<std::vector<std::uint8_t>> takeOptionalChunk(Document& doc, std::string_view type);

struct ChunkInfo {
    std::string type{};  // four characters, e.g. "ENTS"
    std::uint32_t flags = 0;
    std::uint64_t storedSize = 0;
    bool crcOk = true;
};
// Lists the chunks of a file image without decoding them (used by tbcad-dump).
Result<std::vector<ChunkInfo>> listChunks(std::span<const std::uint8_t> bytes);

}  // namespace tbcad::io
