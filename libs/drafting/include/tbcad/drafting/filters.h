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

// The direction (unit vector, world) ORTHO or polar tracking would give the cursor at `raw` from
// the query's base point, or nullopt when neither applies there. A typed distance follows it.
std::optional<Vec2> orthoPolarDirection(const DraftingEnv* env, const cmd::PointQuery& query,
                                        Vec2 raw);

// Snaps to the best object snap candidate under the cursor when OSNAP is on. FilterFeedback:
// marker = the mode name ("endpoint" ...), markerPos, tooltip = the mode label. With PARallel on
// and a base point, resting on a straight edge acquires its direction; moving the rubber band
// close to parallel to it then snaps onto the parallel line through the base point. With
// EXTension on, resting on the end of a line, an open polyline's straight end segment or an arc
// acquires that end (two at most); the cursor then snaps onto the line's extension past the end,
// onto the rest of the arc's circle, or onto where two acquired extensions cross.
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
    std::optional<Vec2> parallel(const cmd::PointQuery& query, Vec2 raw, double aperture,
                                 cmd::FilterFeedback& feedback);
    std::optional<Vec2> extension(const cmd::PointQuery& query, Vec2 raw, double aperture,
                                  cmd::FilterFeedback& feedback);

    // An acquired EXTension end: a line end (dir = outward unit direction) or an arc end.
    struct ExtEnd {
        Vec2 end{};
        Vec2 dir{};
        bool arc = false;
        Vec2 center{};
        double radius = 0, startAngle = 0, endAngle = 0;
    };

    const DraftingEnv* env_;
    const SnapGeometryRegistry* providers_;
    std::vector<SnapCandidate> candidates_{};
    std::size_t cycle_ = 0;
    // PARallel: the straight edge the cursor last rested on (kept until the point is picked).
    std::optional<std::pair<Vec2, Vec2>> parallelEdge_{};
    std::vector<ExtEnd> extEnds_{};  // newest last, at most two; cleared by a click
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
