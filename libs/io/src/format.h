#pragma once

// Constants and codecs shared by the .tbcad reader and writer (internal to libs/io).
// The format is specified in docs/file-format.md.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "bytes.h"
#include "tbcad/core/document.h"

namespace tbcad::io::detail {

inline constexpr std::uint8_t kMagic[10] = {0x89, 'T', 'B', 'C', 'A', 'D', '\r', '\n', 0x1A, '\n'};
inline constexpr std::uint32_t kFormatMajor = 1;
inline constexpr std::uint32_t kFormatMinor = 6;
inline constexpr std::size_t kFileHeaderSize = 22;   // magic 10 + major + minor + flags
inline constexpr std::size_t kChunkHeaderSize = 16;  // fourcc + flags + length
inline constexpr std::uint32_t kChunkDeflate = 1;
inline constexpr std::uint16_t kRecordVersion = 1;
inline constexpr std::size_t kMaxChunkRaw = std::size_t{1} << 30;  // sanity cap per chunk
inline constexpr std::size_t kEntsChunkTarget = std::size_t{8} << 20;

inline constexpr std::uint32_t kHead = fourcc("HEAD");
inline constexpr std::uint32_t kStrs = fourcc("STRS");
inline constexpr std::uint32_t kLayr = fourcc("LAYR");
inline constexpr std::uint32_t kLvls = fourcc("LVLS");
inline constexpr std::uint32_t kLtyp = fourcc("LTYP");
inline constexpr std::uint32_t kTsty = fourcc("TSTY");
inline constexpr std::uint32_t kDsty = fourcc("DSTY");
inline constexpr std::uint32_t kBlks = fourcc("BLKS");
inline constexpr std::uint32_t kXrfs = fourcc("XRFS");
inline constexpr std::uint32_t kDynb = fourcc("DYNB");
inline constexpr std::uint32_t kEnts = fourcc("ENTS");
inline constexpr std::uint32_t kElvl = fourcc("ELVL");
inline constexpr std::uint32_t kOrdr = fourcc("ORDR");
inline constexpr std::uint32_t kXrfo = fourcc("XRFO");
inline constexpr std::uint32_t kEnd = fourcc("END ");

// Key prefixes inside Header::custom used to carry data that has no other home in the Document.
inline constexpr std::string_view kCustomPrefix = "c:";  // HEAD key prefix only
inline constexpr std::string_view kOpaqueChunkPrefix = "\x01tbcad.chunk.";

// originalType prefix of a ProxyData that stands for an entity kind this build does not know.
inline constexpr std::string_view kUnknownKindPrefix = "\x01tbcad-kind:";

// Writer-side string interning. Index 0 is always the empty string.
class StringTable {
public:
    StringTable() { intern(""); }
    std::uint32_t intern(std::string_view s) {
        auto it = index_.find(std::string(s));
        if (it != index_.end())
            return it->second;
        auto id = static_cast<std::uint32_t>(strings_.size());
        strings_.emplace_back(s);
        index_.emplace(strings_.back(), id);
        return id;
    }
    const std::vector<std::string>& all() const { return strings_; }

private:
    std::vector<std::string> strings_{};
    std::unordered_map<std::string, std::uint32_t> index_{};
};

// Appends one entity record (kind, record version, length, common properties, payload).
void encodeEntity(ByteWriter& w, StringTable& strings, const Entity& e, BlockId owner);

struct DecodedEntity {
    Entity entity{};
    BlockId owner{};
    bool unknownKind = false;       // kept as a ProxyData holding the raw payload
    std::size_t trailingBytes = 0;  // payload bytes of a newer minor version that were ignored
};
// Decodes the record at the reader's position.
DecodedEntity decodeEntity(ByteReader& r, const std::vector<std::string>& strings);

}  // namespace tbcad::io::detail
