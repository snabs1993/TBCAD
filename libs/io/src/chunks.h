#pragma once

#include <vector>

#include "format.h"

namespace tbcad::io::detail {

// Appends the file header.
void appendFileHeader(ByteWriter& out);
// Appends a chunk holding `raw`, deflated when `compress` is set and it pays off.
void appendChunk(ByteWriter& out, std::uint32_t type, std::span<const std::uint8_t> raw,
                 bool compress);
// A chunk ready to be appended: deflated (flags kChunkDeflate) or stored, with its CRC. Building
// one touches no shared state, so several can be built on different threads.
struct PreparedChunk {
    std::uint32_t flags = 0;
    std::vector<std::uint8_t> stored{};
    std::uint32_t crc = 0;
};
PreparedChunk prepareChunk(std::span<const std::uint8_t> raw, bool compress);
void appendPrepared(ByteWriter& out, std::uint32_t type, const PreparedChunk& chunk);
// Appends a chunk exactly as stored (used for opaque chunks).
void appendStoredChunk(ByteWriter& out, std::uint32_t type, std::uint32_t flags,
                       std::span<const std::uint8_t> stored);

struct RawChunk {
    std::uint32_t type = 0;
    std::uint32_t flags = 0;
    std::span<const std::uint8_t> stored{};
    bool crcOk = true;
};

struct ParsedFile {
    std::uint32_t major = 0, minor = 0, flags = 0;
    std::vector<RawChunk> chunks{};  // up to and excluding END
    std::size_t trailingBytes = 0;   // after END
};

// Splits a file image into chunks, checking magic, lengths and (optionally) CRCs.
// Throws FormatError on any structural problem.
ParsedFile parseFile(std::span<const std::uint8_t> bytes, bool verifyCrc);
// Returns the chunk's payload, inflating it when needed. `scratch` owns inflated data.
std::span<const std::uint8_t> chunkPayload(const RawChunk& c, std::vector<std::uint8_t>& scratch);

}  // namespace tbcad::io::detail
