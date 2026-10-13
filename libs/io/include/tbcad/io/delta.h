#pragma once

// The "after" half of a ChangeSet in a compact binary form, for sending an edit to another
// process (live collaboration). It reuses the .tbcad entity and row codecs, so what travels and
// what is saved can not drift apart. The receiver rebuilds the "before" half from its own copy of
// the document, which is also why a sender can not lie about it.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "tbcad/core/document.h"
#include "tbcad/core/result.h"

namespace tbcad::io {

template <class Row>
struct RowDelta {
    std::uint32_t id = 0;
    std::optional<Row> after{};  // empty = the row was erased
};

struct EntityDelta {
    EntityId id{};
    std::optional<Entity> after{};  // empty = the entity was erased
};

struct OrderDelta {
    EntityId id{};
    std::optional<std::int64_t> after{};
};

struct Delta {
    std::string name{};
    std::vector<EntityDelta> entities{};
    std::vector<RowDelta<Layer>> layers{};
    std::vector<RowDelta<Level>> levels{};
    std::vector<RowDelta<Linetype>> linetypes{};
    std::vector<RowDelta<TextStyle>> textStyles{};
    std::vector<RowDelta<DimStyle>> dimStyles{};
    std::vector<RowDelta<BlockDef>> blocks{};
    // Header after the change. The session-only flag (activeOnly) and chunks carried through
    // from a file are not part of it; materialize() keeps the receiver's own.
    std::optional<Header> header{};
    std::vector<OrderDelta> order{};

    bool empty() const;
    std::size_t entityCount() const { return entities.size(); }
};

// Largest things a decoder accepts. A hostile peer can not make it allocate more.
struct DeltaLimits {
    std::size_t maxBytes = std::size_t{64} << 20;
    std::size_t maxEntities = std::size_t{1} << 20;
    std::size_t maxRows = std::size_t{1} << 16;
};

Delta deltaOf(const ChangeSet& changes);

std::vector<std::uint8_t> encodeDelta(const Delta& delta);
// Never throws or crashes: damaged or oversized input yields an error.
Result<Delta> decodeDelta(std::span<const std::uint8_t> bytes, const DeltaLimits& limits = {});

// Turns a delta into a ChangeSet against `doc` (the "before" half is read from the document).
// Fails when the delta erases something the document does not have.
Result<ChangeSet> materialize(const Delta& delta, const Document& doc);

}  // namespace tbcad::io
