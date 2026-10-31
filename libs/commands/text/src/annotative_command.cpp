#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <unordered_set>

#include "commands.h"
#include "tbcad/cmd/cmd.h"

namespace tbcad::textcmd {

namespace {

using cmd::CommandContext;
using cmd::Task;

bool canBeAnnotative(const Entity& e) {
    // A block insert has no live scaling: its own scale is re-set when its annotation scale
    // changes, so symbols (north arrows, markers) can be drawn at 1:1 and sized for 1:N.
    return canScaleAnnotatively(kind(e)) || kind(e) == EntityKind::Insert;
}

// "50,100" -> {50,100}; empty, "current" or "-" -> {} (follows the drawing's current scale).
std::optional<std::vector<double>> parseScales(const std::string& text) {
    std::vector<double> out;
    std::size_t at = 0;
    bool any = false;
    while (at <= text.size()) {
        std::size_t sep = text.find_first_of(", ;", at);
        std::string part = text.substr(at, sep == std::string::npos ? sep : sep - at);
        if (part.rfind("1:", 0) == 0)
            part = part.substr(2);
        if (!part.empty()) {
            any = true;
            char* end = nullptr;
            double v = std::strtod(part.c_str(), &end);
            if (end == part.c_str() || *end != 0 || !(v > 0) || !std::isfinite(v))
                return std::nullopt;
            if (std::find(out.begin(), out.end(), v) == out.end())
                out.push_back(v);
        }
        if (sep == std::string::npos)
            break;
        at = sep + 1;
    }
    (void)any;
    return out;
}

// What an object is drawn at in model space.
double primaryScale(const Header& h, const EntityCommon& c) {
    return !c.annoScales.empty() ? c.annoScales.front() : currentAnnotationScale(h);
}

std::string scaleText(double n) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "1:%g", n);
    return buf;
}

// Annotative objects are drawn again at the new scale: a no-op edit marks them as changed. With
// ANNOAUTOSCALE on, an object that lists scales of its own also gets the new scale (symbols are
// sized for their first scale and keep their list). Returns how many objects got the scale.
std::size_t retouchAnnotative(CommandContext& ctx, double scale) {
    const bool autoScale = annoAutoScale(ctx.doc().header());
    std::vector<EntityId> ids, add;
    ctx.doc().forEachEntity([&](const Entity& e) {
        if (!e.common.annotative)
            return;
        ids.push_back(e.common.id);
        if (autoScale && kind(e) != EntityKind::Insert && !e.common.annoScales.empty() &&
            std::find(e.common.annoScales.begin(), e.common.annoScales.end(), scale) ==
                e.common.annoScales.end())
            add.push_back(e.common.id);
    });
    const std::unordered_set<EntityId> adding(add.begin(), add.end());
    for (EntityId id : ids) {
        if (adding.count(id)) {
            (void)ctx.tx().modify(id, [&](Entity& e) {
                std::vector<double> next = e.common.annoScales.get();
                next.push_back(scale);
                e.common.annoScales = ScaleList(std::move(next));
            });
        } else {
            (void)ctx.tx().modify(id, [](Entity&) {});
        }
    }
    return add.size();
}

// ANNOTATIVE [ANNO]: turns annotative scaling on or off for the selected text, dimensions,
// leaders, tables and symbols, or sets the scales they are made for.
class AnnotativeCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ANNOTATIVE"; }
    std::vector<std::string> defaultAliases() const override { return {"ANNO"}; }
    std::string_view description() const override {
        return "Make objects follow an annotation scale, or choose their scales";
    }

    Task run(CommandContext& ctx) override {
        auto sel = co_await ctx.getSelection(
            "Select text, dimensions, leaders, tables or symbols",
            cmd::SelectionOptions{.filter = [](const Entity& e) { return canBeAnnotative(e); }});
        if (!sel.ok() || sel.ids.empty())
            co_return;
        cmd::KeywordOptions ko;
        ko.keywords = {"On", "Off", "Scales"};
        ko.defaultKeyword = "On";
        auto k = co_await ctx.getKeyword("Annotative [On/Off/Scales] <On>", ko);
        if (!k.ok())
            co_return;
        std::optional<std::vector<double>> scales;  // set by "Scales"
        if (k.keyword == "Scales") {
            cmd::StringOptions so;
            so.allowEmpty = true;
            auto t =
                co_await ctx.getString("Scales as N of 1:N, e.g. 50,100 (empty = current)", so);
            if (!t.ok())
                co_return;
            scales = parseScales(t.text);
            if (!scales) {
                ctx.message("Scales must be numbers above zero, like 50,100.");
                co_return;
            }
        }
        const bool on = k.keyword != "Off";
        const Header header = ctx.doc().header();
        std::size_t n = 0;
        for (EntityId id : sel.ids) {
            const Entity* e = ctx.doc().find(id);
            if (!e || !canBeAnnotative(*e))
                continue;
            const bool isInsert = kind(*e) == EntityKind::Insert;
            const double oldN = e->common.annotative ? primaryScale(header, e->common) : 1.0;
            std::vector<double> next = scales ? *scales : e->common.annoScales.get();
            if (on && isInsert && next.empty())
                next = {currentAnnotationScale(header)};
            const double newN =
                !on ? 1.0 : (next.empty() ? currentAnnotationScale(header) : next.front());
            if (e->common.annotative == on && ScaleList(next) == e->common.annoScales)
                continue;
            auto r = ctx.tx().modify(id, [&](Entity& x) {
                x.common.annotative = on;
                x.common.annoScales = ScaleList(on ? next : std::vector<double>{});
                if (auto* ins = entityAs<InsertData>(x)) {
                    ins->scale = ins->scale * (newN / oldN);
                }
            });
            if (r)
                ++n;
        }
        ctx.message(std::to_string(n) + (on ? " set" : " no longer annotative"));
        // Annotative text is drawn at its height times the scale: a height already sized for the
        // drawing (say 250 mm at 1:100) would come out far too big.
        if (on) {
            const double scale = currentAnnotationScale(header);
            std::size_t big = 0;
            for (EntityId id : sel.ids) {
                const Entity* e = ctx.doc().find(id);
                if (!e)
                    continue;
                double h = 0;
                if (const auto* t = entityAs<TextData>(*e))
                    h = t->height;
                else if (const auto* m = entityAs<MTextData>(*e))
                    h = m->height;
                if (h * scale > 500)
                    ++big;
            }
            if (big > 0)
                ctx.message("Warning: " + std::to_string(big) +
                            " text object(s) will be drawn very large (height x the annotation "
                            "scale). Annotative text should use its paper height, such as 2.5.");
        }
        ctx.setUndoLabel("Annotative");
    }
};

// "50", "1:50" and "1/50" all mean one drawing unit to 50; anything else is rejected.
std::optional<double> parseScaleN(const std::string& text) {
    std::string t;
    for (char c : text)
        if (c != ' ')
            t += c;
    for (const char* prefix : {"1:", "1/"})
        if (t.rfind(prefix, 0) == 0) {
            t.erase(0, 2);
            break;
        }
    if (t.empty())
        return std::nullopt;
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end != t.c_str() + t.size() || !std::isfinite(v))
        return std::nullopt;
    return v;
}

// ANNONEW: new text, dimensions, leaders, tables and symbols are annotative.
class AnnoNewCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ANNONEW"; }
    std::string_view description() const override {
        return "New annotation objects follow the annotation scale";
    }
    Task run(CommandContext& ctx) override {
        const bool now = annotateNewObjects(ctx.doc().header());
        cmd::KeywordOptions ko;
        ko.keywords = {"On", "Off"};
        ko.defaultKeyword = now ? "Off" : "On";
        auto k = co_await ctx.getKeyword(
            std::string("New objects annotative [On/Off] <") + (now ? "Off" : "On") + ">", ko);
        if (!k.ok())
            co_return;
        const bool on = k.keyword == "On";
        (void)ctx.tx().modifyHeader([&](Header& h) { h.custom[kAnnoNewKey] = on ? "1" : "0"; });
        ctx.message(on ? "New objects are annotative at the current scale."
                       : "New objects are ordinary.");
        ctx.setUndoLabel("Annotative new objects");
    }
};

// CANNOSCALE [AS]: the current annotation scale. Annotative objects are drawn at this scale.
class AnnoScaleCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "CANNOSCALE"; }
    std::vector<std::string> defaultAliases() const override { return {"AS"}; }
    std::string_view description() const override { return "Set the annotation scale"; }

    Task run(CommandContext& ctx) override {
        const double cur = currentAnnotationScale(ctx.doc().header());
        const std::string def = scaleText(cur).substr(2);
        cmd::StringOptions so;
        so.defaultValue = def;
        auto r = co_await ctx.getString("Annotation scale, N or 1:N <" + def + ">", so);
        if (!r.ok())
            co_return;
        const auto parsed = parseScaleN(r.text.empty() ? def : r.text);
        if (!parsed || !(*parsed > 0)) {
            ctx.message("Enter the scale as N or 1:N, a number above zero.");
            co_return;
        }
        const double v = *parsed;
        (void)ctx.tx().modifyHeader([&](Header& h) {
            char buf[40];
            std::snprintf(buf, sizeof buf, "%.12g", v);
            h.custom[kAnnoScaleKey] = buf;
        });
        const std::size_t added = retouchAnnotative(ctx, v);
        ctx.message("Annotation scale: " + scaleText(v) +
                    (added ? " (added to " + std::to_string(added) + " annotative object(s))"
                           : std::string()));
        ctx.setUndoLabel("Annotation scale");
    }
};

// SCALELISTEDIT: the scales offered for annotation.
class ScaleListCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "SCALELISTEDIT"; }
    std::vector<std::string> defaultAliases() const override { return {"SCALELIST"}; }
    std::string_view description() const override { return "Edit the list of annotation scales"; }

    Task run(CommandContext& ctx) override {
        for (;;) {
            cmd::KeywordOptions ko;
            ko.keywords = {"Add", "Delete", "Reset", "List", "Done"};
            ko.defaultKeyword = "Done";
            auto k = co_await ctx.getKeyword("Scales [Add/Delete/Reset/List/Done] <Done>", ko);
            if (!k.ok() || k.keyword == "Done")
                co_return;
            std::vector<double> list = annotationScaleList(ctx.doc().header());
            if (k.keyword == "List") {
                std::string s;
                for (double v : list)
                    s += (s.empty() ? "" : "  ") + scaleText(v);
                ctx.message(s);
                continue;
            }
            if (k.keyword == "Reset") {
                (void)ctx.tx().modifyHeader([](Header& h) { h.custom.erase(kAnnoScaleListKey); });
                continue;
            }
            auto r = co_await ctx.getString("Scale, N or 1:N");
            if (!r.ok())
                co_return;
            const auto parsed = parseScaleN(r.text);
            if (!parsed || !(*parsed > 0)) {
                ctx.message("Enter the scale as N or 1:N, a number above zero.");
                continue;
            }
            const double v = *parsed;
            auto it = std::find(list.begin(), list.end(), v);
            if (k.keyword == "Add") {
                if (it == list.end())
                    list.push_back(v);
                std::sort(list.begin(), list.end());
            } else if (it != list.end()) {
                list.erase(it);
            } else {
                ctx.message("That scale is not in the list.");
                continue;
            }
            if (list.empty()) {
                ctx.message("The list needs at least one scale.");
                continue;
            }
            std::string joined;
            for (double x : list) {
                char buf[40];
                std::snprintf(buf, sizeof buf, "%.12g", x);
                joined += (joined.empty() ? "" : ",") + std::string(buf);
            }
            (void)ctx.tx().modifyHeader([&](Header& h) { h.custom[kAnnoScaleListKey] = joined; });
        }
    }
};

// OBJECTSCALE: adds a scale to, or takes one from, the scales of annotative objects. The first
// scale is the one the object is drawn at in model space; a block insert follows it.
class ObjectScaleCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "OBJECTSCALE"; }
    std::string_view description() const override {
        return "Add or remove annotation scales of annotative objects";
    }

    Task run(CommandContext& ctx) override {
        auto sel = co_await ctx.getSelection("Select annotative objects",
                                             cmd::SelectionOptions{.filter = [](const Entity& e) {
                                                 return canBeAnnotative(e) && e.common.annotative;
                                             }});
        if (!sel.ok() || sel.ids.empty())
            co_return;
        cmd::KeywordOptions ko;
        ko.keywords = {"Add", "Delete", "All"};
        ko.defaultKeyword = "Add";
        auto k = co_await ctx.getKeyword("Object scale [Add/Delete/All] <Add>", ko);
        if (!k.ok())
            co_return;
        std::vector<double> chosen;
        if (k.keyword == "All") {
            chosen = annotationScaleList(ctx.doc().header());
        } else {
            auto r = co_await ctx.getString("Scale, N or 1:N");
            if (!r.ok())
                co_return;
            const auto parsed = parseScaleN(r.text);
            if (!parsed || !(*parsed > 0)) {
                ctx.message("Enter the scale as N or 1:N, a number above zero.");
                co_return;
            }
            chosen = {*parsed};
        }
        const Header header = ctx.doc().header();
        std::size_t changed = 0;
        for (EntityId id : sel.ids) {
            const Entity* e = ctx.doc().find(id);
            if (!e || !canBeAnnotative(*e) || !e->common.annotative)
                continue;
            std::vector<double> list = e->common.annoScales.get();
            if (list.empty())
                list = {currentAnnotationScale(header)};
            const double oldN = list.front();
            if (k.keyword == "Delete") {
                for (double v : chosen)
                    list.erase(std::remove(list.begin(), list.end(), v), list.end());
                if (list.empty()) {
                    ctx.message("An object keeps at least one scale; " + std::to_string(id.value) +
                                " was left alone.");
                    continue;
                }
            } else {
                for (double v : chosen)
                    if (std::find(list.begin(), list.end(), v) == list.end())
                        list.push_back(v);
            }
            if (ScaleList(list) == e->common.annoScales)
                continue;
            const double newN = list.front();
            if (ctx.tx().modify(id, [&](Entity& x) {
                    x.common.annoScales = ScaleList(list);
                    if (auto* ins = entityAs<InsertData>(x))
                        ins->scale = ins->scale * (newN / oldN);
                }))
                ++changed;
        }
        ctx.message(std::to_string(changed) + " object(s) changed");
        ctx.setUndoLabel("Object scale");
    }
};

// ANNORESET: the selected annotative objects go back to the current-scale representation: the
// current scale becomes the one they are drawn at in model space (a symbol follows it). The list
// of scales is kept; an object that does not list the current scale is left alone.
class AnnoResetCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ANNORESET"; }
    std::string_view description() const override {
        return "Reset annotative objects to the current annotation scale";
    }

    Task run(CommandContext& ctx) override {
        auto sel = co_await ctx.getSelection("Select annotative objects",
                                             cmd::SelectionOptions{.filter = [](const Entity& e) {
                                                 return canBeAnnotative(e) && e.common.annotative;
                                             }});
        if (!sel.ok() || sel.ids.empty())
            co_return;
        const double cur = currentAnnotationScale(ctx.doc().header());
        std::size_t changed = 0, skipped = 0;
        for (EntityId id : sel.ids) {
            const Entity* e = ctx.doc().find(id);
            if (!e || !canBeAnnotative(*e) || !e->common.annotative)
                continue;
            std::vector<double> list = e->common.annoScales.get();
            if (list.empty() || list.front() == cur)
                continue;  // already follows the current scale
            auto at = std::find(list.begin(), list.end(), cur);
            if (at == list.end()) {
                ++skipped;
                continue;
            }
            const double oldN = list.front();
            list.erase(at);
            list.insert(list.begin(), cur);
            if (ctx.tx().modify(id, [&](Entity& x) {
                    x.common.annoScales = ScaleList(list);
                    if (auto* ins = entityAs<InsertData>(x))
                        ins->scale = ins->scale * (cur / oldN);
                }))
                ++changed;
        }
        ctx.message(std::to_string(changed) + " object(s) reset to " + scaleText(cur) +
                    (skipped ? "; " + std::to_string(skipped) +
                                   " do not list that scale (use OBJECTSCALE to add it)"
                             : std::string()));
        ctx.setUndoLabel("Annotation reset");
    }
};

// ANNOUPDATE: text in an annotative text style becomes annotative at the current scale, so it
// matches its style again. Objects that already are annotative are left as they are.
class AnnoUpdateCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ANNOUPDATE"; }
    std::string_view description() const override {
        return "Update objects to match the annotative setting of their styles";
    }

    Task run(CommandContext& ctx) override {
        auto sel = co_await ctx.getSelection(
            "Select text to update", cmd::SelectionOptions{.filter = [](const Entity& e) {
                return kind(e) == EntityKind::Text || kind(e) == EntityKind::MText;
            }});
        if (!sel.ok() || sel.ids.empty())
            co_return;
        std::size_t changed = 0;
        for (EntityId id : sel.ids) {
            const Entity* e = ctx.doc().find(id);
            if (!e || e->common.annotative)
                continue;
            Entity probe = *e;
            applyStyleAnnotative(ctx.doc(), probe);
            if (!probe.common.annotative)
                continue;
            if (ctx.tx().modify(id, [&](Entity& x) { applyStyleAnnotative(ctx.doc(), x); }))
                ++changed;
        }
        ctx.message(std::to_string(changed) + " object(s) updated");
        ctx.setUndoLabel("Annotation update");
    }
};

// Redraws every annotative object (after the zones changed, the scale it is drawn at may too).
void touchAnnotative(CommandContext& ctx) {
    std::vector<EntityId> ids;
    ctx.doc().forEachEntity([&](const Entity& e) {
        if (e.common.annotative && !ctx.doc().ownerBlock(e.common.id))
            ids.push_back(e.common.id);
    });
    for (EntityId id : ids)
        (void)ctx.tx().modify(id, [](Entity&) {});
}

std::size_t countInZone(const Document& doc, const AnnotationZone& z) {
    std::size_t n = 0;
    doc.forEachEntity([&](const Entity& e) {
        if (!e.common.annotative || doc.ownerBlock(e.common.id))
            return;
        if (const auto at = annotationAnchor(e); at && z.box.contains(*at))
            ++n;
    });
    return n;
}

std::string lowerAscii(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ANNOZONE [AZONE]: named rectangles of model space with their own annotation scale. Inside a
// zone, annotative text, dimensions, leaders, tables and hatches are drawn (and plotted from the
// model) at the zone's scale, so a 1:20 detail sits next to a 1:100 plan in one model.
class AnnoZoneCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ANNOZONE"; }
    std::vector<std::string> defaultAliases() const override { return {"AZONE"}; }
    std::string_view description() const override {
        return "Give an area of the model its own annotation scale";
    }

    Task run(CommandContext& ctx) override {
        cmd::PointOptions po;
        po.keywords = {"Scale", "Rename", "Delete", "List", "Frames"};
        auto first = co_await ctx.getPoint(
            "First corner of new zone or [Scale/Rename/Delete/List/Frames]", po);
        if (first.status == cmd::Status::Keyword) {
            if (first.keyword == "List")
                listZones(ctx);
            else if (first.keyword == "Frames")
                co_await frames(ctx);
            else
                co_await editZone(ctx, first.keyword);
            co_return;
        }
        if (!first.ok())
            co_return;
        cmd::PointOptions next;
        next.base = first.point;
        next.preview = [a = first.point](Vec2 c, DisplayList& out) {
            DisplayPolyline pl;
            pl.pts = {a, {c.x, a.y}, c, {a.x, c.y}};
            pl.closed = true;
            out.items.emplace_back(std::move(pl));
        };
        auto second = co_await ctx.getPoint("Opposite corner", std::move(next));
        if (!second.ok())
            co_return;
        AnnotationZone z;
        z.box.expand(first.point);
        z.box.expand(second.point);
        if (!(z.box.width() > 0) || !(z.box.height() > 0)) {
            ctx.message("The zone needs width and height.");
            co_return;
        }
        const Header& h = ctx.doc().header();
        const std::string def = scaleText(currentAnnotationScale(h)).substr(2);
        cmd::StringOptions so;
        so.defaultValue = def;
        auto r =
            co_await ctx.getString("Annotation scale in this zone, N or 1:N <" + def + ">", so);
        if (!r.ok())
            co_return;
        const auto parsed = parseScaleN(r.text.empty() ? def : r.text);
        if (!parsed || !(*parsed > 0)) {
            ctx.message("Enter the scale as N or 1:N, a number above zero.");
            co_return;
        }
        z.scale = *parsed;
        std::vector<AnnotationZone> zones = annotationZones(ctx.doc().header());
        const std::string defName = "Zone " + std::to_string(zones.size() + 1);
        cmd::StringOptions no;
        no.defaultValue = defName;
        auto nm = co_await ctx.getString("Zone name <" + defName + ">", no);
        if (!nm.ok() && nm.status != cmd::Status::None)
            co_return;
        z.name = nm.text.empty() ? defName : nm.text;
        zones.push_back(z);
        (void)ctx.tx().modifyHeader([&](Header& hh) { setAnnotationZones(hh, zones); });
        touchAnnotative(ctx);
        ctx.message("Zone \"" + z.name + "\" at " + scaleText(z.scale) + ": " +
                    std::to_string(countInZone(ctx.doc(), z)) +
                    " annotative object(s) inside are drawn at that scale.");
        ctx.setUndoLabel("Annotation zone");
    }

private:
    static void listZones(CommandContext& ctx) {
        const auto zones = annotationZones(ctx.doc().header());
        if (zones.empty()) {
            ctx.message("No annotation zones. The whole model uses " +
                        scaleText(currentAnnotationScale(ctx.doc().header())) + ".");
            return;
        }
        for (const AnnotationZone& z : zones) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "  %s  (%g x %g)", scaleText(z.scale).c_str(),
                          z.box.width(), z.box.height());
            ctx.message(z.name + buf);
        }
        ctx.message("Outside the zones: " + scaleText(currentAnnotationScale(ctx.doc().header())));
    }

    static Task frames(CommandContext& ctx) {
        const bool now = annotationZonesShown(ctx.doc().header());
        cmd::KeywordOptions ko;
        ko.keywords = {"On", "Off"};
        ko.defaultKeyword = now ? "Off" : "On";
        auto k = co_await ctx.getKeyword(
            std::string("Show zone frames [On/Off] <") + (now ? "Off" : "On") + ">", ko);
        if (!k.ok())
            co_return;
        const bool on = k.keyword == "On";
        (void)ctx.tx().modifyHeader([&](Header& h) {
            if (on)
                h.custom.erase(kAnnoZonesShowKey);
            else
                h.custom[kAnnoZonesShowKey] = "0";
        });
        ctx.message(on ? "Zone frames shown (they never plot)." : "Zone frames hidden.");
        ctx.setUndoLabel("Annotation zone frames");
    }

    static Task editZone(CommandContext& ctx, std::string action) {
        std::vector<AnnotationZone> zones = annotationZones(ctx.doc().header());
        if (zones.empty()) {
            ctx.message("No annotation zones yet: pick two corners to make one.");
            co_return;
        }
        cmd::PointOptions po;
        po.freeText = true;
        po.numberAsText = true;
        po.snap = false;
        auto pick = co_await ctx.getPoint("Click inside the zone or type its name", po);
        std::size_t idx = zones.size();
        if (pick.ok()) {
            double area = 0;
            for (std::size_t i = 0; i < zones.size(); ++i) {
                const double a = zones[i].box.width() * zones[i].box.height();
                if (zones[i].box.contains(pick.point) && (idx == zones.size() || a < area)) {
                    idx = i;
                    area = a;
                }
            }
        } else if (pick.status == cmd::Status::Keyword) {
            const std::string want = lowerAscii(pick.keyword);
            for (std::size_t i = 0; i < zones.size(); ++i)
                if (lowerAscii(zones[i].name) == want)
                    idx = i;
        } else {
            co_return;
        }
        if (idx == zones.size()) {
            ctx.message("No zone there.");
            co_return;
        }
        AnnotationZone& z = zones[idx];
        const std::string old = z.name;
        if (action == "Delete") {
            zones.erase(zones.begin() + static_cast<std::ptrdiff_t>(idx));
            ctx.message("Zone \"" + old + "\" deleted.");
        } else if (action == "Rename") {
            cmd::StringOptions so;
            so.defaultValue = old;
            auto r = co_await ctx.getString("New name <" + old + ">", so);
            if (!r.ok() || r.text.empty())
                co_return;
            z.name = r.text;
            ctx.message("Zone \"" + old + "\" is now \"" + z.name + "\".");
        } else {
            const std::string def = scaleText(z.scale).substr(2);
            cmd::StringOptions so;
            so.defaultValue = def;
            auto r = co_await ctx.getString(
                "Annotation scale of \"" + old + "\", N or 1:N <" + def + ">", so);
            if (!r.ok())
                co_return;
            const auto parsed = parseScaleN(r.text.empty() ? def : r.text);
            if (!parsed || !(*parsed > 0)) {
                ctx.message("Enter the scale as N or 1:N, a number above zero.");
                co_return;
            }
            z.scale = *parsed;
            ctx.message("Zone \"" + old + "\" now uses " + scaleText(z.scale) + ".");
        }
        (void)ctx.tx().modifyHeader([&](Header& h) { setAnnotationZones(h, zones); });
        touchAnnotative(ctx);
        ctx.setUndoLabel("Annotation zone");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeAnnoResetCommand() {
    return std::make_unique<AnnoResetCommand>();
}
std::unique_ptr<cmd::Command> makeAnnoUpdateCommand() {
    return std::make_unique<AnnoUpdateCommand>();
}
std::unique_ptr<cmd::Command> makeObjectScaleCommand() {
    return std::make_unique<ObjectScaleCommand>();
}
std::unique_ptr<cmd::Command> makeAnnotativeCommand() {
    return std::make_unique<AnnotativeCommand>();
}
std::unique_ptr<cmd::Command> makeAnnoNewCommand() {
    return std::make_unique<AnnoNewCommand>();
}
std::unique_ptr<cmd::Command> makeAnnoScaleCommand() {
    return std::make_unique<AnnoScaleCommand>();
}
std::unique_ptr<cmd::Command> makeScaleListCommand() {
    return std::make_unique<ScaleListCommand>();
}
std::unique_ptr<cmd::Command> makeAnnoZoneCommand() {
    return std::make_unique<AnnoZoneCommand>();
}

}  // namespace tbcad::textcmd
