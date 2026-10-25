// CHECKSTANDARDS: compare the named rows of a drawing with a standards drawing, and fix them.
#include "tbcad/layers/standards.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

#include "tbcad/layers/layer_ops.h"

namespace tbcad::layers {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool same(double a, double b) {
    return std::abs(a - b) < 1e-6;
}

bool builtinLinetype(const std::string& lowerName) {
    return lowerName == "continuous" || lowerName == "bylayer" || lowerName == "byblock";
}

std::string linetypeNameOf(const Document& doc, LinetypeId id) {
    const Linetype* lt = doc.linetype(id);
    return lt ? lt->name : "Continuous";
}

bool samePattern(const Linetype& a, const Linetype& b) {
    if (a.pattern.size() != b.pattern.size() || a.marks.size() != b.marks.size())
        return false;
    for (std::size_t i = 0; i < a.pattern.size(); ++i)
        if (!same(a.pattern[i], b.pattern[i]))
            return false;
    return a.marks == b.marks;
}

bool sameTextStyle(const TextStyle& a, const TextStyle& b) {
    return lower(a.fontFamily) == lower(b.fontFamily) && lower(a.fontFile) == lower(b.fontFile) &&
           lower(a.shxFile) == lower(b.shxFile) && same(a.widthFactor, b.widthFactor) &&
           same(a.oblique, b.oblique) && same(a.height, b.height);
}

// The dimension style settings an office standard fixes. Colours and line weights too.
std::string dimDifference(const DimStyle& a, const DimStyle& b) {
    std::string out;
    auto add = [&](const char* what) { out += (out.empty() ? "" : ", ") + std::string(what); };
    if (!same(a.textHeight, b.textHeight))
        add("text height");
    if (!same(a.textGap, b.textGap))
        add("text gap");
    if (a.arrowType != b.arrowType)
        add("arrow");
    if (!same(a.arrowSize, b.arrowSize))
        add("arrow size");
    if (!same(a.extLineOffset, b.extLineOffset) || !same(a.extLineExtension, b.extLineExtension))
        add("extension lines");
    if (a.units != b.units || a.precision != b.precision)
        add("units");
    if (!same(a.overallScale, b.overallScale))
        add("scale");
    if (a.textPlacement != b.textPlacement || a.textAlign != b.textAlign)
        add("text placement");
    if (a.dimLineColor != b.dimLineColor || a.extLineColor != b.extLineColor ||
        a.textColor != b.textColor)
        add("colours");
    return out;
}

bool isXrefLayer(const std::string& path) {
    return path.find(kPathSeparator) != std::string::npos;
}

char letterOf(StandardsKind k) {
    switch (k) {
        case StandardsKind::Layer:
            return 'L';
        case StandardsKind::Linetype:
            return 'Y';
        case StandardsKind::TextStyle:
            return 'T';
        default:
            return 'D';
    }
}

}  // namespace

std::string StandardsViolation::key() const {
    return std::string(1, letterOf(kind)) + ":" + name;
}

std::string fixLabel(const StandardsViolation& v) {
    switch (v.fix) {
        case StandardsFix::Rename:
            return "Rename to " + v.target;
        case StandardsFix::Replace:
            return "Replace with " + v.target;
        default:
            return "Set to the standard";
    }
}

std::vector<StandardsViolation> checkStandards(const Document& drawing, const Document& standards) {
    std::vector<StandardsViolation> out;

    // ---- layers ----
    std::map<std::string, LayerId> stdLayers;  // lower-case path
    for (const auto& [id, l] : standards.layers())
        stdLayers[lower(layerPath(standards, id))] = id;
    for (LayerId id : layerTreeOrder(drawing)) {
        const Layer* l = drawing.layer(id);
        if (!l || id == kLayerZero)
            continue;
        const std::string path = layerPath(drawing, id);
        if (isXrefLayer(path))
            continue;
        StandardsViolation v;
        v.kind = StandardsKind::Layer;
        v.name = path;
        auto it = stdLayers.find(lower(path));
        if (it == stdLayers.end()) {
            v.fix = StandardsFix::Replace;
            v.target = "0";
            // A standard layer that looks the same takes the objects; layer 0 otherwise.
            for (const auto& [sid, sl] : standards.layers()) {
                if (sid == kLayerZero || sl.parent || sl.color != l->color ||
                    sl.lineweight != l->lineweight ||
                    lower(linetypeNameOf(standards, sl.linetype)) !=
                        lower(linetypeNameOf(drawing, l->linetype)))
                    continue;
                v.target = sl.name;
                break;
            }
            v.problem = "Layer is not in the standard";
            out.push_back(std::move(v));
            continue;
        }
        const Layer& sl = *standards.layer(it->second);
        std::string what;
        auto add = [&](const char* w) { what += (what.empty() ? "" : ", ") + std::string(w); };
        if (path != layerPath(standards, it->second)) {
            v.fix = StandardsFix::Rename;
            v.target = sl.name;
            add("name");
        }
        if (sl.color != l->color)
            add("colour");
        if (sl.lineweight != l->lineweight)
            add("lineweight");
        if (lower(linetypeNameOf(standards, sl.linetype)) !=
            lower(linetypeNameOf(drawing, l->linetype)))
            add("linetype");
        if (what.empty())
            continue;
        v.problem = "Layer differs from the standard: " + what;
        out.push_back(std::move(v));
    }

    // ---- linetypes ----
    for (const auto& [id, lt] : drawing.linetypes()) {
        const std::string key = lower(lt.name);
        if (builtinLinetype(key))
            continue;
        StandardsViolation v;
        v.kind = StandardsKind::Linetype;
        v.name = lt.name;
        const Linetype* sl = standards.linetypeByName(lt.name);
        if (!sl) {
            v.fix = StandardsFix::Replace;
            v.target = "Continuous";
            v.problem = "Linetype is not in the standard";
            out.push_back(std::move(v));
        } else if (!samePattern(lt, *sl)) {
            v.problem = "Linetype pattern differs from the standard";
            out.push_back(std::move(v));
        }
    }

    // ---- text styles ----
    for (const auto& [id, ts] : drawing.textStyles()) {
        if (lower(ts.name) == "standard")
            continue;
        StandardsViolation v;
        v.kind = StandardsKind::TextStyle;
        v.name = ts.name;
        const TextStyle* ss = standards.textStyleByName(ts.name);
        if (!ss) {
            v.fix = StandardsFix::Replace;
            v.target = "Standard";
            v.problem = "Text style is not in the standard";
            out.push_back(std::move(v));
        } else if (!sameTextStyle(ts, *ss)) {
            v.problem = "Text style differs from the standard (font, width, slant or height)";
            out.push_back(std::move(v));
        }
    }

    // ---- dimension styles ----
    for (const auto& [id, ds] : drawing.dimStyles()) {
        if (lower(ds.name) == "standard")
            continue;
        StandardsViolation v;
        v.kind = StandardsKind::DimStyle;
        v.name = ds.name;
        const DimStyle* ss = standards.dimStyleByName(ds.name);
        if (!ss) {
            v.fix = StandardsFix::Replace;
            v.target = "Standard";
            v.problem = "Dimension style is not in the standard";
            out.push_back(std::move(v));
        } else if (std::string diff = dimDifference(ds, *ss); !diff.empty()) {
            v.problem = "Dimension style differs from the standard: " + diff;
            out.push_back(std::move(v));
        }
    }
    return out;
}

namespace {

LinetypeId ensureLinetype(const Document& doc, const Document& standards, Transaction& tx,
                          const std::string& name) {
    if (const Linetype* have = doc.linetypeByName(name))
        return have->id;
    if (const Linetype* from = standards.linetypeByName(name)) {
        Linetype row = *from;
        row.id = {};
        if (auto r = tx.addLinetype(std::move(row)); r)
            return r.value();
    }
    return {};
}

}  // namespace

StandardsResult fixStandards(const Document& doc, const Document& standards, Transaction& tx,
                             const std::vector<StandardsViolation>& violations) {
    StandardsResult res;
    auto fail = [&](const StandardsViolation& v, const std::string& why) {
        res.failed.push_back(v.name + ": " + why);
    };
    for (const StandardsViolation& v : violations) {
        switch (v.kind) {
            case StandardsKind::Layer: {
                auto from = resolveLayer(doc, v.name);
                if (!from) {
                    fail(v, "no such layer");
                    break;
                }
                if (v.fix == StandardsFix::Replace) {
                    auto into = resolveLayer(doc, v.target);
                    if (!into) {
                        const Layer* sl = standards.layerByName(v.target);
                        if (!sl) {
                            fail(v, "no layer " + v.target + " to replace it with");
                            break;
                        }
                        Layer row = *sl;
                        row.id = {};
                        row.parent = {};
                        row.linetype = ensureLinetype(doc, standards, tx,
                                                      linetypeNameOf(standards, sl->linetype));
                        auto added = tx.addLayer(std::move(row));
                        if (!added) {
                            fail(v, added.error().message);
                            break;
                        }
                        into = added.value();
                    }
                    if (auto r = mergeLayer(doc, tx, from.value(), into.value()); !r) {
                        fail(v, r.error().message);
                        break;
                    }
                    ++res.fixed;
                    break;
                }
                const Layer* sl = nullptr;
                for (const auto& [sid, l] : standards.layers())
                    if (lower(layerPath(standards, sid)) == lower(v.name)) {
                        sl = &l;
                        break;
                    }
                if (!sl) {
                    fail(v, "layer is not in the standard");
                    break;
                }
                const LinetypeId lt =
                    ensureLinetype(doc, standards, tx, linetypeNameOf(standards, sl->linetype));
                auto r = tx.modifyLayer(from.value(), [&](Layer& l) {
                    if (v.fix == StandardsFix::Rename)
                        l.name = sl->name;
                    l.color = sl->color;
                    l.lineweight = sl->lineweight;
                    if (lt)
                        l.linetype = lt;
                });
                r ? void(++res.fixed) : fail(v, r.error().message);
                break;
            }
            case StandardsKind::Linetype: {
                const Linetype* have = doc.linetypeByName(v.name);
                if (!have) {
                    fail(v, "no such linetype");
                    break;
                }
                const LinetypeId id = have->id;
                if (v.fix == StandardsFix::Update) {
                    const Linetype* sl = standards.linetypeByName(v.name);
                    if (!sl) {
                        fail(v, "linetype is not in the standard");
                        break;
                    }
                    auto r = tx.modifyLinetype(id, [&](Linetype& l) {
                        l.pattern = sl->pattern;
                        l.marks = sl->marks;
                        l.description = sl->description;
                    });
                    r ? void(++res.fixed) : fail(v, r.error().message);
                    break;
                }
                const Linetype* to = doc.linetypeByName(v.target);
                const LinetypeId into = to ? to->id : LinetypeId{};
                std::vector<LayerId> onLayers;
                for (const auto& [lid, l] : doc.layers())
                    if (l.linetype == id)
                        onLayers.push_back(lid);
                std::vector<EntityId> onObjects;
                doc.forEachEntity([&](const Entity& e) {
                    if (e.common.linetype == id)
                        onObjects.push_back(e.common.id);
                });
                for (LayerId lid : onLayers)
                    (void)tx.modifyLayer(lid, [&](Layer& l) { l.linetype = into; });
                for (EntityId eid : onObjects)
                    (void)tx.modify(eid, [&](Entity& e) {
                        e.common.linetype = into.value ? into : kLinetypeByLayer;
                    });
                if (doc.header().currentLinetype == id)
                    (void)tx.modifyHeader([&](Header& h) { h.currentLinetype = kLinetypeByLayer; });
                auto r = tx.eraseLinetype(id);
                r ? void(++res.fixed) : fail(v, r.error().message);
                break;
            }
            case StandardsKind::TextStyle: {
                const TextStyle* have = doc.textStyleByName(v.name);
                if (!have) {
                    fail(v, "no such text style");
                    break;
                }
                const TextStyleId id = have->id;
                if (v.fix == StandardsFix::Update) {
                    const TextStyle* ss = standards.textStyleByName(v.name);
                    if (!ss) {
                        fail(v, "text style is not in the standard");
                        break;
                    }
                    auto r = tx.modifyTextStyle(id, [&](TextStyle& t) {
                        const TextStyleId keep = t.id;
                        const std::string name = t.name;
                        t = *ss;
                        t.id = keep;
                        t.name = name;
                    });
                    r ? void(++res.fixed) : fail(v, r.error().message);
                    break;
                }
                const TextStyle* to = doc.textStyleByName(v.target);
                const TextStyleId into = to ? to->id : kTextStyleStandard;
                std::vector<EntityId> onObjects;
                doc.forEachEntity([&](const Entity& e) {
                    if (const auto* t = entityAs<TextData>(e); t && t->style == id)
                        onObjects.push_back(e.common.id);
                    else if (const auto* m = entityAs<MTextData>(e); m && m->style == id)
                        onObjects.push_back(e.common.id);
                });
                for (EntityId eid : onObjects)
                    (void)tx.modify(eid, [&](Entity& e) {
                        if (auto* t = entityAs<TextData>(e))
                            t->style = into;
                        else if (auto* m = entityAs<MTextData>(e))
                            m->style = into;
                    });
                std::vector<DimStyleId> dims;
                for (const auto& [did, d] : doc.dimStyles())
                    if (d.textStyle == id)
                        dims.push_back(did);
                for (DimStyleId did : dims)
                    (void)tx.modifyDimStyle(did, [&](DimStyle& d) { d.textStyle = into; });
                if (doc.header().currentTextStyle == id)
                    (void)tx.modifyHeader([&](Header& h) { h.currentTextStyle = into; });
                auto r = tx.eraseTextStyle(id);
                r ? void(++res.fixed) : fail(v, r.error().message);
                break;
            }
            case StandardsKind::DimStyle: {
                const DimStyle* have = doc.dimStyleByName(v.name);
                if (!have) {
                    fail(v, "no such dimension style");
                    break;
                }
                const DimStyleId id = have->id;
                if (v.fix == StandardsFix::Update) {
                    const DimStyle* ss = standards.dimStyleByName(v.name);
                    if (!ss) {
                        fail(v, "dimension style is not in the standard");
                        break;
                    }
                    // The standard's text style is another table: found again by name.
                    TextStyleId text = have->textStyle;
                    if (const TextStyle* st = standards.textStyle(ss->textStyle))
                        if (const TextStyle* mine = doc.textStyleByName(st->name))
                            text = mine->id;
                    auto r = tx.modifyDimStyle(id, [&](DimStyle& d) {
                        const DimStyleId keep = d.id;
                        const std::string name = d.name;
                        d = *ss;
                        d.id = keep;
                        d.name = name;
                        d.textStyle = text;
                    });
                    r ? void(++res.fixed) : fail(v, r.error().message);
                    break;
                }
                const DimStyle* to = doc.dimStyleByName(v.target);
                const DimStyleId into = to ? to->id : kDimStyleStandard;
                std::vector<EntityId> onObjects;
                doc.forEachEntity([&](const Entity& e) {
                    if (const auto* d = entityAs<DimensionData>(e); d && d->style == id)
                        onObjects.push_back(e.common.id);
                    else if (const auto* l = entityAs<LeaderData>(e); l && l->style == id)
                        onObjects.push_back(e.common.id);
                    else if (const auto* m = entityAs<MLeaderData>(e); m && m->style == id)
                        onObjects.push_back(e.common.id);
                });
                for (EntityId eid : onObjects)
                    (void)tx.modify(eid, [&](Entity& e) {
                        if (auto* d = entityAs<DimensionData>(e))
                            d->style = into;
                        else if (auto* l = entityAs<LeaderData>(e))
                            l->style = into;
                        else if (auto* m = entityAs<MLeaderData>(e))
                            m->style = into;
                    });
                if (doc.header().currentDimStyle == id)
                    (void)tx.modifyHeader([&](Header& h) { h.currentDimStyle = into; });
                auto r = tx.eraseDimStyle(id);
                r ? void(++res.fixed) : fail(v, r.error().message);
                break;
            }
        }
    }
    return res;
}

}  // namespace tbcad::layers
