#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "tbcad/core/ids.h"
#include "tbcad/core/math.h"

namespace tbcad::index {

namespace detail {

// Non-owning reference to a callable (the callable must outlive the call).
template <class Sig>
class FunctionRef;
template <class R, class... Args>
class FunctionRef<R(Args...)> {
public:
    template <class F, class = std::enable_if_t<!std::is_same_v<std::decay_t<F>, FunctionRef>>>
    FunctionRef(F&& f) noexcept  // NOLINT(google-explicit-constructor)
        : obj_(const_cast<void*>(static_cast<const void*>(&f))), call_([](void* o, Args... a) -> R {
              return (*static_cast<std::remove_reference_t<F>*>(o))(std::forward<Args>(a)...);
          }) {}
    R operator()(Args... a) const { return call_(obj_, std::forward<Args>(a)...); }

private:
    void* obj_;
    R (*call_)(void*, Args...);
};

}  // namespace detail

// Entity ids with a bounding box, as handed to bulkLoad.
using IdBox = std::pair<EntityId, BBox2>;

struct Neighbor {
    EntityId id{};
    double distance = 0;  // distance from the query point to the box (0 when inside)
};

// Index of axis-aligned boxes keyed by EntityId.
//
// Layout: a packed Hilbert R-tree (node capacity 32) built by bulkLoad, kept up to date with
// R*-tree style insertion, splitting and deletion. Boxes covering more than a quarter of the
// root's area ("large" boxes: construction lines, background hatches) are kept in a separate list
// that is scanned linearly so that they do not degrade the tree.
//
// Query callbacks receive (EntityId, const BBox2&) and may return bool: true to continue,
// false to stop the query. A callback returning void always continues. Results are reported in
// no particular order. Callbacks must not modify the index.
//
// Not thread-safe for writers; any number of concurrent readers is fine while nobody writes.
// Boxes must be finite and non-empty; an id may appear once.
class SpatialIndex {
public:
    SpatialIndex();
    ~SpatialIndex();
    SpatialIndex(SpatialIndex&&) noexcept;
    SpatialIndex& operator=(SpatialIndex&&) noexcept;
    SpatialIndex(const SpatialIndex&) = delete;
    SpatialIndex& operator=(const SpatialIndex&) = delete;

    // Replaces the contents. Entries with a non-finite or empty box are skipped; if an id is
    // repeated the first occurrence wins. O(n log n), about 100-200 ms for 1 M boxes.
    void bulkLoad(std::span<const IdBox> items);

    // False (and no change) when the id is already present or the box is not finite/non-empty.
    bool insert(EntityId id, const BBox2& box);
    // False when the id is not present.
    bool remove(EntityId id);
    // Moves an existing id to a new box. Inserts it when absent. False only for an invalid box
    // (the entry is then removed, as it can no longer be located).
    bool update(EntityId id, const BBox2& newBox);

    bool contains(EntityId id) const;
    // The stored box of an id, or an empty box when absent.
    BBox2 boxOf(EntityId id) const;

    // Every entry whose box intersects `window` (touching counts).
    template <class F>
    void query(const BBox2& window, F&& callback) const {
        queryImpl(window, adapt(callback));
    }
    // Every entry whose box is within `radius` of `p` (true Euclidean distance to the box).
    template <class F>
    void queryPoint(Vec2 p, double radius, F&& callback) const {
        queryPointImpl(p, radius, adapt(callback));
    }
    // The k entries nearest to p (by distance to their boxes), nearest first, none farther than
    // maxDist. Ties are broken by id.
    std::vector<Neighbor> nearest(Vec2 p, std::size_t k, double maxDist) const;

    // Convenience: collect ids.
    std::vector<EntityId> queryIds(const BBox2& window) const;

    std::size_t size() const;
    bool empty() const { return size() == 0; }
    void clear();

    // Rebuilds the tree from its own contents (restores the bulk-load packing after many edits).
    void rebalance();

    // Bounds of every entry (empty when the index is empty).
    BBox2 bounds() const;
    // Entries currently in the linear "large" list.
    std::size_t largeCount() const;

    // Walks every entry. Mainly for tests.
    template <class F>
    void forEach(F&& callback) const {
        queryImpl(BBox2{{-kInf, -kInf}, {kInf, kInf}}, adapt(callback));
    }

    // Checks the structural invariants (box nesting, fill, parent links, id map). For tests.
    bool validate() const;

private:
    static constexpr double kInf = 1.7976931348623157e308;
    using Visitor = detail::FunctionRef<bool(EntityId, const BBox2&)>;

    template <class F>
    static auto adapt(F& f) {
        if constexpr (std::is_void_v<decltype(f(EntityId{}, BBox2{}))>) {
            return [&f](EntityId id, const BBox2& b) {
                f(id, b);
                return true;
            };
        } else {
            return [&f](EntityId id, const BBox2& b) -> bool { return f(id, b); };
        }
    }

    void queryImpl(const BBox2& window, Visitor visit) const;
    void queryPointImpl(Vec2 p, double radius, Visitor visit) const;

    struct Impl;
    Impl* impl_;
};

}  // namespace tbcad::index
