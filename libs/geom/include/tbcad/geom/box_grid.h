#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "tbcad/core/math.h"

namespace tbcad::geom {

// Finds the boxes that meet a query box without comparing with all of them: a uniform grid sized
// to the number of boxes, boxes that span many cells kept in a short list checked every time.
class BoxGrid {
public:
    explicit BoxGrid(const std::vector<BBox2>& boxes) : boxes_(boxes), seen_(boxes.size(), 0) {
        BBox2 all;
        for (const BBox2& b : boxes)
            all.merge(b);
        if (all.empty() || boxes.empty())
            return;
        const double w = std::max(all.width(), 1e-9), h = std::max(all.height(), 1e-9);
        cell_ = std::sqrt(w * h / std::max<double>(4.0 * boxes.size(), 1.0));
        // A thin drawing: keep at least a few cells across its short side.
        cell_ = std::max(cell_, std::max(w, h) / 2.0e6);
        cell_ = std::max(cell_, 1e-9);
        origin_ = all.min;
        nx_ = static_cast<long>(w / cell_) + 1;
        ny_ = static_cast<long>(h / cell_) + 1;
        cells_.assign(static_cast<std::size_t>(nx_ * ny_), {});
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            const auto [x0, y0, x1, y1] = range(boxes[i]);
            if ((x1 - x0 + 1) * (y1 - y0 + 1) > 64) {
                large_.push_back(i);
                continue;
            }
            for (long y = y0; y <= y1; ++y)
                for (long x = x0; x <= x1; ++x)
                    cells_[static_cast<std::size_t>(y * nx_ + x)].push_back(i);
        }
    }

    // Calls f(i) once for each box that may meet `q`.
    template <class F>
    void query(const BBox2& q, F&& f) const {
        ++stamp_;
        auto visit = [&](std::size_t i) {
            if (seen_[i] == stamp_)
                return;
            seen_[i] = stamp_;
            if (boxes_[i].intersects(q))
                f(i);
        };
        for (std::size_t i : large_)
            visit(i);
        if (cells_.empty())
            return;
        const auto [x0, y0, x1, y1] = range(q);
        for (long y = y0; y <= y1; ++y)
            for (long x = x0; x <= x1; ++x)
                for (std::size_t i : cells_[static_cast<std::size_t>(y * nx_ + x)])
                    visit(i);
    }

private:
    std::array<long, 4> range(const BBox2& b) const {
        auto cx = [&](double v) { return std::clamp(static_cast<long>(std::floor((v - origin_.x) / cell_)), 0L, nx_ - 1); };
        auto cy = [&](double v) { return std::clamp(static_cast<long>(std::floor((v - origin_.y) / cell_)), 0L, ny_ - 1); };
        return {cx(b.min.x), cy(b.min.y), cx(b.max.x), cy(b.max.y)};
    }

    const std::vector<BBox2>& boxes_;
    mutable std::vector<unsigned> seen_;
    mutable unsigned stamp_ = 0;
    Vec2 origin_{};
    double cell_ = 1;
    long nx_ = 0, ny_ = 0;
    std::vector<std::vector<std::size_t>> cells_{};
    std::vector<std::size_t> large_{};
};


}  // namespace tbcad::geom
