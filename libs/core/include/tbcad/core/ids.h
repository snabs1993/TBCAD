#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace tbcad {

// Strong integer id. Value 0 is the null id.
template <class Tag, class Rep>
struct StrongId {
    Rep value{};

    constexpr explicit operator bool() const { return value != 0; }
    friend constexpr bool operator==(StrongId, StrongId) = default;
    friend constexpr auto operator<=>(StrongId, StrongId) = default;
};

// Never reused within a document.
using EntityId = StrongId<struct EntityIdTag, std::uint64_t>;
using LayerId = StrongId<struct LayerIdTag, std::uint32_t>;
// LinetypeId{0} means "ByLayer" when used on an entity; kLinetypeByBlock means "ByBlock".
using LinetypeId = StrongId<struct LinetypeIdTag, std::uint32_t>;
using TextStyleId = StrongId<struct TextStyleIdTag, std::uint32_t>;
using DimStyleId = StrongId<struct DimStyleIdTag, std::uint32_t>;
using BlockId = StrongId<struct BlockIdTag, std::uint32_t>;
// A drawing level (floor, storey): objects on different levels never interact.
using LevelId = StrongId<struct LevelIdTag, std::uint32_t>;

inline constexpr LinetypeId kLinetypeByLayer{0};
inline constexpr LinetypeId kLinetypeByBlock{0xFFFFFFFEu};
// Only valid on a sub-layer: take the parent layer's linetype.
inline constexpr LinetypeId kLinetypeByParent{0xFFFFFFFDu};

}  // namespace tbcad

namespace std {
template <class Tag, class Rep>
struct hash<tbcad::StrongId<Tag, Rep>> {
    size_t operator()(tbcad::StrongId<Tag, Rep> id) const noexcept { return hash<Rep>{}(id.value); }
};
}  // namespace std
