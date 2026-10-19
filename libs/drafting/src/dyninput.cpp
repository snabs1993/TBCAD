#include "tbcad/drafting/dyninput.h"

#include <algorithm>
#include <cmath>

#include "tbcad/core/units.h"
#include "tbcad/drafting/filters.h"
#include "tbcad/drafting/settings.h"

namespace tbcad::drafting {

namespace {

LengthUnit unitOf(const DraftingEnv* env) {
    return env && env->doc ? env->doc->header().units : LengthUnit::Millimeter;
}

bool sameBase(const std::optional<Vec2>& a, const std::optional<Vec2>& b) {
    if (a.has_value() != b.has_value())
        return false;
    return !a || (a->x == b->x && a->y == b->y);
}

}  // namespace

bool DynamicInput::enabled() const {
    if (!env_ || !env_->host)
        return true;
    return DraftingSettings::read(*env_->host).dynInput;
}

void DynamicInput::clearLocks() {
    dist_.reset();
    angle_.reset();
    x_.reset();
    y_.reset();
}

void DynamicInput::reset() {
    clearLocks();
    base_.reset();
    seen_ = false;
    focused_ = -1;
}

std::optional<Vec2> DynamicInput::filter(const cmd::PointQuery& query, Vec2 raw,
                                         cmd::FilterFeedback&) {
    seen_ = true;
    if (!sameBase(base_, query.base)) {
        clearLocks();
        focused_ = -1;
        base_ = query.base;
    }
    if (!enabled() || !hasLocks())
        return std::nullopt;

    Vec2 p = raw;
    if (query.base) {
        const Vec2 b = *query.base;
        const Vec2 v = raw - b;
        if (dist_ && angle_) {
            p = b + Vec2{std::cos(*angle_), std::sin(*angle_)} * *dist_;
        } else if (dist_) {
            // A typed distance goes the way ORTHO or polar tracking points the cursor.
            Vec2 dir{1, 0};
            if (auto o = orthoPolarDirection(env_, query, raw)) {
                dir = *o;
            } else if (const double d = length(v); d > 1e-12) {
                dir = v * (1.0 / d);
            }
            p = b + dir * *dist_;
        } else if (angle_) {
            const Vec2 dir{std::cos(*angle_), std::sin(*angle_)};
            p = b + dir * std::max(0.0, dot(v, dir));
        } else {
            return std::nullopt;  // X / Y locks do not apply with a base point
        }
    } else {
        if (x_)
            p.x = *x_;
        if (y_)
            p.y = *y_;
        if (!x_ && !y_)
            return std::nullopt;
    }
    if (query.click) {
        clearLocks();
        focused_ = -1;
    }
    return p;
}

std::vector<cmd::CursorField> DynamicInput::fields(const cmd::FilterFeedback& fb) const {
    std::vector<cmd::CursorField> out;
    if (!seen_ || !enabled() || !fb.hasPoint)
        return out;
    UnitFormat uf;
    uf.unit = unitOf(env_);
    uf.trimTrailingZeros = true;
    auto len = [&](double mm) { return formatLength(mm, 2, uf); };
    auto ang = [](double rad) {
        double deg = rad * 180.0 / kPi;
        deg = std::fmod(deg, 360.0);
        if (deg < 0)
            deg += 360.0;
        if (std::abs(deg) < 5e-3 || std::abs(deg - 360.0) < 5e-3)
            deg = 0;
        return formatDouble(std::round(deg * 1e2) / 1e2);
    };
    if (fb.base) {
        const Vec2 v = fb.point - *fb.base;
        std::string label = "Distance";
        if (const std::string_view unit = unitSuffix(uf.unit); !unit.empty())
            label += " (" + std::string(unit) + ")";  // "Distance (mm)"
        out.push_back({label, dist_ ? len(*dist_) : len(length(v)), dist_.has_value()});
        out.push_back(
            {"Angle", angle_ ? ang(*angle_) : ang(std::atan2(v.y, v.x)), angle_.has_value()});
    } else {
        out.push_back({"X", x_ ? len(*x_) : len(fb.point.x), x_.has_value()});
        out.push_back({"Y", y_ ? len(*y_) : len(fb.point.y), y_.has_value()});
    }
    return out;
}

std::optional<Vec2> DynamicInput::lockedPoint(const cmd::FilterFeedback& fb) const {
    if (!seen_ || !enabled() || !fb.hasPoint)
        return std::nullopt;
    if (fb.base) {
        if (dist_ && angle_)
            return *fb.base + Vec2{std::cos(*angle_), std::sin(*angle_)} * *dist_;
        return std::nullopt;
    }
    if (x_ && y_)
        return Vec2{*x_, *y_};
    return std::nullopt;
}

void DynamicInput::focus(int index) {
    focused_ = (seen_ && index >= 0 && index < 2) ? index : -1;
}

bool DynamicInput::focusNext() {
    if (!seen_ || !enabled())
        return false;
    focused_ = focused_ < 0 ? 0 : (focused_ + 1) % 2;
    return true;
}

bool DynamicInput::edit(std::size_t index, std::string_view text) {
    if (!seen_ || index > 1)
        return false;
    std::optional<double>* slot =
        base_ ? (index == 0 ? &dist_ : &angle_) : (index == 0 ? &x_ : &y_);
    std::string_view t = text;
    while (!t.empty() && t.front() == ' ')
        t.remove_prefix(1);
    while (!t.empty() && t.back() == ' ')
        t.remove_suffix(1);
    if (t.empty()) {
        slot->reset();
        return true;
    }
    if (base_ && index == 1) {
        auto a = parseAngle(t, AngleUnit::Degrees);
        if (!a)
            return false;
        *slot = a.value();
        return true;
    }
    auto l = parseLength(t, unitOf(env_));
    if (!l)
        return false;
    if (base_ && index == 0 && l.value() < 0)
        return false;  // a distance is not negative
    *slot = l.value();
    return true;
}

}  // namespace tbcad::drafting
