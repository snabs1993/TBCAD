#pragma once

#include <optional>

#include "tbcad/cmd/input.h"
#include "tbcad/drafting/env.h"

namespace tbcad::drafting {

// Dynamic input: fields next to the cursor that show the distance and angle from the base point
// (X and Y while there is no base point) and that the user can type into. A typed value locks that
// part of the point; the rest keeps following the cursor. Locks are forgotten when the base point
// changes, when the point is clicked and when the prompt ends (reset()).
//
// As a point filter it comes before object snap, so a typed value beats a snap. It does nothing
// while nothing is locked. As a CursorFieldProvider it feeds the window.
class DynamicInput final : public cmd::PointFilter, public cmd::CursorFieldProvider {
public:
    explicit DynamicInput(const DraftingEnv* env) : env_(env) {}

    // cmd::PointFilter
    std::optional<Vec2> filter(const cmd::PointQuery& query, Vec2 raw,
                               cmd::FilterFeedback& feedback) override;

    // cmd::CursorFieldProvider
    std::vector<cmd::CursorField> fields(const cmd::FilterFeedback& feedback) const override;
    int focused() const override { return focused_; }
    void focus(int index) override;
    bool focusNext() override;
    bool edit(std::size_t index, std::string_view text) override;
    std::optional<Vec2> lockedPoint(const cmd::FilterFeedback& feedback) const override;
    void reset() override;

    // The `drafting.dynInput` setting (on unless switched off).
    bool enabled() const;
    bool hasLocks() const { return dist_ || angle_ || x_ || y_; }

private:
    void clearLocks();

    const DraftingEnv* env_;
    // mm, radians
    std::optional<double> dist_{}, angle_{}, x_{}, y_{};
    std::optional<Vec2> base_{};  // base the locks belong to
    bool seen_ = false;           // a point prompt has been filtered since the last reset
    int focused_ = -1;
};

}  // namespace tbcad::drafting
