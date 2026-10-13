#pragma once

#include <vector>

#include "tbcad/cmd/input.h"
#include "tbcad/drafting/env.h"
#include "tbcad/drafting/osnap.h"

namespace tbcad::drafting {

// Order values for PointFilterChain / UiRegistry::addPointFilter: osnap > polar/ortho > grid.
inline constexpr int kDynamicInputOrder = 50;  // typed field values beat every snap
inline constexpr int kOsnapOrder = 100;
inline constexpr int kDynamicUcsOrder = 150;
inline constexpr int kPolarOrder = 200;
inline constexpr int kGridSnapOrder = 300;

// Snaps to the best object snap candidate under the cursor when OSNAP is on. FilterFeedback:
// marker = the mode name ("endpoint" ...), markerPos, tooltip = the mode label.
class OsnapFilter final : public cmd::PointFilter {
public:
    explicit OsnapFilter(const DraftingEnv* env, const SnapGeometryRegistry* providers = nullptr);
    std::optional<Vec2> filter(const cmd::PointQuery& query, Vec2 raw,
                               cmd::FilterFeedback& feedback) override;

    // Tab: moves to the next candidate at the current cursor position (wraps around).
    void cycle();
    // Candidates found by the last filter() call, best first.
    const std::vector<SnapCandidate>& candidates() const { return candidates_; }

private:
    const DraftingEnv* env_;
    const SnapGeometryRegistry* providers_;
    std::vector<SnapCandidate> candidates_{};
    std::size_t cycle_ = 0;
};

// Dynamic UCS: while DYNUCS is on and the cursor is within the pick aperture of a straight edge
// (a line or a straight polyline segment), DraftingEnv::dynamicUcs is that edge's frame, so the
// polar / ortho and grid snap filters that run after this one work along the edge. Never moves
// the point itself.
class DynamicUcsFilter final : public cmd::PointFilter {
public:
    explicit DynamicUcsFilter(const DraftingEnv* env) : env_(env) {}
    std::optional<Vec2> filter(const cmd::PointQuery& query, Vec2 raw,
                               cmd::FilterFeedback& feedback) override;

private:
    const DraftingEnv* env_;
};

// Ortho (locks the direction from the base point to the nearest axis) or polar tracking (snaps
// the direction to the increment angles when within the tolerance and shows the tracking ray
// and a "distance < angle" tooltip). Needs a base point; with SNAP on the distance is rounded to
// the snap spacing as well. FilterFeedback::guide is the tracking ray.
class PolarOrthoFilter final : public cmd::PointFilter {
public:
    explicit PolarOrthoFilter(DraftingEnv* env) : env_(env) {}
    std::optional<Vec2> filter(const cmd::PointQuery& query, Vec2 raw,
                               cmd::FilterFeedback& feedback) override;

private:
    DraftingEnv* env_;
};

// SNAP: rounds the point to the snap spacing, with the grid origin at (0,0).
class GridSnapFilter final : public cmd::PointFilter {
public:
    explicit GridSnapFilter(const DraftingEnv* env) : env_(env) {}
    std::optional<Vec2> filter(const cmd::PointQuery& query, Vec2 raw,
                               cmd::FilterFeedback& feedback) override;

private:
    const DraftingEnv* env_;
};

}  // namespace tbcad::drafting
