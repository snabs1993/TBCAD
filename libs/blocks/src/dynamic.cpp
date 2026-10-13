#include "tbcad/blocks/dynamic.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include "tbcad/blocks/blocks.h"
#include "tbcad/geom/geom.h"

namespace tbcad::blocks {

namespace {

Vec2 unitOf(Vec2 a, Vec2 b) {
    Vec2 d = b - a;
    double len = std::hypot(d.x, d.y);
    return len > 0 ? Vec2{d.x / len, d.y / len} : Vec2{1, 0};
}

double clampToLimits(const DynParam& p, double v) {
    if (p.kind == DynParam::Kind::Flip)
        return v >= 0.5 ? 1.0 : 0.0;
    if (p.kind == DynParam::Kind::Visibility) {
        double top = p.states.empty() ? 0.0 : static_cast<double>(p.states.size() - 1);
        return std::clamp(std::round(v), 0.0, top);
    }
    if (p.max > p.min)
        v = std::clamp(v, p.min, p.max);
    return v;
}

bool inFrame(Vec2 p, const DynAction& a) {
    const double e = 1e-9;
    return p.x >= std::min(a.frameMin.x, a.frameMax.x) - e &&
           p.x <= std::max(a.frameMin.x, a.frameMax.x) + e &&
           p.y >= std::min(a.frameMin.y, a.frameMax.y) - e &&
           p.y <= std::max(a.frameMin.y, a.frameMax.y) + e;
}

void applyTransform(Entity& e, const Transform2& t) {
    if (auto r = geom::transformed(e, t))
        e = std::move(r).value();
}

// Moves the points of `e` inside the frame by `d`. Kinds without separate points move as a whole
// when their reference point (first grip) is inside the frame.
void stretchEntity(Entity& e, const DynAction& a, Vec2 d) {
    auto moveAll = [&] { applyTransform(e, Transform2::translate(d)); };
    if (auto* l = entityAs<LineData>(e)) {
        if (inFrame(l->a, a))
            l->a += d;
        if (inFrame(l->b, a))
            l->b += d;
        return;
    }
    if (auto* pl = entityAs<PolylineData>(e)) {
        for (PolyVertex& v : pl->v)
            if (inFrame(v.p, a))
                v.p += d;
        return;
    }
    if (auto* sp = entityAs<SplineData>(e)) {
        auto& pts = sp->fit.empty() ? sp->control : sp->fit;
        for (Vec2& p : pts)
            if (inFrame(p, a))
                p += d;
        return;
    }
    auto grips = geom::gripPoints(e);
    if (!grips.empty() && inFrame(grips.front(), a))
        moveAll();
}

}  // namespace

double dynDefault(const DynParam& p) {
    if (p.kind == DynParam::Kind::Linear)
        return distance(p.base, p.end);
    return 0;
}

int dynFind(const DynamicDef& def, std::string_view name) {
    auto lower = [](std::string_view s) {
        std::string r(s);
        for (char& c : r)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        return r;
    };
    const std::string want = lower(name);
    for (std::size_t i = 0; i < def.params.size(); ++i)
        if (lower(def.params[i].name) == want)
            return static_cast<int>(i);
    return -1;
}

std::vector<double> dynValues(const BlockDef& def, const InsertData& insert) {
    std::vector<double> v;
    if (!def.dynamic)
        return v;
    const auto& params = def.dynamic->params;
    v.reserve(params.size());
    for (std::size_t i = 0; i < params.size(); ++i) {
        double x = i < insert.dynValues.size() ? insert.dynValues[i] : dynDefault(params[i]);
        if (!std::isfinite(x))
            x = dynDefault(params[i]);
        v.push_back(clampToLimits(params[i], x));
    }
    return v;
}

std::vector<Entity> evaluateDynamic(const Document& doc, const BlockDef& def,
                                    const std::vector<double>& values) {
    std::vector<Entity> out;
    std::unordered_set<EntityId> hidden;
    if (def.dynamic) {
        for (std::size_t pi = 0; pi < def.dynamic->params.size(); ++pi) {
            const DynParam& p = def.dynamic->params[pi];
            if (p.kind != DynParam::Kind::Visibility)
                continue;
            const std::size_t shown =
                pi < values.size() ? static_cast<std::size_t>(std::max(values[pi], 0.0)) : 0;
            for (std::size_t s = 0; s < p.stateObjects.size(); ++s)
                if (s != shown)
                    hidden.insert(p.stateObjects[s].begin(), p.stateObjects[s].end());
            if (shown < p.stateObjects.size())
                for (EntityId id : p.stateObjects[shown])
                    hidden.erase(id);
        }
    }
    std::unordered_map<EntityId, std::size_t> slot;
    for (EntityId id : def.entities) {
        if (hidden.count(id))
            continue;
        if (const Entity* e = doc.find(id)) {
            slot[id] = out.size();
            out.push_back(*e);
        }
    }
    if (!def.dynamic)
        return out;
    for (const DynAction& a : def.dynamic->actions) {
        if (a.param < 0 || static_cast<std::size_t>(a.param) >= def.dynamic->params.size())
            continue;
        const DynParam& p = def.dynamic->params[static_cast<std::size_t>(a.param)];
        const double v = static_cast<std::size_t>(a.param) < values.size()
                             ? values[static_cast<std::size_t>(a.param)]
                             : dynDefault(p);
        const Vec2 dir = unitOf(p.base, p.end);
        const double def0 = dynDefault(p);
        for (EntityId id : a.objects) {
            auto it = slot.find(id);
            if (it == slot.end())
                continue;
            Entity& e = out[it->second];
            switch (a.kind) {
                case DynAction::Kind::Stretch:
                    if (p.kind == DynParam::Kind::Linear)
                        stretchEntity(e, a, dir * (v - def0));
                    break;
                case DynAction::Kind::Move:
                    if (p.kind == DynParam::Kind::Linear)
                        applyTransform(e, Transform2::translate(dir * (v - def0)));
                    break;
                case DynAction::Kind::Scale:
                    if (p.kind == DynParam::Kind::Linear && def0 > 0 && v > 0)
                        applyTransform(e, Transform2::scale(v / def0, v / def0, p.base));
                    break;
                case DynAction::Kind::Rotate:
                    if (p.kind == DynParam::Kind::Rotation)
                        applyTransform(e, Transform2::rotate(v, p.base));
                    break;
                case DynAction::Kind::Flip:
                    if (p.kind == DynParam::Kind::Flip && v >= 0.5 && !(p.base == p.end))
                        if (auto m = Transform2::mirror(p.base, p.end))
                            applyTransform(e, m.value());
                    break;
            }
        }
    }
    return out;
}

std::vector<const Entity*> blockItems(const Document& doc, const BlockDef& def,
                                      const InsertData* insert, std::vector<Entity>& storage) {
    std::vector<const Entity*> items;
    if (def.dynamic) {
        InsertData none;
        const std::vector<double> values = dynValues(def, insert ? *insert : none);
        storage = evaluateDynamic(doc, def, values);
        items.reserve(storage.size());
        for (const Entity& e : storage)
            items.push_back(&e);
        return items;
    }
    items.reserve(def.entities.size());
    for (EntityId id : def.entities)
        if (const Entity* e = doc.find(id))
            items.push_back(e);
    return items;
}

std::vector<DynHandle> dynHandles(const Document& doc, const Entity& insert) {
    std::vector<DynHandle> out;
    const auto* ins = entityAs<InsertData>(insert);
    const BlockDef* def = ins ? doc.block(ins->block) : nullptr;
    if (!def || !def->dynamic)
        return out;
    const Transform2 t = instanceTransform(*ins, def->base);
    const std::vector<double> values = dynValues(*def, *ins);
    for (std::size_t i = 0; i < def->dynamic->params.size(); ++i) {
        const DynParam& p = def->dynamic->params[i];
        if (p.kind == DynParam::Kind::Linear) {
            out.push_back(
                {static_cast<int>(i), t.apply(p.base + unitOf(p.base, p.end) * values[i])});
        } else if (p.kind == DynParam::Kind::Rotation) {
            Vec2 arm = p.end - p.base;
            double c = std::cos(values[i]), s = std::sin(values[i]);
            out.push_back({static_cast<int>(i),
                           t.apply(p.base + Vec2{arm.x * c - arm.y * s, arm.x * s + arm.y * c})});
        }
    }
    return out;
}

Result<Entity> dynSet(const Document& doc, const Entity& insert, int param, double value) {
    const auto* ins = entityAs<InsertData>(insert);
    const BlockDef* def = ins ? doc.block(ins->block) : nullptr;
    if (!def || !def->dynamic)
        return makeError(ErrorCode::InvalidArgument, "This block has no dynamic parameters");
    if (param < 0 || static_cast<std::size_t>(param) >= def->dynamic->params.size())
        return makeError(ErrorCode::OutOfRange, "There is no such parameter");
    Entity out = insert;
    auto& d = *entityAs<InsertData>(out);
    d.dynValues = dynValues(*def, *ins);
    d.dynValues[static_cast<std::size_t>(param)] =
        clampToLimits(def->dynamic->params[static_cast<std::size_t>(param)], value);
    return out;
}

Result<Entity> dynDrag(const Document& doc, const Entity& insert, int param, Vec2 world) {
    const auto* ins = entityAs<InsertData>(insert);
    const BlockDef* def = ins ? doc.block(ins->block) : nullptr;
    if (!def || !def->dynamic || param < 0 ||
        static_cast<std::size_t>(param) >= def->dynamic->params.size())
        return makeError(ErrorCode::InvalidArgument, "This block has no such parameter");
    const DynParam& p = def->dynamic->params[static_cast<std::size_t>(param)];
    auto inv = instanceTransform(*ins, def->base).inverse();
    if (!inv)
        return inv.error();
    const Vec2 local = inv.value().apply(world);
    if (p.kind == DynParam::Kind::Linear) {
        const Vec2 d = local - p.base;
        const Vec2 u = unitOf(p.base, p.end);
        return dynSet(doc, insert, param, d.x * u.x + d.y * u.y);
    }
    if (p.kind == DynParam::Kind::Rotation) {
        const Vec2 d = local - p.base;
        const Vec2 arm = p.end - p.base;
        if (std::hypot(d.x, d.y) < 1e-12)
            return insert;
        return dynSet(doc, insert, param, std::atan2(d.y, d.x) - std::atan2(arm.y, arm.x));
    }
    return makeError(ErrorCode::InvalidArgument, "That parameter has no handle to drag");
}

}  // namespace tbcad::blocks
