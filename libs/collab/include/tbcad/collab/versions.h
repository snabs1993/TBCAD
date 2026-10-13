#pragma once

// Versions: how a host tells whether an edit was made on top of the latest state of a thing.
//
// Every entity, every table row, the header and every draw-order entry has a version. It is the
// sequence number of the last accepted operation that touched it (0 = never touched since the
// session began). An operation carries the versions its author saw; the host accepts it only if
// they are still current.

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "tbcad/core/result.h"
#include "tbcad/io/delta.h"

namespace tbcad::collab {

enum class Space : std::uint8_t {
    Entity,
    Layer,
    Linetype,
    TextStyle,
    DimStyle,
    Block,
    Header,  // one per document, id 0
    Order,   // draw order of one entity, id = entity id
    Level,
};
inline constexpr std::uint8_t kLastSpace = static_cast<std::uint8_t>(Space::Level);

struct Key {
    Space space = Space::Entity;
    std::uint64_t id = 0;
    friend constexpr auto operator<=>(const Key&, const Key&) = default;
};

// Everything a delta touches, sorted and without repeats.
std::vector<Key> keysOf(const io::Delta& delta);
// Does the thing exist in the document right now? (Header and Order entries always do.)
bool exists(const Document& doc, const Key& key);
// True when the delta creates, changes or erases the thing and `existedBefore` tells which.
const char* spaceName(Space s);

class VersionMap {
public:
    std::uint64_t get(const Key& k) const {
        auto it = v_.find(k);
        return it == v_.end() ? 0 : it->second;
    }
    // Setting 0 forgets the entry. Entries of erased things are kept on purpose: an old edit of
    // an erased entity must still be recognised as stale.
    void set(const Key& k, std::uint64_t version) {
        if (version == 0)
            v_.erase(k);
        else
            v_[k] = version;
    }
    void setAll(const std::vector<Key>& keys, std::uint64_t version) {
        for (const Key& k : keys)
            set(k, version);
    }
    std::size_t size() const { return v_.size(); }
    bool empty() const { return v_.empty(); }
    const std::map<Key, std::uint64_t>& entries() const { return v_; }
    friend bool operator==(const VersionMap&, const VersionMap&) = default;

private:
    std::map<Key, std::uint64_t> v_{};
};

// The optional "VERS" chunk of a .tbcad file: the session's sequence number and all versions.
inline constexpr const char* kVersionsChunk = "VERS";
std::vector<std::uint8_t> encodeVersions(const VersionMap& versions, std::uint64_t sequence);
struct DecodedVersions {
    VersionMap versions{};
    std::uint64_t sequence = 0;
};
// Entries beyond `maxEntries` are an error, so a damaged file can not make this allocate wildly.
Result<DecodedVersions> decodeVersions(std::span<const std::uint8_t> bytes,
                                       std::size_t maxEntries = std::size_t{1} << 26);

}  // namespace tbcad::collab
