#pragma once

#include <vector>

#include "tbcad/cmd/input.h"
#include "tbcad/drafting/env.h"
#include "tbcad/drafting/osnap.h"

namespace tbcad::drafting {

// Order values for PointFilterChain / UiRegistry::addPointFilter: osnap > polar/ortho > grid.
inline constexpr int kDynamicInputOrder = 50;  // typed field values beat every snap
inline constexpr int kOtrackAcquireOrder =
    99;  // watches the osnap candidates, never moves the point
inline constexpr int kOsnapOrder = 100;
inline constexpr int kOtrackOrder = 120;  // after osnap, before polar
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

// Object snap tracking (OTRACK). Hovering an osnap candidate acquires its point (at most three are
// kept); while moving along a polar / ortho direction through an acquired point the cursor snaps
// to that ray, and to the intersection of two rays (acquired point or the base point's polar
// ray). FilterFeedback::guide is the tracking ray.
class OtrackFilter final : public cmd::PointFilter {
public:
    OtrackFilter(DraftingEnv* env, const OsnapFilter* osnap) : env_(env), osnap_(osnap) {}
    std::optional<Vec2> filter(const cmd::PointQuery& query, Vec2 raw,
                               cmd::FilterFeedback& feedback) override;

    // Adds `p` as an acquired point (no-op when already there); the oldest is dropped past three.
    void acquire(Vec2 p, double tolerance);
    void clear() {
        acquired_.clear();
        rays_.clear();
    }
    const std::vector<Vec2>& acquired() const { return acquired_; }
    // Rays (origin, end) the last filter() call snapped to; empty when it did not.
    const std::vector<std::pair<Vec2, Vec2>>& rays() const { return rays_; }

private:
    DraftingEnv* env_;
    [[maybe_unused]] const OsnapFilter* osnap_;
    std::vector<Vec2> acquired_{};
    std::vector<std::pair<Vec2, Vec2>> rays_{};
};

// Order kOtrackAcquireOrder: feeds the osnap candidates under the cursor to an OtrackFilter and
// forgets its points on a click. Never returns a point.
class OtrackAcquirer final : public cmd::PointFilter {
public:
    OtrackAcquirer(DraftingEnv* env, const OsnapFilter* osnap, OtrackFilter* tracker)
        : env_(env), osnap_(osnap), tracker_(tracker) {}
    std::optional<Vec2> filter(const cmd::PointQuery& query, Vec2 raw,
                               cmd::FilterFeedback& feedback) override;

private:
    DraftingEnv* env_;
    const OsnapFilter* osnap_;
    OtrackFilter* tracker_;
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
