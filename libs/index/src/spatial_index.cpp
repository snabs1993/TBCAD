#include "tbcad/index/spatial_index.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <queue>

#include "hilbert.h"

namespace tbcad::index {

namespace {

constexpr std::uint32_t kMaxEntries = 32;  // node capacity
constexpr std::uint32_t kMinEntries = 13;  // ~40% of the capacity
constexpr std::uint32_t kPackFill = 28;    // entries per node after a bulk load
constexpr std::uint32_t kNil = 0xFFFFFFFFu;
constexpr std::uint32_t kInLarge = 0xFFFFFFFEu;  // id map value for entries in the large list
constexpr double kLargeFraction = 0.25;
constexpr std::size_t kLargeMinSize = 64;  // no "large" boxes while the index is tiny

inline void prefetch(const void* p) {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(p);
#else
    (void)p;
#endif
}

struct KeyIndex {
    std::uint64_t key;
    std::uint32_t index;
};

// Stable LSD radix sort on the 64-bit key; byte positions where every key agrees are skipped
// (common when the drawing is small compared to the 2^32 grid).
void sortByKey(std::vector<KeyIndex>& a) {
    const std::size_t n = a.size();
    if (n < 4096) {
        std::stable_sort(a.begin(), a.end(),
                         [](const KeyIndex& x, const KeyIndex& y) { return x.key < y.key; });
        return;
    }
    std::vector<std::uint32_t> hist(8 * 256, 0);
    for (const KeyIndex& k : a)
        for (int b = 0; b < 8; ++b)
            ++hist[b * 256 + ((k.key >> (8 * b)) & 0xFF)];
    std::vector<KeyIndex> tmp(n);
    std::vector<KeyIndex>* src = &a;
    std::vector<KeyIndex>* dst = &tmp;
    for (int b = 0; b < 8; ++b) {
        std::uint32_t* h = &hist[b * 256];
        if (h[(a[0].key >> (8 * b)) & 0xFF] == n)
            continue;
        std::uint32_t sum = 0;
        for (int i = 0; i < 256; ++i) {
            std::uint32_t c = h[i];
            h[i] = sum;
            sum += c;
        }
        for (const KeyIndex& k : *src)
            (*dst)[h[(k.key >> (8 * b)) & 0xFF]++] = k;
        std::swap(src, dst);
    }
    if (src != &a)
        a = std::move(*src);
}

bool validBox(const BBox2& b) {
    return std::isfinite(b.min.x) && std::isfinite(b.min.y) && std::isfinite(b.max.x) &&
           std::isfinite(b.max.y) && b.min.x <= b.max.x && b.min.y <= b.max.y;
}

double areaOf(const BBox2& b) {
    return (b.max.x - b.min.x) * (b.max.y - b.min.y);
}
double marginOf(const BBox2& b) {
    return (b.max.x - b.min.x) + (b.max.y - b.min.y);
}
BBox2 unite(const BBox2& a, const BBox2& b) {
    BBox2 r;
    r.min = {std::min(a.min.x, b.min.x), std::min(a.min.y, b.min.y)};
    r.max = {std::max(a.max.x, b.max.x), std::max(a.max.y, b.max.y)};
    return r;
}
bool overlaps(const BBox2& a, const BBox2& b) {
    return a.min.x <= b.max.x && b.min.x <= a.max.x && a.min.y <= b.max.y && b.min.y <= a.max.y;
}
bool covers(const BBox2& outer, const BBox2& inner) {
    return outer.min.x <= inner.min.x && outer.min.y <= inner.min.y && outer.max.x >= inner.max.x &&
           outer.max.y >= inner.max.y;
}
double overlapArea(const BBox2& a, const BBox2& b) {
    double w = std::min(a.max.x, b.max.x) - std::max(a.min.x, b.min.x);
    double h = std::min(a.max.y, b.max.y) - std::max(a.min.y, b.min.y);
    return (w > 0 && h > 0) ? w * h : 0;
}
// Squared distance from p to the box (0 inside).
double distSq(const Vec2& p, const BBox2& b) {
    double dx = std::max({b.min.x - p.x, 0.0, p.x - b.max.x});
    double dy = std::max({b.min.y - p.y, 0.0, p.y - b.max.y});
    return dx * dx + dy * dy;
}

// Open addressing map EntityId -> leaf node index (or kInLarge). Linear probing with
// backward-shift deletion, so there are no tombstones.
class IdMap {
public:
    void clear() {
        slots_.clear();
        count_ = 0;
        mask_ = 0;
        shift_ = 64;
    }
    void reserve(std::size_t n) {
        std::size_t cap = 16;
        while (cap < n * 2)
            cap <<= 1;
        if (cap > slots_.size())
            rehash(cap);
    }
    std::size_t size() const { return count_; }
    void prefetch(std::uint64_t key) const {
        if (!slots_.empty())
            index::prefetch(&slots_[home(key)]);
    }

    // Returns the stored value or kNil.
    std::uint32_t find(std::uint64_t key) const {
        if (slots_.empty())
            return kNil;
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            const Slot& s = slots_[i];
            if (s.key == key)
                return s.val;
            if (s.key == 0)
                return kNil;
        }
    }
    // Sets key -> val. Returns true when the key was new.
    bool set(std::uint64_t key, std::uint32_t val) {
        if ((count_ + 1) * 2 > slots_.size())
            rehash(slots_.empty() ? 16 : slots_.size() * 2);
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (s.key == key) {
                s.val = val;
                return false;
            }
            if (s.key == 0) {
                s = {key, val};
                ++count_;
                return true;
            }
        }
    }
    // Sets only when absent; returns true when inserted.
    bool insertNew(std::uint64_t key, std::uint32_t val) {
        if ((count_ + 1) * 2 > slots_.size())
            rehash(slots_.empty() ? 16 : slots_.size() * 2);
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (s.key == key)
                return false;
            if (s.key == 0) {
                s = {key, val};
                ++count_;
                return true;
            }
        }
    }
    void erase(std::uint64_t key) {
        if (slots_.empty())
            return;
        std::size_t i = home(key);
        while (slots_[i].key != key) {
            if (slots_[i].key == 0)
                return;
            i = (i + 1) & mask_;
        }
        std::size_t hole = i;
        for (std::size_t j = (i + 1) & mask_; slots_[j].key != 0; j = (j + 1) & mask_) {
            std::size_t h = home(slots_[j].key);
            // Can slot j move into the hole? Only if its home is not cyclically in (hole, j].
            bool inRange = hole <= j ? (h > hole && h <= j) : (h > hole || h <= j);
            if (!inRange) {
                slots_[hole] = slots_[j];
                hole = j;
            }
        }
        slots_[hole] = Slot{};
        --count_;
    }
    template <class F>
    void forEach(F&& f) const {
        for (const Slot& s : slots_)
            if (s.key != 0)
                f(s.key, s.val);
    }

private:
    struct Slot {
        std::uint64_t key = 0;  // 0 = empty (EntityId 0 is the null id and never stored)
        std::uint32_t val = 0;
    };
    std::size_t home(std::uint64_t key) const {
        return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> shift_);
    }
    void rehash(std::size_t cap) {
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(cap, Slot{});
        mask_ = cap - 1;
        shift_ = 64;
        for (std::size_t c = cap; c > 1; c >>= 1)
            --shift_;
        count_ = 0;
        for (const Slot& s : old)
            if (s.key != 0)
                set(s.key, s.val);
    }

    std::vector<Slot> slots_;
    std::size_t count_ = 0;
    std::size_t mask_ = 0;
    unsigned shift_ = 64;
};

}  // namespace

struct SpatialIndex::Impl {
    struct Entry {
        BBox2 box;
        std::uint64_t ref;  // leaf: EntityId value; inner: child node index
    };
    struct Node {
        BBox2 box;
        std::uint32_t parent = kNil;
        std::uint16_t count = 0;
        std::uint16_t level = 0;   // 0 = leaf
        Entry e[kMaxEntries + 1];  // one spare slot so a node can overflow before it splits
    };

    std::vector<Node> nodes;
    std::vector<std::uint32_t> freeNodes;
    std::uint32_t root = kNil;
    std::size_t treeCount = 0;
    std::vector<IdBox> large;
    IdMap map;

    std::size_t size() const { return treeCount + large.size(); }

    void clear() {
        nodes.clear();
        freeNodes.clear();
        root = kNil;
        treeCount = 0;
        large.clear();
        map.clear();
    }

    std::uint32_t allocNode(std::uint16_t level) {
        std::uint32_t idx;
        if (!freeNodes.empty()) {
            idx = freeNodes.back();
            freeNodes.pop_back();
        } else {
            idx = static_cast<std::uint32_t>(nodes.size());
            nodes.emplace_back();
        }
        Node& n = nodes[idx];
        n.box = BBox2{};
        n.parent = kNil;
        n.count = 0;
        n.level = level;
        return idx;
    }
    void freeNode(std::uint32_t idx) { freeNodes.push_back(idx); }

    BBox2 boxOfEntries(const Node& n) const {
        BBox2 b;
        for (std::uint32_t i = 0; i < n.count; ++i)
            b = unite(b, n.e[i].box);
        return b;
    }

    int indexInParent(std::uint32_t parent, std::uint32_t child) const {
        const Node& p = nodes[parent];
        for (std::uint32_t i = 0; i < p.count; ++i)
            if (p.e[i].ref == child)
                return static_cast<int>(i);
        return -1;
    }

    bool isLarge(const BBox2& b) const {
        if (treeCount < kLargeMinSize || root == kNil)
            return false;
        double rootArea = areaOf(nodes[root].box);
        return rootArea > 0 && areaOf(b) > kLargeFraction * rootArea;
    }

    // ---- insertion -------------------------------------------------------------------------

    std::uint32_t chooseNode(const BBox2& box, std::uint16_t level) const {
        std::uint32_t n = root;
        while (nodes[n].level > level) {
            const Node& node = nodes[n];
            std::uint32_t best = 0;
            double bestEnl = std::numeric_limits<double>::infinity();
            double bestMEnl = bestEnl;
            double bestArea = bestEnl;
            for (std::uint32_t i = 0; i < node.count; ++i) {
                const BBox2& cb = node.e[i].box;
                BBox2 u = unite(cb, box);
                double area = areaOf(cb);
                double enl = areaOf(u) - area;
                double menl = marginOf(u) - marginOf(cb);
                if (enl < bestEnl || (enl == bestEnl &&
                                      (menl < bestMEnl || (menl == bestMEnl && area < bestArea)))) {
                    best = i;
                    bestEnl = enl;
                    bestMEnl = menl;
                    bestArea = area;
                }
            }
            n = static_cast<std::uint32_t>(node.e[best].ref);
        }
        return n;
    }

    // Splits the overflowing node `idx` (count == kMaxEntries + 1); returns the new sibling.
    std::uint32_t split(std::uint32_t idx) {
        constexpr std::uint32_t total = kMaxEntries + 1;
        Entry tmp[total];
        std::uint16_t level = nodes[idx].level;
        std::memcpy(tmp, nodes[idx].e, sizeof(tmp));

        auto lessBy = [](int axis, bool byMax) {
            return [axis, byMax](const Entry& a, const Entry& b) {
                double al = axis == 0 ? a.box.min.x : a.box.min.y;
                double ah = axis == 0 ? a.box.max.x : a.box.max.y;
                double bl = axis == 0 ? b.box.min.x : b.box.min.y;
                double bh = axis == 0 ? b.box.max.x : b.box.max.y;
                return byMax ? (ah != bh ? ah < bh : al < bl) : (al != bl ? al < bl : ah < bh);
            };
        };

        BBox2 pre[total];
        BBox2 suf[total];
        auto fillBounds = [&]() {
            BBox2 acc;
            for (std::uint32_t i = 0; i < total; ++i) {
                acc = unite(acc, tmp[i].box);
                pre[i] = acc;
            }
            acc = BBox2{};
            for (std::uint32_t i = total; i-- > 0;) {
                acc = unite(acc, tmp[i].box);
                suf[i] = acc;
            }
        };

        // Choose the split axis: the one whose distributions have the smallest margin sum.
        int bestAxis = 0;
        double bestMarginSum = std::numeric_limits<double>::infinity();
        for (int axis = 0; axis < 2; ++axis) {
            double sum = 0;
            for (int byMax = 0; byMax < 2; ++byMax) {
                std::sort(tmp, tmp + total, lessBy(axis, byMax != 0));
                fillBounds();
                for (std::uint32_t k = kMinEntries; k <= total - kMinEntries; ++k)
                    sum += marginOf(pre[k - 1]) + marginOf(suf[k]);
            }
            if (sum < bestMarginSum) {
                bestMarginSum = sum;
                bestAxis = axis;
            }
        }
        // On that axis choose the distribution with the least overlap (then area, then margin).
        bool bestByMax = false;
        std::uint32_t bestK = kMinEntries;
        double bestOverlap = std::numeric_limits<double>::infinity();
        double bestAreaSum = bestOverlap;
        double bestMargin = bestOverlap;
        for (int byMax = 0; byMax < 2; ++byMax) {
            std::sort(tmp, tmp + total, lessBy(bestAxis, byMax != 0));
            fillBounds();
            for (std::uint32_t k = kMinEntries; k <= total - kMinEntries; ++k) {
                double ov = overlapArea(pre[k - 1], suf[k]);
                double ar = areaOf(pre[k - 1]) + areaOf(suf[k]);
                double mg = marginOf(pre[k - 1]) + marginOf(suf[k]);
                if (ov < bestOverlap ||
                    (ov == bestOverlap &&
                     (ar < bestAreaSum || (ar == bestAreaSum && mg < bestMargin)))) {
                    bestOverlap = ov;
                    bestAreaSum = ar;
                    bestMargin = mg;
                    bestK = k;
                    bestByMax = byMax != 0;
                }
            }
        }
        std::sort(tmp, tmp + total, lessBy(bestAxis, bestByMax));

        std::uint32_t sib = allocNode(level);  // may reallocate `nodes`
        Node& a = nodes[idx];
        Node& b = nodes[sib];
        a.count = static_cast<std::uint16_t>(bestK);
        b.count = static_cast<std::uint16_t>(total - bestK);
        std::memcpy(a.e, tmp, bestK * sizeof(Entry));
        std::memcpy(b.e, tmp + bestK, (total - bestK) * sizeof(Entry));
        a.box = boxOfEntries(a);
        b.box = boxOfEntries(b);
        b.parent = a.parent;
        for (std::uint32_t i = 0; i < b.count; ++i) {
            if (level == 0)
                map.set(b.e[i].ref, sib);
            else
                nodes[static_cast<std::uint32_t>(b.e[i].ref)].parent = sib;
        }
        return sib;
    }

    // Adds an entry at `level` (0 = leaf entry, otherwise a child node) and repairs the path up.
    void insertEntry(const Entry& en, std::uint16_t level) {
        if (root == kNil) {
            root = allocNode(0);  // level must be 0 here: only leaf entries create a root
        }
        std::uint32_t cur = chooseNode(en.box, level);
        {
            Node& n = nodes[cur];
            n.e[n.count++] = en;
            n.box = unite(n.box, en.box);
            if (level == 0)
                map.set(en.ref, cur);
            else
                nodes[static_cast<std::uint32_t>(en.ref)].parent = cur;
        }
        while (true) {
            std::uint32_t sib = kNil;
            if (nodes[cur].count > kMaxEntries)
                sib = split(cur);
            std::uint32_t parent = nodes[cur].parent;
            if (parent == kNil) {
                if (sib != kNil) {
                    std::uint32_t nr = allocNode(static_cast<std::uint16_t>(nodes[cur].level + 1));
                    Node& r = nodes[nr];
                    r.e[0] = {nodes[cur].box, cur};
                    r.e[1] = {nodes[sib].box, sib};
                    r.count = 2;
                    r.box = unite(r.e[0].box, r.e[1].box);
                    nodes[cur].parent = nr;
                    nodes[sib].parent = nr;
                    root = nr;
                }
                return;
            }
            Node& p = nodes[parent];
            int pi = indexInParent(parent, cur);
            BBox2 before = p.e[pi].box;
            p.e[pi].box = nodes[cur].box;
            if (sib != kNil) {
                p.e[p.count++] = {nodes[sib].box, sib};
                nodes[sib].parent = parent;
                p.box = unite(p.box, nodes[sib].box);
            } else if (before.min.x == nodes[cur].box.min.x &&
                       before.min.y == nodes[cur].box.min.y &&
                       before.max.x == nodes[cur].box.max.x &&
                       before.max.y == nodes[cur].box.max.y) {
                return;  // nothing above changes
            }
            p.box = unite(p.box, nodes[cur].box);
            cur = parent;
        }
    }

    void insertLeaf(EntityId id, const BBox2& box) { insertEntry(Entry{box, id.value}, 0); }

    // ---- removal ---------------------------------------------------------------------------

    // Appends every leaf entry below `idx` to `out` and frees the subtree.
    void flatten(std::uint32_t idx, std::vector<Entry>& out) {
        std::vector<std::uint32_t> stack{idx};
        while (!stack.empty()) {
            std::uint32_t n = stack.back();
            stack.pop_back();
            const Node& node = nodes[n];
            for (std::uint32_t i = 0; i < node.count; ++i) {
                if (node.level == 0)
                    out.push_back(node.e[i]);
                else
                    stack.push_back(static_cast<std::uint32_t>(node.e[i].ref));
            }
            freeNode(n);
        }
    }

    // Removes the entry at position `pos` of leaf `leaf`, then condenses the tree.
    void removeFromLeaf(std::uint32_t leaf, std::uint32_t pos) {
        {
            Node& n = nodes[leaf];
            n.e[pos] = n.e[n.count - 1];
            --n.count;
        }
        std::vector<std::uint32_t> orphans;
        std::uint32_t cur = leaf;
        while (cur != root) {
            std::uint32_t parent = nodes[cur].parent;
            int pi = indexInParent(parent, cur);
            Node& p = nodes[parent];
            if (nodes[cur].count < kMinEntries) {
                p.e[pi] = p.e[p.count - 1];
                --p.count;
                orphans.push_back(cur);
            } else {
                nodes[cur].box = boxOfEntries(nodes[cur]);
                p.e[pi].box = nodes[cur].box;
            }
            p.box = boxOfEntries(p);
            cur = parent;
        }
        std::vector<Entry> reinsert;
        for (std::uint32_t o : orphans)
            flatten(o, reinsert);
        if (root != kNil && nodes[root].count > 0)
            nodes[root].box = boxOfEntries(nodes[root]);
        // Shrink the root.
        while (root != kNil) {
            Node& r = nodes[root];
            if (r.count == 0) {
                freeNode(root);
                root = kNil;
            } else if (r.level > 0 && r.count == 1) {
                std::uint32_t child = static_cast<std::uint32_t>(r.e[0].ref);
                freeNode(root);
                root = child;
                nodes[root].parent = kNil;
            } else {
                break;
            }
        }
        for (const Entry& e : reinsert)
            insertEntry(e, 0);
    }

    bool removeId(EntityId id) {
        std::uint32_t loc = map.find(id.value);
        if (loc == kNil)
            return false;
        map.erase(id.value);
        if (loc == kInLarge) {
            for (std::size_t i = 0; i < large.size(); ++i) {
                if (large[i].first == id) {
                    large[i] = large.back();
                    large.pop_back();
                    break;
                }
            }
            return true;
        }
        Node& n = nodes[loc];
        for (std::uint32_t i = 0; i < n.count; ++i) {
            if (n.e[i].ref == id.value) {
                --treeCount;
                removeFromLeaf(loc, i);
                return true;
            }
        }
        return true;  // unreachable if the map is consistent
    }

    bool insertId(EntityId id, const BBox2& box) {
        if (!id || !validBox(box) || map.find(id.value) != kNil)
            return false;
        if (isLarge(box)) {
            large.emplace_back(id, box);
            map.set(id.value, kInLarge);
        } else {
            insertLeaf(id, box);
            ++treeCount;
        }
        return true;
    }

    // ---- bulk load -------------------------------------------------------------------------

    void bulkLoad(std::span<const IdBox> items) {
        clear();
        // Valid items and the extent of their boxes.
        std::vector<std::uint32_t> keep;
        keep.reserve(items.size());
        map.reserve(items.size());
        BBox2 extent;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (!items[i].first || !validBox(items[i].second))
                continue;
            if (i + 16 < items.size())
                map.prefetch(items[i + 16].first.value);
            if (!map.insertNew(items[i].first.value, kInLarge))
                continue;  // repeated id: the first occurrence is kept
            keep.push_back(static_cast<std::uint32_t>(i));
            extent = unite(extent, items[i].second);
        }

        // Peel off the large boxes, then measure the rest again so the curve covers only them.
        std::vector<std::uint32_t> tree;
        tree.reserve(keep.size());
        double extentArea = areaOf(extent);
        bool peel = keep.size() >= kLargeMinSize && extentArea > 0;
        BBox2 treeExtent;
        for (std::uint32_t i : keep) {
            const BBox2& b = items[i].second;
            if (peel && areaOf(b) > kLargeFraction * extentArea) {
                large.emplace_back(items[i]);
            } else {
                tree.push_back(i);
                treeExtent = unite(treeExtent, b);
            }
        }
        keep = {};
        if (tree.empty())
            return;

        // Sort by Hilbert key of the box centre.
        double ext =
            std::max(treeExtent.max.x - treeExtent.min.x, treeExtent.max.y - treeExtent.min.y);
        double scale = ext > 0 ? 4294967295.0 / ext : 0.0;
        std::vector<KeyIndex> keyed;
        keyed.reserve(tree.size());
        const detail::HilbertTable& ht = detail::HilbertTable::instance();
        for (std::uint32_t i : tree) {
            const BBox2& b = items[i].second;
            double cx = (b.min.x + b.max.x) * 0.5 - treeExtent.min.x;
            double cy = (b.min.y + b.max.y) * 0.5 - treeExtent.min.y;
            auto q = [&](double v) {
                double s = v * scale;
                return static_cast<std::uint32_t>(s <= 0 ? 0
                                                         : (s >= 4294967295.0 ? 4294967295.0 : s));
            };
            keyed.push_back({ht.key(q(cx), q(cy)), i});
        }
        tree = {};
        sortByKey(keyed);

        // Count the nodes per level so the vector is allocated once.
        auto groupsFor = [](std::size_t n) { return (n + kPackFill - 1) / kPackFill; };
        std::size_t totalNodes = 0;
        for (std::size_t n = keyed.size();;) {
            std::size_t g = groupsFor(n);
            totalNodes += g;
            if (g == 1)
                break;
            n = g;
        }
        nodes.reserve(totalNodes);

        // Leaves. Entries are spread evenly so no node ends up under the minimum fill.
        std::uint32_t levelBegin = 0;
        {
            std::size_t n = keyed.size();
            std::size_t g = groupsFor(n);
            std::size_t base = n / g;
            std::size_t extra = n % g;
            std::size_t pos = 0;
            for (std::size_t gi = 0; gi < g; ++gi) {
                std::size_t cnt = base + (gi < extra ? 1 : 0);
                std::uint32_t idx = allocNode(0);
                Node& node = nodes[idx];
                BBox2 box;
                for (std::size_t j = 0; j < cnt; ++j, ++pos) {
                    if (pos + 32 < keyed.size())
                        prefetch(&items[keyed[pos + 32].index]);
                    if (pos + 16 < keyed.size())
                        map.prefetch(items[keyed[pos + 16].index].first.value);
                    const IdBox& it = items[keyed[pos].index];
                    map.set(it.first.value, idx);
                    node.e[node.count++] = Entry{it.second, it.first.value};
                    box = unite(box, it.second);
                }
                node.box = box;
                treeCount += node.count;
            }
        }
        keyed = {};
        std::uint32_t levelEnd = static_cast<std::uint32_t>(nodes.size());
        std::uint16_t level = 0;
        while (levelEnd - levelBegin > 1) {
            ++level;
            std::size_t n = levelEnd - levelBegin;
            std::size_t g = groupsFor(n);
            std::size_t base = n / g;
            std::size_t extra = n % g;
            std::uint32_t child = levelBegin;
            for (std::size_t gi = 0; gi < g; ++gi) {
                std::size_t cnt = base + (gi < extra ? 1 : 0);
                std::uint32_t idx = allocNode(level);
                BBox2 box;
                for (std::size_t j = 0; j < cnt; ++j, ++child) {
                    Node& node = nodes[idx];
                    node.e[node.count++] = Entry{nodes[child].box, child};
                    nodes[child].parent = idx;
                    box = unite(box, nodes[child].box);
                }
                nodes[idx].box = box;
            }
            levelBegin = levelEnd;
            levelEnd = static_cast<std::uint32_t>(nodes.size());
        }
        root = levelBegin;
        nodes[root].parent = kNil;
    }

    void collect(std::vector<IdBox>& out) const {
        out.reserve(size());
        for (const IdBox& l : large)
            out.push_back(l);
        if (root == kNil)
            return;
        std::vector<std::uint32_t> stack{root};
        while (!stack.empty()) {
            std::uint32_t n = stack.back();
            stack.pop_back();
            const Node& node = nodes[n];
            for (std::uint32_t i = 0; i < node.count; ++i) {
                if (node.level == 0)
                    out.emplace_back(EntityId{node.e[i].ref}, node.e[i].box);
                else
                    stack.push_back(static_cast<std::uint32_t>(node.e[i].ref));
            }
        }
    }
};

SpatialIndex::SpatialIndex() : impl_(new Impl) {}
SpatialIndex::~SpatialIndex() {
    delete impl_;
}
SpatialIndex::SpatialIndex(SpatialIndex&& o) noexcept : impl_(o.impl_) {
    o.impl_ = new Impl;
}
SpatialIndex& SpatialIndex::operator=(SpatialIndex&& o) noexcept {
    if (this != &o) {
        delete impl_;
        impl_ = o.impl_;
        o.impl_ = new Impl;
    }
    return *this;
}

void SpatialIndex::bulkLoad(std::span<const IdBox> items) {
    impl_->bulkLoad(items);
}

bool SpatialIndex::insert(EntityId id, const BBox2& box) {
    return impl_->insertId(id, box);
}

bool SpatialIndex::remove(EntityId id) {
    return impl_->removeId(id);
}

bool SpatialIndex::update(EntityId id, const BBox2& newBox) {
    Impl& im = *impl_;
    if (!id)
        return false;
    if (!validBox(newBox)) {
        im.removeId(id);
        return false;
    }
    std::uint32_t loc = im.map.find(id.value);
    if (loc == kNil)
        return im.insertId(id, newBox);
    if (loc == kInLarge) {
        if (!im.isLarge(newBox)) {
            im.removeId(id);
            return im.insertId(id, newBox);
        }
        for (IdBox& l : im.large)
            if (l.first == id) {
                l.second = newBox;
                break;
            }
        return true;
    }
    // In the tree: replace in place when the leaf still covers the new box.
    Impl::Node& leaf = im.nodes[loc];
    if (covers(leaf.box, newBox) && !im.isLarge(newBox)) {
        for (std::uint32_t i = 0; i < leaf.count; ++i)
            if (leaf.e[i].ref == id.value) {
                leaf.e[i].box = newBox;
                return true;
            }
    }
    im.removeId(id);
    return im.insertId(id, newBox);
}

bool SpatialIndex::contains(EntityId id) const {
    return id && impl_->map.find(id.value) != kNil;
}

BBox2 SpatialIndex::boxOf(EntityId id) const {
    const Impl& im = *impl_;
    std::uint32_t loc = id ? im.map.find(id.value) : kNil;
    if (loc == kNil)
        return BBox2{};
    if (loc == kInLarge) {
        for (const IdBox& l : im.large)
            if (l.first == id)
                return l.second;
        return BBox2{};
    }
    const Impl::Node& n = im.nodes[loc];
    for (std::uint32_t i = 0; i < n.count; ++i)
        if (n.e[i].ref == id.value)
            return n.e[i].box;
    return BBox2{};
}

void SpatialIndex::queryImpl(const BBox2& window, Visitor visit) const {
    const Impl& im = *impl_;
    if (window.empty())
        return;
    for (const IdBox& l : im.large)
        if (overlaps(l.second, window) && !visit(l.first, l.second))
            return;
    if (im.root == kNil)
        return;
    std::uint32_t stack[64];
    int top = 0;
    stack[top++] = im.root;
    while (top > 0) {
        const Impl::Node& node = im.nodes[stack[--top]];
        if (node.level == 0) {
            for (std::uint32_t i = 0; i < node.count; ++i) {
                const Impl::Entry& e = node.e[i];
                if (overlaps(e.box, window) && !visit(EntityId{e.ref}, e.box))
                    return;
            }
        } else {
            for (std::uint32_t i = 0; i < node.count; ++i) {
                const Impl::Entry& e = node.e[i];
                if (overlaps(e.box, window))
                    stack[top++] = static_cast<std::uint32_t>(e.ref);
            }
        }
    }
}

void SpatialIndex::queryPointImpl(Vec2 p, double radius, Visitor visit) const {
    if (!(radius >= 0))
        return;
    BBox2 window;
    window.min = {p.x - radius, p.y - radius};
    window.max = {p.x + radius, p.y + radius};
    double r2 = radius * radius;
    queryImpl(window, [&](EntityId id, const BBox2& b) {
        if (distSq(p, b) <= r2)
            return visit(id, b);
        return true;
    });
}

std::vector<Neighbor> SpatialIndex::nearest(Vec2 p, std::size_t k, double maxDist) const {
    std::vector<Neighbor> result;
    const Impl& im = *impl_;
    if (k == 0 || !(maxDist >= 0))
        return result;
    double max2 = maxDist * maxDist;
    struct Item {
        double d2;
        bool isEntry;
        std::uint64_t ref;  // node index or entity id
    };
    // Nodes before entries at equal distance so that ties between entries resolve by id.
    auto worse = [](const Item& a, const Item& b) {
        if (a.d2 != b.d2)
            return a.d2 > b.d2;
        if (a.isEntry != b.isEntry)
            return a.isEntry;
        return a.isEntry && a.ref > b.ref;
    };
    std::priority_queue<Item, std::vector<Item>, decltype(worse)> heap(worse);
    for (const IdBox& l : im.large) {
        double d2 = distSq(p, l.second);
        if (d2 <= max2)
            heap.push({d2, true, l.first.value});
    }
    if (im.root != kNil) {
        double d2 = distSq(p, im.nodes[im.root].box);
        if (d2 <= max2)
            heap.push({d2, false, im.root});
    }
    while (!heap.empty() && result.size() < k) {
        Item it = heap.top();
        heap.pop();
        if (it.isEntry) {
            result.push_back({EntityId{it.ref}, std::sqrt(it.d2)});
            continue;
        }
        const Impl::Node& node = im.nodes[static_cast<std::uint32_t>(it.ref)];
        for (std::uint32_t i = 0; i < node.count; ++i) {
            const Impl::Entry& e = node.e[i];
            double d2 = distSq(p, e.box);
            if (d2 <= max2)
                heap.push({d2, node.level == 0, e.ref});
        }
    }
    return result;
}

std::vector<EntityId> SpatialIndex::queryIds(const BBox2& window) const {
    std::vector<EntityId> out;
    queryImpl(window, [&](EntityId id, const BBox2&) {
        out.push_back(id);
        return true;
    });
    return out;
}

std::size_t SpatialIndex::size() const {
    return impl_->size();
}

void SpatialIndex::clear() {
    impl_->clear();
}

void SpatialIndex::rebalance() {
    std::vector<IdBox> all;
    impl_->collect(all);
    impl_->bulkLoad(all);
}

BBox2 SpatialIndex::bounds() const {
    const Impl& im = *impl_;
    BBox2 b;
    for (const IdBox& l : im.large)
        b = unite(b, l.second);
    if (im.root != kNil)
        b = unite(b, im.nodes[im.root].box);
    return b;
}

std::size_t SpatialIndex::largeCount() const {
    return impl_->large.size();
}

bool SpatialIndex::validate() const {
    const Impl& im = *impl_;
    std::size_t leafEntries = 0;
    if (im.root != kNil) {
        if (im.nodes[im.root].parent != kNil)
            return false;
        std::vector<std::uint32_t> stack{im.root};
        while (!stack.empty()) {
            std::uint32_t n = stack.back();
            stack.pop_back();
            const Impl::Node& node = im.nodes[n];
            if (node.count > kMaxEntries || node.count == 0)
                return false;
            if (n != im.root && node.count < kMinEntries)
                return false;
            if (!covers(node.box, im.boxOfEntries(node)))
                return false;
            for (std::uint32_t i = 0; i < node.count; ++i) {
                const Impl::Entry& e = node.e[i];
                if (node.level == 0) {
                    if (im.map.find(e.ref) != n)
                        return false;
                    ++leafEntries;
                } else {
                    std::uint32_t c = static_cast<std::uint32_t>(e.ref);
                    const Impl::Node& child = im.nodes[c];
                    if (child.parent != n || child.level + 1 != node.level)
                        return false;
                    if (!covers(e.box, child.box) || !covers(node.box, e.box))
                        return false;
                    stack.push_back(c);
                }
            }
        }
    }
    if (leafEntries != im.treeCount)
        return false;
    for (const IdBox& l : im.large)
        if (im.map.find(l.first.value) != kInLarge)
            return false;
    return im.map.size() == im.treeCount + im.large.size();
}

}  // namespace tbcad::index
