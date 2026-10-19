#include "chunks.h"

#include <algorithm>

#include "miniz.h"

namespace tbcad::io::detail {

void appendFileHeader(ByteWriter& out) {
    out.raw(kMagic, sizeof kMagic);
    out.u32(kFormatMajor);
    out.u32(kFormatMinor);
    out.u32(0);  // file flags, reserved
}

void appendStoredChunk(ByteWriter& out, std::uint32_t type, std::uint32_t flags,
                       std::span<const std::uint8_t> stored) {
    out.u32(type);
    out.u32(flags);
    out.u64(stored.size());
    out.raw(stored);
    out.u32(tbcadCrc32(stored));
}

PreparedChunk prepareChunk(std::span<const std::uint8_t> raw, bool compress) {
    PreparedChunk out;
    if (compress && raw.size() >= 64 && raw.size() < 0x7FFFFFFF) {
        const mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(raw.size()));
        // 8 bytes for the uncompressed size, then the deflate stream, in one buffer.
        std::vector<std::uint8_t> z(bound + 8);
        mz_ulong zlen = bound;
        const int rc =
            mz_compress2(z.data() + 8, &zlen, raw.data(), static_cast<mz_ulong>(raw.size()), 1);
        if (rc == MZ_OK && zlen + 8 < raw.size()) {
            const std::uint64_t n = raw.size();
            for (int i = 0; i < 8; ++i)
                z[i] = static_cast<std::uint8_t>(n >> (8 * i));
            out.flags = kChunkDeflate;
            // Copied out at its size: the work buffer has room for the raw bytes, and keeping it
            // would hold that much per chunk until the file is laid out.
            out.stored.assign(z.begin(), z.begin() + static_cast<std::ptrdiff_t>(zlen + 8));
            out.crc = tbcadCrc32(out.stored);
            return out;
        }
    }
    out.flags = 0;
    out.stored.assign(raw.begin(), raw.end());
    out.crc = tbcadCrc32(raw);
    return out;
}

void appendPrepared(ByteWriter& out, std::uint32_t type, const PreparedChunk& chunk) {
    out.u32(type);
    out.u32(chunk.flags);
    out.u64(chunk.stored.size());
    out.raw(chunk.stored);
    out.u32(chunk.crc);
}

void appendChunk(ByteWriter& out, std::uint32_t type, std::span<const std::uint8_t> raw,
                 bool compress) {
    appendPrepared(out, type, prepareChunk(raw, compress));
}

ParsedFile parseFile(std::span<const std::uint8_t> bytes, bool verifyCrc,
                     std::vector<std::string>* salvage) {
    if (bytes.size() < sizeof kMagic ||
        !std::equal(std::begin(kMagic), std::end(kMagic), bytes.begin())) {
        if (bytes.size() >= 8 && bytes[1] == 'T' && bytes[2] == 'B' && bytes[3] == 'C') {
            throw FormatError(
                "not a valid .tbcad file: the signature is damaged (was the file copied in text "
                "mode or edited?)");
        }
        throw FormatError("not a .tbcad file");
    }
    ByteReader r(bytes);
    r.skip(sizeof kMagic);
    ParsedFile f;
    f.major = r.u32();
    f.minor = r.u32();
    f.flags = r.u32();
    if (f.major == 0)
        throw FormatError("invalid format version 0");
    if (f.major > kFormatMajor) {
        throw FormatError("this file uses .tbcad format " + std::to_string(f.major) + "." +
                          std::to_string(f.minor) +
                          ", which is newer than this program understands (format " +
                          std::to_string(kFormatMajor) + "); please update TBCAD");
    }
    for (;;) {
        if (r.remaining() == 0 || r.remaining() < kChunkHeaderSize) {
            if (salvage) {
                salvage->push_back("the file ends early (no END chunk): what came before is kept");
                return f;
            }
            if (r.remaining() == 0)
                throw FormatError("file is truncated (no END chunk)");
            throw FormatError("file is truncated inside a chunk header");
        }
        RawChunk c;
        c.type = r.u32();
        c.flags = r.u32();
        std::uint64_t len = r.u64();
        if (len > r.remaining() || r.remaining() - len < 4) {
            if (salvage) {
                salvage->push_back("the file is cut off inside chunk " + fourccName(c.type) +
                                   ": that chunk and everything after it is lost");
                return f;
            }
            throw FormatError("file is truncated inside chunk " + fourccName(c.type));
        }
        c.stored = r.bytes(static_cast<std::size_t>(len));
        std::uint32_t crc = r.u32();
        c.crcOk = crc == tbcadCrc32(c.stored);
        if (!c.crcOk && verifyCrc && !salvage)
            throw FormatError("checksum mismatch in chunk " + fourccName(c.type));
        if (c.type == kEnd) {
            f.trailingBytes = r.remaining();
            return f;
        }
        if (!c.crcOk && verifyCrc) {
            salvage->push_back("damaged chunk " + fourccName(c.type) + " was dropped");
            continue;
        }
        f.chunks.push_back(c);
    }
}

std::span<const std::uint8_t> chunkPayload(const RawChunk& c, std::vector<std::uint8_t>& scratch) {
    if (!(c.flags & kChunkDeflate))
        return c.stored;
    ByteReader r(c.stored);
    std::uint64_t rawLen = r.u64();
    auto z = r.rest();
    // zlib cannot expand more than ~1032:1; anything beyond that is a lie about the size.
    if (rawLen > kMaxChunkRaw || rawLen > static_cast<std::uint64_t>(z.size()) * 1100 + 64)
        throw FormatError("implausible uncompressed size in chunk " + fourccName(c.type));
    scratch.assign(static_cast<std::size_t>(rawLen), 0);
    mz_ulong outLen = static_cast<mz_ulong>(rawLen);
    int rc = mz_uncompress(scratch.data(), &outLen, z.data(), static_cast<mz_ulong>(z.size()));
    if (rc != MZ_OK || outLen != rawLen)
        throw FormatError("corrupt compressed data in chunk " + fourccName(c.type));
    return scratch;
}

}  // namespace tbcad::io::detail
