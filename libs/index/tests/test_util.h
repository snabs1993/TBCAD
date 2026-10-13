#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <random>
#include <vector>

#include "tbcad/index/spatial_index.h"

// Full-size acceptance tests and timing gates run only in optimised builds without sanitizers;
// elsewhere the same tests run at a reduced size so the debug and ASan jobs stay quick.
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_UNDEFINED__)
#if defined(__has_feature)
#if !__has_feature(address_sanitizer) && !__has_feature(undefined_behavior_sanitizer)
#define TBCAD_FULL_SCALE 1
#endif
#else
#define TBCAD_FULL_SCALE 1
#endif
#endif

namespace tbcad::index::test {

#ifdef TBCAD_FULL_SCALE
inline constexpr bool kFullScale = true;
#else
inline constexpr bool kFullScale = false;
#endif

// Boxes that mix a 100 km site with millimetre details, zero-width lines and a few huge boxes.
struct BoxGen {
    std::mt19937_64 rng;
    explicit BoxGen(std::uint64_t seed) : rng(seed) {}

    double uni(double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng); }

    BBox2 next() {
        double kind = uni(0, 1);
        double cx = uni(-5e7, 5e7);
        double cy = uni(-5e7, 5e7);
        if (kind < 0.5) {  // clustered detail
            cx = std::fmod(cx, 1e4);
            cy = std::fmod(cy, 1e4);
        }
        double w = std::exp(uni(std::log(1e-3), std::log(1e4)));
        double h = std::exp(uni(std::log(1e-3), std::log(1e4)));
        if (kind > 0.9 && kind < 0.95)
            h = 0;  // horizontal line
        else if (kind >= 0.95 && kind < 0.99)
            w = 0;  // vertical line
        else if (kind >= 0.999) {
            w = uni(2e7, 1e8);
            h = uni(2e7, 1e8);
        }
        return BBox2{{cx - w / 2, cy - h / 2}, {cx + w / 2, cy + h / 2}};
    }

    BBox2 queryBox() {
        double cx = uni(-5e7, 5e7);
        double cy = uni(-5e7, 5e7);
        if (uni(0, 1) < 0.5) {
            cx = std::fmod(cx, 1e4);
            cy = std::fmod(cy, 1e4);
        }
        double w = std::exp(uni(std::log(1e-2), std::log(2e5)));
        double h = std::exp(uni(std::log(1e-2), std::log(2e5)));
        return BBox2{{cx - w / 2, cy - h / 2}, {cx + w / 2, cy + h / 2}};
    }
};

using Model = std::map<std::uint64_t, BBox2>;

// Brute-force reference with O(1) updates, for the big randomised test.
struct FlatModel {
    std::vector<BBox2> box;
    std::vector<char> alive;
    std::size_t count = 0;
    bool has(std::uint64_t id) const { return id < alive.size() && alive[id]; }
    void set(std::uint64_t id, const BBox2& b) {
        if (id >= alive.size()) {
            alive.resize(id + 1, 0);
            box.resize(id + 1);
        }
        count += alive[id] ? 0 : 1;
        alive[id] = 1;
        box[id] = b;
    }
    bool erase(std::uint64_t id) {
        if (!has(id))
            return false;
        alive[id] = 0;
        --count;
        return true;
    }
    // Answers many queries in one pass over the boxes (cache friendly); each result is ascending.
    std::vector<std::vector<std::uint64_t>> queryAll(const std::vector<BBox2>& windows) const {
        std::vector<std::vector<std::uint64_t>> out(windows.size());
        for (std::size_t i = 0; i < alive.size(); ++i) {
            if (!alive[i])
                continue;
            const BBox2& b = box[i];
            for (std::size_t q = 0; q < windows.size(); ++q) {
                const BBox2& w = windows[q];
                if (b.min.x <= w.max.x && w.min.x <= b.max.x && b.min.y <= w.max.y &&
                    w.min.y <= b.max.y)
                    out[q].push_back(i);
            }
        }
        return out;
    }
    std::vector<std::uint64_t> query(const BBox2& w) const {
        std::vector<std::uint64_t> out;
        for (std::size_t i = 0; i < alive.size(); ++i) {
            const BBox2& b = box[i];
            if (alive[i] && b.min.x <= w.max.x && w.min.x <= b.max.x && b.min.y <= w.max.y &&
                w.min.y <= b.max.y)
                out.push_back(i);
        }
        return out;
    }
};

inline std::vector<std::uint64_t> bruteQuery(const Model& m, const BBox2& w) {
    std::vector<std::uint64_t> out;
    for (const auto& [id, b] : m)
        if (b.intersects(w))
            out.push_back(id);
    return out;  // ascending ids (map order)
}

inline std::vector<std::uint64_t> indexQuery(const SpatialIndex& idx, const BBox2& w) {
    std::vector<std::uint64_t> out;
    idx.query(w, [&](EntityId id, const BBox2&) { out.push_back(id.value); });
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace tbcad::index::test
