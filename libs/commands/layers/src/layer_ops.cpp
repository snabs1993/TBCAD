#include "tbcad/layers/layer_ops.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <functional>

namespace tbcad::layers {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view trimView(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

Error err(ErrorCode code, std::string msg) {
    return makeError(code, std::move(msg));
}

bool parseDouble(std::string_view s, double& out) {
    s = trimView(s);
    if (s.empty())
        return false;
    auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size() && std::isfinite(out);
}

bool parseUInt(std::string_view s, unsigned long& out) {
    s = trimView(s);
    if (s.empty())
        return false;
    auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

std::string quoted(const std::string& name) {
    return "\"" + name + "\"";
}

}  // namespace

std::string layerPath(const Document& doc, LayerId id) {
    std::vector<const Layer*> chain;
    for (const Layer* l = doc.layer(id); l; l = l->parent ? doc.layer(l->parent) : nullptr)
        chain.push_back(l);
    std::string out;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (!out.empty())
            out += kPathSeparator;
        out += (*it)->name;
    }
    return out;
}

int layerDepth(const Document& doc, LayerId id) {
    int d = 0;
    const Layer* l = doc.layer(id);
    while (l && l->parent) {
        ++d;
        l = doc.layer(l->parent);
    }
    return d;
}

std::vector<LayerId> childrenOf(const Document& doc, LayerId parent) {
    std::vector<const Layer*> kids;
    for (const auto& [id, l] : doc.layers())
        if (l.parent == parent && id != kLayerZero)
            kids.push_back(&l);
    if (!parent) {
        if (const Layer* zero = doc.layer(kLayerZero))
            kids.push_back(zero);
    }
    std::sort(kids.begin(), kids.end(),
              [](const Layer* a, const Layer* b) { return lower(a->name) < lower(b->name); });
    std::vector<LayerId> out;
    out.reserve(kids.size());
    for (const Layer* l : kids)
        out.push_back(l->id);
    return out;
}

std::vector<LayerId> layerTreeOrder(const Document& doc) {
    std::vector<LayerId> out;
    out.reserve(doc.layers().size());
    auto visit = [&](auto&& self, LayerId parent) -> void {
        for (LayerId id : childrenOf(doc, parent)) {
            out.push_back(id);
            self(self, id);
        }
    };
    visit(visit, LayerId{});
    return out;
}

std::size_t entityCount(const Document& doc, LayerId id) {
    std::size_t n = 0;
    doc.forEachEntity([&](const Entity& e) { n += e.common.layer == id ? 1 : 0; });
    return n;
}

std::vector<LayerId> branchOf(const Document& doc, LayerId id) {
    std::vector<LayerId> out{id};
    for (std::size_t i = 0; i < out.size(); ++i)
        for (LayerId c : childrenOf(doc, out[i]))
            out.push_back(c);
    return out;
}

Result<LayerId> resolveLayer(const Document& doc, std::string_view ref) {
    ref = trimView(ref);
    if (ref.empty())
        return err(ErrorCode::InvalidArgument, "Layer name is empty.");
    if (ref.front() == '#') {
        unsigned long v = 0;
        if (!parseUInt(ref.substr(1), v) || !doc.layer(LayerId{static_cast<std::uint32_t>(v)}))
            return err(ErrorCode::NotFound, "No layer " + std::string(ref) + ".");
        return LayerId{static_cast<std::uint32_t>(v)};
    }
    LayerId parent{};
    std::size_t pos = 0;
    while (true) {
        std::size_t next = ref.find(kPathSeparator, pos);
        std::string_view part = trimView(
            ref.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos));
        const Layer* l = doc.layerByName(parent, part);
        if (!l) {
            // A bare name that is not top level: accept it when exactly one sub-layer has it.
            if (pos == 0 && next == std::string_view::npos) {
                const Layer* found = nullptr;
                int hits = 0;
                for (const auto& [id, cand] : doc.layers())
                    if (cand.parent && lower(cand.name) == lower(part)) {
                        found = &cand;
                        ++hits;
                    }
                if (hits == 1)
                    return found->id;
                if (hits > 1)
                    return err(ErrorCode::Conflict,
                               "Several sub-layers are called " + quoted(std::string(part)) +
                                   "; use the full path (Parent" + kPathSeparator + "Child).");
            }
            return err(ErrorCode::NotFound, "No layer " + quoted(std::string(ref)) + ".");
        }
        parent = l->id;
        if (next == std::string_view::npos)
            return parent;
        pos = next + 1;
    }
}

Result<std::vector<LayerId>> resolveLayers(const Document& doc, std::string_view refs) {
    std::vector<LayerId> out;
    std::size_t pos = 0;
    refs = trimView(refs);
    if (refs.empty())
        return err(ErrorCode::InvalidArgument, "Layer name is empty.");
    while (pos <= refs.size()) {
        std::size_t next = refs.find(',', pos);
        std::string_view part = trimView(
            refs.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos));
        if (part == "*") {
            for (const auto& [id, l] : doc.layers())
                out.push_back(id);
        } else {
            auto r = resolveLayer(doc, part);
            if (!r)
                return r.error();
            out.push_back(r.value());
        }
        if (next == std::string_view::npos)
            break;
        pos = next + 1;
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

Result<LayerId> createLayer(const Document&, Transaction& tx, std::string name, LayerId parent) {
    Layer row;
    row.name = std::move(name);
    row.parent = parent;
    if (parent) {
        row.color = Color::byParent();
        row.linetype = kLinetypeByParent;
        row.lineweight = LineWeight::byParent();
    } else {
        row.linetype = kLinetypeContinuous;
    }
    auto r = tx.addLayer(std::move(row));
    if (!r)
        return err(r.error().code, "Cannot create layer: " + r.error().message);
    return r;
}

Result<void> renameLayer(const Document&, Transaction& tx, LayerId id, std::string newName) {
    auto r = tx.modifyLayer(id, [&](Layer& l) { l.name = std::move(newName); });
    if (!r)
        return err(r.error().code, "Cannot rename layer: " + r.error().message);
    return {};
}

std::string whyCannotReparent(const Document& doc, LayerId id, LayerId newParent) {
    const Layer* l = doc.layer(id);
    if (!l)
        return "No such layer.";
    if (id == kLayerZero)
        return "Layer 0 cannot be moved.";
    if (newParent == kLayerZero)
        return "Layer 0 cannot have sub-layers.";
    if (newParent && !doc.layer(newParent))
        return "No such parent layer.";
    if (l->parent == newParent)
        return "It is already there.";
    for (LayerId p = newParent; p; p = doc.layer(p)->parent)
        if (p == id)
            return "A layer cannot move under itself or one of its own sub-layers (cycle).";
    if (doc.layerByName(newParent, l->name))
        return "A layer called " + quoted(l->name) + " already exists there.";
    // Levels below `id` (0 for a leaf) plus the new position must stay within 8 levels.
    int height = 0;
    for (LayerId b : branchOf(doc, id))
        height = std::max(height, layerDepth(doc, b) - layerDepth(doc, id));
    int newDepth = newParent ? layerDepth(doc, newParent) + 1 : 0;
    if (newDepth + height > 7)
        return "Sub-layers nest at most 8 levels deep.";
    return {};
}

Result<void> reparentLayer(const Document& doc, Transaction& tx, LayerId id, LayerId newParent) {
    const Layer* l = doc.layer(id);
    if (!l)
        return err(ErrorCode::NotFound, "No such layer.");
    // A top-level layer cannot be ByParent, so it takes the values it showed until now.
    EffectiveLayer eff = doc.effective(id);
    auto r = tx.modifyLayer(id, [&](Layer& row) {
        if (!newParent) {
            if (row.color.kind == ColorKind::ByParent)
                row.color = eff.color;
            if (row.linetype == kLinetypeByParent)
                row.linetype = eff.linetype;
            if (row.lineweight.isByParent())
                row.lineweight = eff.lineweight;
        }
        row.parent = newParent;
    });
    if (!r)
        return err(r.error().code, "Cannot move layer: " + r.error().message);
    return {};
}

namespace {

// Why `id` cannot be deleted, or empty when it can (sub-layers are not considered).
std::string whyNotEmpty(const Document& doc, LayerId id) {
    const Layer* l = doc.layer(id);
    if (!l)
        return "No such layer.";
    if (id == kLayerZero)
        return "Layer 0 cannot be deleted.";
    if (doc.header().currentLayer == id)
        return "Layer " + quoted(layerPath(doc, id)) + " is the current layer.";
    if (std::size_t n = entityCount(doc, id))
        return "Layer " + quoted(layerPath(doc, id)) + " still holds " + std::to_string(n) +
               (n == 1 ? " object." : " objects.");
    return {};
}

}  // namespace

Result<void> deleteLayer(const Document& doc, Transaction& tx, LayerId id, DeleteMode mode) {
    if (!doc.layer(id))
        return err(ErrorCode::NotFound, "No such layer.");
    std::vector<LayerId> kids = childrenOf(doc, id);
    if (mode == DeleteMode::LeafOnly && !kids.empty())
        return err(ErrorCode::Conflict, "Layer " + quoted(layerPath(doc, id)) +
                                            " has sub-layers; delete the branch or move them up.");
    std::vector<LayerId> doomed =
        mode == DeleteMode::Branch ? branchOf(doc, id) : std::vector<LayerId>{id};
    // Check everything first so a refusal changes nothing.
    for (LayerId d : doomed)
        if (std::string why = whyNotEmpty(doc, d); !why.empty())
            return err(ErrorCode::Conflict, "Cannot delete: " + why);
    if (mode == DeleteMode::MoveChildrenUp) {
        const Layer* self = doc.layer(id);
        for (LayerId c : kids) {
            const Layer* cl = doc.layer(c);
            if (doc.layerByName(self->parent, cl->name))
                return err(ErrorCode::Conflict, "Cannot move " + quoted(cl->name) +
                                                    " up: a layer with that name already "
                                                    "exists next to " +
                                                    quoted(self->name) + ".");
        }
        // Children that inherit from the deleted layer keep their look by taking its values.
        EffectiveLayer eff = doc.effective(id);
        for (LayerId c : kids) {
            auto r = tx.modifyLayer(c, [&](Layer& l) {
                if (l.color.kind == ColorKind::ByParent)
                    l.color = eff.color;
                if (l.linetype == kLinetypeByParent)
                    l.linetype = eff.linetype;
                if (l.lineweight.isByParent())
                    l.lineweight = eff.lineweight;
                l.parent = self->parent;
            });
            if (!r)
                return err(r.error().code, "Cannot move sub-layer: " + r.error().message);
        }
        auto r = tx.eraseLayer(id);
        if (!r)
            return err(r.error().code, "Cannot delete layer: " + r.error().message);
        return {};
    }
    // Leaves first.
    for (auto it = doomed.rbegin(); it != doomed.rend(); ++it) {
        auto r = tx.eraseLayer(*it);
        if (!r)
            return err(r.error().code, "Cannot delete layer: " + r.error().message);
    }
    return {};
}

Result<void> setActiveOnly(const Document& doc, Transaction& tx, bool on) {
    if (doc.header().activeOnly == on)
        return {};
    return tx.modifyHeader([&](Header& h) { h.activeOnly = on; });
}

Result<void> setCurrentLayer(const Document& doc, Transaction& tx, LayerId id) {
    if (!doc.layer(id))
        return err(ErrorCode::NotFound, "No such layer.");
    EffectiveLayer eff = doc.effective(id);
    if (eff.frozen)
        return err(ErrorCode::Conflict,
                   "Cannot make frozen layer " + quoted(layerPath(doc, id)) + " current.");
    auto r = tx.modifyHeader([&](Header& h) { h.currentLayer = id; });
    if (!r)
        return err(r.error().code, r.error().message);
    return {};
}

namespace {

Result<void> editLayer(Transaction& tx, LayerId id, const std::function<void(Layer&)>& fn) {
    auto r = tx.modifyLayer(id, fn);
    if (!r)
        return err(r.error().code, r.error().message);
    return {};
}

}  // namespace

Result<void> setOn(const Document&, Transaction& tx, LayerId id, bool on) {
    return editLayer(tx, id, [&](Layer& l) { l.on = on; });
}

Result<void> setFrozen(const Document& doc, Transaction& tx, LayerId id, bool frozen) {
    if (frozen) {
        // Freezing a layer freezes its branch, so the current layer must not be inside it.
        for (LayerId b : branchOf(doc, id))
            if (b == doc.header().currentLayer)
                return err(ErrorCode::Conflict, "Cannot freeze " + quoted(layerPath(doc, id)) +
                                                    ": the current layer is " +
                                                    (b == id ? "this layer." : "inside it."));
    }
    return editLayer(tx, id, [&](Layer& l) { l.frozen = frozen; });
}

Result<void> setLocked(const Document&, Transaction& tx, LayerId id, bool locked) {
    return editLayer(tx, id, [&](Layer& l) { l.locked = locked; });
}

Result<void> setPlot(const Document&, Transaction& tx, LayerId id, bool plot) {
    return editLayer(tx, id, [&](Layer& l) { l.plot = plot; });
}

Result<void> setLayerColor(const Document&, Transaction& tx, LayerId id, Color color) {
    return editLayer(tx, id, [&](Layer& l) { l.color = color; });
}

Result<void> setLayerLinetype(const Document&, Transaction& tx, LayerId id, LinetypeId linetype) {
    return editLayer(tx, id, [&](Layer& l) { l.linetype = linetype; });
}

Result<void> setLayerLineweight(const Document&, Transaction& tx, LayerId id, LineWeight weight) {
    return editLayer(tx, id, [&](Layer& l) { l.lineweight = weight; });
}

Result<void> isolateLayer(const Document& doc, Transaction& tx, LayerId keep) {
    if (!doc.layer(keep))
        return err(ErrorCode::NotFound, "No such layer.");
    std::vector<LayerId> keepers;
    for (const Layer* l = doc.layer(keep); l; l = l->parent ? doc.layer(l->parent) : nullptr)
        keepers.push_back(l->id);
    for (LayerId b : branchOf(doc, keep))
        keepers.push_back(b);
    // Collect first: editing replaces rows in the layer table, so it cannot be iterated meanwhile.
    std::vector<LayerId> turnOff, turnOn;
    for (const auto& [id, l] : doc.layers()) {
        bool k = std::find(keepers.begin(), keepers.end(), id) != keepers.end();
        if (!k && l.on)
            turnOff.push_back(id);
    }
    // The kept layer and its ancestors must be visible for the isolation to mean anything.
    for (LayerId k : keepers)
        if (!doc.layer(k)->on)
            turnOn.push_back(k);
    for (LayerId id : turnOff)
        if (auto r = editLayer(tx, id, [](Layer& x) { x.on = false; }); !r)
            return r;
    for (LayerId id : turnOn)
        if (auto r = editLayer(tx, id, [](Layer& x) { x.on = true; }); !r)
            return r;
    return {};
}

Result<void> showAllLayers(const Document& doc, Transaction& tx) {
    std::vector<LayerId> off;
    for (const auto& [id, l] : doc.layers())
        if (!l.on)
            off.push_back(id);
    for (LayerId id : off) {
        auto r = editLayer(tx, id, [](Layer& x) { x.on = true; });
        if (!r)
            return r;
    }
    return {};
}

// ---- parsing helpers ---------------------------------------------------------------------------

Result<Color> parseColor(std::string_view text) {
    text = trimView(text);
    std::string t = lower(text);
    if (t.empty())
        return err(ErrorCode::Parse, "Colour is empty.");
    if (t == "bylayer")
        return Color::byLayer();
    if (t == "byblock")
        return Color::byBlock();
    if (t == "byparent")
        return Color::byParent();
    static const char* const names[] = {"red",  "yellow",  "green", "cyan",
                                        "blue", "magenta", "white"};
    for (int i = 0; i < 7; ++i)
        if (t == names[i])
            return Color::index(static_cast<std::uint8_t>(i + 1));
    if (t.find(',') != std::string::npos) {
        unsigned long v[3] = {};
        std::size_t pos = 0;
        for (int i = 0; i < 3; ++i) {
            std::size_t next = t.find(',', pos);
            if ((i < 2) != (next != std::string::npos))
                return err(ErrorCode::Parse, "Colour must be red,green,blue (0-255 each).");
            std::string_view part(t.data() + pos,
                                  (next == std::string::npos ? t.size() : next) - pos);
            if (!parseUInt(part, v[i]) || v[i] > 255)
                return err(ErrorCode::Parse, "Colour must be red,green,blue (0-255 each).");
            pos = next + 1;
        }
        return Color::rgb(static_cast<std::uint8_t>(v[0]), static_cast<std::uint8_t>(v[1]),
                          static_cast<std::uint8_t>(v[2]));
    }
    unsigned long idx = 0;
    if (parseUInt(t, idx) && idx >= 1 && idx <= 255)
        return Color::index(static_cast<std::uint8_t>(idx));
    return err(ErrorCode::Parse, "Colour must be a name, 1-255 or red,green,blue.");
}

std::string colorName(Color c) {
    switch (c.kind) {
        case ColorKind::ByLayer:
            return "ByLayer";
        case ColorKind::ByBlock:
            return "ByBlock";
        case ColorKind::ByParent:
            return "ByParent";
        case ColorKind::Index: {
            static const char* const names[] = {"red",  "yellow",  "green", "cyan",
                                                "blue", "magenta", "white"};
            if (c.indexValue() >= 1 && c.indexValue() <= 7)
                return names[c.indexValue() - 1];
            return std::to_string(c.indexValue());
        }
        case ColorKind::Rgb:
            return std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b);
    }
    return {};
}

Result<LineWeight> parseLineweight(std::string_view text) {
    std::string t = lower(trimView(text));
    if (t == "default")
        return LineWeight::defaultWeight();
    if (t == "bylayer")
        return LineWeight::byLayer();
    if (t == "byblock")
        return LineWeight::byBlock();
    if (t == "byparent")
        return LineWeight::byParent();
    if (t.size() > 2 && t.compare(t.size() - 2, 2, "mm") == 0)
        t.resize(t.size() - 2);
    double mm = 0;
    if (!parseDouble(t, mm) || mm < 0 || mm > 2.11)
        return err(ErrorCode::Parse, "Lineweight must be 0 to 2.11 mm, Default or ByParent.");
    return LineWeight::mm(mm);
}

std::string lineweightName(LineWeight w) {
    if (w.isByLayer())
        return "ByLayer";
    if (w.isByBlock())
        return "ByBlock";
    if (w.isByParent())
        return "ByParent";
    if (w.isDefault())
        return "Default";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f mm", w.toMm());
    return buf;
}

Result<LinetypeId> parseLinetype(const Document& doc, std::string_view text) {
    text = trimView(text);
    std::string t = lower(text);
    if (t == "bylayer")
        return kLinetypeByLayer;
    if (t == "byblock")
        return kLinetypeByBlock;
    if (t == "byparent")
        return kLinetypeByParent;
    if (!t.empty() && t.front() == '#') {
        unsigned long v = 0;
        if (parseUInt(t.substr(1), v) && doc.linetype(LinetypeId{static_cast<std::uint32_t>(v)}))
            return LinetypeId{static_cast<std::uint32_t>(v)};
        return err(ErrorCode::NotFound, "No linetype " + std::string(text) + ".");
    }
    if (const Linetype* lt = doc.linetypeByName(text))
        return lt->id;
    return err(ErrorCode::NotFound,
               "Linetype " + quoted(std::string(text)) + " is not loaded (see -LINETYPE Load).");
}

std::string linetypeName(const Document& doc, LinetypeId id) {
    if (id == kLinetypeByLayer)
        return "ByLayer";
    if (id == kLinetypeByBlock)
        return "ByBlock";
    if (id == kLinetypeByParent)
        return "ByParent";
    const Linetype* lt = doc.linetype(id);
    return lt ? lt->name : "?";
}

}  // namespace tbcad::layers
