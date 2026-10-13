#include <cmath>
#include <cstdlib>
#include <string>

#include "commands.h"
#include "tbcad/cmd/cmd.h"
#include "tbcad/text/text.h"
#include "tbcad/textcmd/textcmd.h"

namespace tbcad::textcmd {

namespace {

using cmd::CommandContext;
using cmd::Status;
using cmd::Task;

constexpr const char* kHeightKey = "text.height";
constexpr const char* kJustifyKey = "text.justify";
constexpr const char* kAttachKey = "mtext.attach";

double settingNumber(CommandContext& ctx, const char* key, double fallback) {
    if (auto s = ctx.host().setting(key)) {
        char* end = nullptr;
        double v = std::strtod(s->c_str(), &end);
        if (end != s->c_str() && v > 0 && std::isfinite(v))
            return v;
    }
    return fallback;
}

void rememberNumber(CommandContext& ctx, const char* key, double v) {
    ctx.host().setSetting(key, std::to_string(v));
}

struct Justify {
    const char* name;
    HAlign h;
    VAlign v;
};
constexpr Justify kJustify[] = {
    {"Left", HAlign::Left, VAlign::Baseline},
    {"Center", HAlign::Center, VAlign::Baseline},
    {"Right", HAlign::Right, VAlign::Baseline},
    {"Aligned", HAlign::Aligned, VAlign::Baseline},
    {"Middle", HAlign::Middle, VAlign::Baseline},
    {"Fit", HAlign::Fit, VAlign::Baseline},
    {"TL", HAlign::Left, VAlign::Top},
    {"TC", HAlign::Center, VAlign::Top},
    {"TR", HAlign::Right, VAlign::Top},
    {"ML", HAlign::Left, VAlign::Middle},
    {"MC", HAlign::Middle, VAlign::Middle},
    {"MR", HAlign::Right, VAlign::Middle},
    {"BL", HAlign::Left, VAlign::Bottom},
    {"BC", HAlign::Center, VAlign::Bottom},
    {"BR", HAlign::Right, VAlign::Bottom},
};

const Justify* findJustify(const std::string& name) {
    for (const Justify& j : kJustify)
        if (name == j.name)
            return &j;
    return nullptr;
}

std::vector<std::string> justifyKeywords() {
    std::vector<std::string> k;
    for (const Justify& j : kJustify)
        k.emplace_back(j.name);
    return k;
}

struct Attach {
    const char* name;
    Attachment a;
};
constexpr Attach kAttach[] = {
    {"TL", Attachment::TopLeft},      {"TC", Attachment::TopCenter},
    {"TR", Attachment::TopRight},     {"ML", Attachment::MiddleLeft},
    {"MC", Attachment::MiddleCenter}, {"MR", Attachment::MiddleRight},
    {"BL", Attachment::BottomLeft},   {"BC", Attachment::BottomCenter},
    {"BR", Attachment::BottomRight},
};

std::optional<TextStyleId> styleByName(CommandContext& ctx, const std::string& name) {
    if (const TextStyle* s = ctx.doc().textStyleByName(name))
        return s->id;
    ctx.message("Unknown text style: " + name);
    return std::nullopt;
}

bool isTextEntity(const Entity& e) {
    EntityKind k = kind(e);
    return k == EntityKind::Text || k == EntityKind::MText;
}

// ---- TEXT -----------------------------------------------------------------------------------

class TextCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "TEXT"; }
    std::vector<std::string> defaultAliases() const override { return {"T", "DT"}; }
    std::string_view description() const override { return "Single-line text"; }

    Task run(CommandContext& ctx) override {
        double height = settingNumber(ctx, kHeightKey, 2.5);
        Justify just = kJustify[0];
        if (auto s = ctx.host().setting(kJustifyKey))
            if (const Justify* j = findJustify(*s))
                just = *j;
        TextStyleId style{};  // null = the document's current style

        // Start point, with the Justify / Style options.
        Vec2 start{};
        for (;;) {
            auto r = co_await ctx.getPoint("Specify start point of text", startOptions());
            if (r.status == Status::Keyword && r.keyword == "Justify") {
                cmd::KeywordOptions ko;
                ko.keywords = justifyKeywords();
                auto k = co_await ctx.getKeyword("Enter an option", ko);
                if (k.ok())
                    if (const Justify* j = findJustify(k.keyword))
                        just = *j;
                continue;
            }
            if (r.status == Status::Keyword && r.keyword == "Style") {
                cmd::StringOptions so;
                so.defaultValue = currentStyleName(ctx);
                auto n = co_await ctx.getString("Enter style name", so);
                if (n.ok())
                    if (auto id = styleByName(ctx, n.text))
                        style = *id;
                continue;
            }
            if (!r.ok())
                co_return;
            start = r.point;
            break;
        }

        TextData d;
        d.position = d.alignPoint = start;
        d.h = just.h;
        d.v = just.v;
        d.style = style;
        const bool spans = just.h == HAlign::Aligned || just.h == HAlign::Fit;

        if (spans) {
            cmd::PointOptions po;
            po.base = start;
            po.allowNone = false;
            auto e = co_await ctx.getPoint("Specify second endpoint of text baseline", po);
            if (!e.ok())
                co_return;
            d.alignPoint = e.point;
        }
        if (just.h != HAlign::Aligned) {
            cmd::DistanceOptions dop;
            dop.base = start;
            dop.defaultValue = height;
            dop.allowZero = false;
            auto h = co_await ctx.getDistance("Specify height", dop);
            if (!h.ok())
                co_return;
            height = h.value;
        }
        if (!spans) {
            cmd::AngleOptions ao;
            ao.base = start;
            ao.defaultValue = 0.0;
            auto a = co_await ctx.getAngle("Specify rotation angle of text", ao);
            if (!a.ok())
                co_return;
            d.rotation = a.value;
        }
        d.height = height;
        if (just.h != HAlign::Aligned)
            rememberNumber(ctx, kHeightKey, height);
        ctx.host().setSetting(kJustifyKey, just.name);

        auto t = co_await ctx.getString("Enter text");
        if (!t.ok())
            co_return;
        d.utf8 = t.text;
        Entity created{{}, std::move(d)};
        applyCurrentProperties(ctx.doc().header(), created);
        auto id = ctx.tx().add(std::move(created));
        if (!id)
            ctx.message(id.error().message);
    }

private:
    static std::string currentStyleName(CommandContext& ctx) {
        const TextStyle* s = ctx.doc().textStyle(ctx.doc().header().currentTextStyle);
        return s ? s->name : std::string("Standard");
    }
    static cmd::PointOptions startOptions() {
        cmd::PointOptions po;
        po.keywords = {"Justify", "Style"};
        po.allowNone = false;
        return po;
    }
};

// ---- MTEXT ------------------------------------------------------------------------------------

struct MTextState {
    double height = 2.5;
    double rotation = 0;
    double width = -1;  // -1 = from the corners
    Attachment attach = Attachment::TopLeft;
    TextStyleId style{};
};

// Handles one of the MTEXT options at the corner prompts.
Task mtextOption(CommandContext& ctx, MTextState& st, const std::string& option, Vec2 base) {
    if (option == "Height") {
        cmd::DistanceOptions o;
        o.base = base;
        o.defaultValue = st.height;
        o.allowZero = false;
        auto r = co_await ctx.getDistance("Specify height", o);
        if (r.ok())
            st.height = r.value;
    } else if (option == "Justify") {
        cmd::KeywordOptions o;
        for (const Attach& a : kAttach)
            o.keywords.emplace_back(a.name);
        auto r = co_await ctx.getKeyword("Enter justification", o);
        if (r.ok())
            for (const Attach& a : kAttach)
                if (r.keyword == a.name)
                    st.attach = a.a;
    } else if (option == "Rotation") {
        cmd::AngleOptions o;
        o.base = base;
        o.defaultValue = st.rotation;
        auto r = co_await ctx.getAngle("Specify rotation angle", o);
        if (r.ok())
            st.rotation = r.value;
    } else if (option == "Style") {
        cmd::StringOptions o;
        auto r = co_await ctx.getString("Enter style name", o);
        if (r.ok())
            if (auto id = styleByName(ctx, r.text))
                st.style = *id;
    } else if (option == "Width") {
        cmd::DistanceOptions o;
        o.base = base;
        o.defaultValue = st.width > 0 ? st.width : 0.0;
        auto r = co_await ctx.getDistance("Specify width (0 = no wrapping)", o);
        if (r.ok())
            st.width = r.value;
    }
}

class MTextCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "MTEXT"; }
    std::vector<std::string> defaultAliases() const override { return {"MT"}; }
    std::string_view description() const override { return "Multiline text"; }

    Task run(CommandContext& ctx) override {
        MTextState st;
        st.height = settingNumber(ctx, kHeightKey, 2.5);
        if (auto s = ctx.host().setting(kAttachKey))
            for (const Attach& a : kAttach)
                if (*s == a.name)
                    st.attach = a.a;

        const std::vector<std::string> keywords{"Height", "Justify", "Rotation", "Style", "Width"};
        Vec2 first{};
        for (;;) {
            cmd::PointOptions po;
            po.keywords = keywords;
            po.allowNone = false;
            auto r = co_await ctx.getPoint("Specify first corner", po);
            if (r.status == Status::Keyword) {
                co_await mtextOption(ctx, st, r.keyword, {});
                continue;
            }
            if (!r.ok())
                co_return;
            first = r.point;
            break;
        }
        Vec2 second{};
        for (;;) {
            cmd::PointOptions po;
            po.base = first;
            po.keywords = keywords;
            po.allowNone = false;
            po.preview = [first, rot = st.rotation](Vec2 cursor, DisplayList& out) {
                out.items.emplace_back(rectangle(first, cursor, rot));
            };
            auto r = co_await ctx.getPoint("Specify opposite corner", po);
            if (r.status == Status::Keyword) {
                co_await mtextOption(ctx, st, r.keyword, first);
                continue;
            }
            if (!r.ok())
                co_return;
            second = r.point;
            break;
        }

        // The rectangle in the rotated frame whose origin is the first corner.
        const double c = std::cos(st.rotation), s = std::sin(st.rotation);
        Vec2 v = second - first;
        Vec2 u{c * v.x + s * v.y, -s * v.x + c * v.y};
        const double xmin = std::min(0.0, u.x), xmax = std::max(0.0, u.x);
        const double ymin = std::min(0.0, u.y), ymax = std::max(0.0, u.y);
        Vec2 local{};
        switch (st.attach) {
            case Attachment::TopLeft:
            case Attachment::MiddleLeft:
            case Attachment::BottomLeft:
                local.x = xmin;
                break;
            case Attachment::TopCenter:
            case Attachment::MiddleCenter:
            case Attachment::BottomCenter:
                local.x = (xmin + xmax) / 2;
                break;
            default:
                local.x = xmax;
        }
        switch (st.attach) {
            case Attachment::TopLeft:
            case Attachment::TopCenter:
            case Attachment::TopRight:
                local.y = ymax;
                break;
            case Attachment::MiddleLeft:
            case Attachment::MiddleCenter:
            case Attachment::MiddleRight:
                local.y = (ymin + ymax) / 2;
                break;
            default:
                local.y = ymin;
        }

        MTextData d;
        d.position = first + Vec2{c * local.x - s * local.y, s * local.x + c * local.y};
        d.rotation = st.rotation;
        d.attach = st.attach;
        d.height = st.height;
        d.style = st.style;
        d.width = st.width >= 0 ? st.width : (xmax - xmin);
        if (d.width < tol::kLinear)
            d.width = 0;

        auto t = co_await ctx.getString("Enter text (\\P starts a new line)");
        if (!t.ok())
            co_return;
        d.utf8 = t.text;
        rememberNumber(ctx, kHeightKey, st.height);
        for (const Attach& a : kAttach)
            if (a.a == st.attach)
                ctx.host().setSetting(kAttachKey, a.name);
        Entity created{{}, std::move(d)};
        applyCurrentProperties(ctx.doc().header(), created);
        auto id = ctx.tx().add(std::move(created));
        if (!id)
            ctx.message(id.error().message);
    }

private:
    static DisplayPolyline rectangle(Vec2 a, Vec2 b, double rot) {
        const double c = std::cos(rot), s = std::sin(rot);
        Vec2 v = b - a;
        Vec2 u{c * v.x + s * v.y, -s * v.x + c * v.y};
        auto world = [&](Vec2 l) { return a + Vec2{c * l.x - s * l.y, s * l.x + c * l.y}; };
        DisplayPolyline pl;
        pl.closed = true;
        pl.pts = {a, world({u.x, 0}), b, world({0, u.y})};
        return pl;
    }
};

// ---- TEXTEDIT ----------------------------------------------------------------------------------

class TextEditCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "TEXTEDIT"; }
    std::vector<std::string> defaultAliases() const override { return {"ED", "DDEDIT"}; }
    std::string_view description() const override { return "Edit the content of a text object"; }

    Task run(CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.single = true;
        so.filter = isTextEntity;
        auto sel = co_await ctx.getSelection("Select an annotation object", so);
        if (!sel.ok() || sel.ids.empty())
            co_return;
        const Entity* e = ctx.doc().find(sel.ids.front());
        if (!e || !isTextEntity(*e))
            co_return;
        std::string current;
        if (const auto* t = entityAs<TextData>(*e))
            current = t->utf8;
        else
            current = entityAs<MTextData>(*e)->utf8;

        cmd::StringOptions o;
        o.defaultValue = current;
        auto r = co_await ctx.getString("Enter new text", o);
        if (!r.ok())
            co_return;
        const std::string value = r.text;
        auto res = ctx.tx().modify(sel.ids.front(), [&](Entity& en) {
            if (auto* t = entityAs<TextData>(en))
                t->utf8 = value;
            else if (auto* m = entityAs<MTextData>(en))
                m->utf8 = value;
        });
        if (!res)
            ctx.message(res.error().message);
        co_return;
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeTextCommand() {
    return std::make_unique<TextCommand>();
}
std::unique_ptr<cmd::Command> makeMTextCommand() {
    return std::make_unique<MTextCommand>();
}
std::unique_ptr<cmd::Command> makeTextEditCommand() {
    return std::make_unique<TextEditCommand>();
}

}  // namespace tbcad::textcmd
