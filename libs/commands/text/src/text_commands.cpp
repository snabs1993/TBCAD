#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

#include "commands.h"
#include "tbcad/cmd/cmd.h"
#include "tbcad/text/annotation_size.h"
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

// Text that suits the drawing: 2.5 mm on a sheet-sized drawing, larger when the annotation scale
// is set, when the view is zoomed far out or when the drawing is big (a 20 m plan printed on a
// sheet needs about extent/160 text).
double autoTextHeight(CommandContext& ctx) {
    constexpr double kBase = 2.5;
    return kBase * text::suggestedAnnotationScale(ctx.doc(), ctx.host().view().scale,
                                                  static_cast<bool>(ctx.host().drawingOwner()),
                                                  kBase);
}

// The fixed height of a text style (0 = variable). A null id means the document's current style.
double fixedStyleHeight(CommandContext& ctx, TextStyleId id) {
    const Document& doc = ctx.doc();
    const TextStyle* s = doc.textStyle(id ? id : doc.header().currentTextStyle);
    return s && s->height > 0 ? s->height : 0;
}

double settingNumber(CommandContext& ctx, const char* key, double fallback) {
    if (auto s = ctx.host().setting(key)) {
        char* end = nullptr;
        double v = std::strtod(s->c_str(), &end);
        if (end != s->c_str() && v > 0 && std::isfinite(v))
            return v;
    }
    return fallback;
}

// Only a height the user chose is remembered; the automatic one follows the next drawing.
void rememberHeight(CommandContext& ctx, double v) {
    const bool automatic = std::fabs(v - autoTextHeight(ctx)) < 1e-9;
    ctx.host().setSetting(kHeightKey, automatic ? std::string() : std::to_string(v));
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
// Settings the MTEXT text box edits while the text is typed (never saved).
constexpr const char* kEditHeightKey = "tmp.mtext.height";
constexpr const char* kEditAttachKey = "tmp.mtext.attach";
constexpr const char* kEditStyleKey = "tmp.mtext.style";
// Columns ("1" = none), "dynamic" or "static", the static height (0 = the rectangle's), and the
// background mask ("1" = on).
constexpr const char* kEditColumnsKey = "tmp.mtext.columns";
constexpr const char* kEditColumnKindKey = "tmp.mtext.colkind";
constexpr const char* kEditColumnHeightKey = "tmp.mtext.colheight";
constexpr const char* kEditMaskKey = "tmp.mtext.bgmask";
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
    std::vector<std::string> defaultAliases() const override { return {"DT", "DTEXT"}; }
    std::string_view description() const override { return "Single-line text"; }

    Task run(CommandContext& ctx) override {
        double height = settingNumber(ctx, kHeightKey, autoTextHeight(ctx));
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
        // A style with a fixed height sets it; no question, as in AutoCAD.
        const double fixed = fixedStyleHeight(ctx, style);
        if (fixed > 0) {
            height = fixed;
        } else if (just.h != HAlign::Aligned) {
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
            ao.inUcs = true;
            auto a = co_await ctx.getAngle("Specify rotation angle of text", ao);
            if (!a.ok())
                co_return;
            d.rotation = a.value;
        }
        d.height = height;
        if (just.h != HAlign::Aligned && fixed <= 0)
            rememberHeight(ctx, height);
        ctx.host().setSetting(kJustifyKey, just.name);

        cmd::StringOptions textOpts;
        textOpts.editor = cmd::TextEditorHint{{d.position.x, d.position.y + d.height}, 0, 0, false};
        textOpts.textPreview = [&ctx, d](const std::string& typed, DisplayList& out) {
            TextData shown = d;
            shown.utf8 = typed;
            Entity e{{}, std::move(shown)};
            e.common.layer = ctx.doc().header().currentLayer;
            geom::DisplayContext dc;
            dc.pixelSizeMm = std::max(ctx.host().view().scale, 1e-9);
            geom::buildDisplay(ctx.doc(), e, dc, out);
        };
        auto t = co_await ctx.getString("Enter text", std::move(textOpts));
        if (!t.ok())
            co_return;
        d.utf8 = t.text;
        Entity created{{}, std::move(d)};
        applyCurrentProperties(ctx.doc().header(), created);
        applyStyleAnnotative(ctx.doc(), created);
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
    bool heightChosen = false;  // the Height option was used: it beats the style's fixed height
    double rotation = 0;
    double width = -1;  // -1 = from the corners
    Attachment attach = Attachment::TopLeft;
    TextStyleId style{};
    int columns = 1;  // > 1: the corners give the whole width, split into this many columns
};

// Handles one of the MTEXT options at the corner prompts.
Task mtextOption(CommandContext& ctx, MTextState& st, const std::string& option, Vec2 base) {
    if (option == "Height") {
        cmd::DistanceOptions o;
        o.base = base;
        o.defaultValue = st.height;
        o.allowZero = false;
        auto r = co_await ctx.getDistance("Specify height", o);
        if (r.ok()) {
            st.height = r.value;
            st.heightChosen = true;
        }
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
        o.inUcs = true;
        o.defaultValue = st.rotation - ctx.host().ucsAngle();
        auto r = co_await ctx.getAngle("Specify rotation angle", o);
        if (r.ok())
            st.rotation = r.value;
    } else if (option == "Style") {
        cmd::StringOptions o;
        auto r = co_await ctx.getString("Enter style name", o);
        if (r.ok())
            if (auto id = styleByName(ctx, r.text))
                st.style = *id;
    } else if (option == "Columns") {
        cmd::IntOptions o;
        o.defaultValue = st.columns;
        o.min = 1;
        o.max = 8;
        auto r = co_await ctx.getInteger("Number of columns", o);
        if (r.ok())
            st.columns = static_cast<int>(r.value);
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
    std::vector<std::string> defaultAliases() const override { return {"T", "MT"}; }
    std::string_view description() const override { return "Multiline text"; }

    Task run(CommandContext& ctx) override {
        MTextState st;
        st.rotation = ctx.host().ucsAngle();  // the lines run along the UCS X axis
        st.height = settingNumber(ctx, kHeightKey, autoTextHeight(ctx));
        if (auto s = ctx.host().setting(kAttachKey))
            for (const Attach& a : kAttach)
                if (*s == a.name)
                    st.attach = a.a;

        const std::vector<std::string> keywords{"Height", "Justify", "Rotation",
                                                "Style",  "Width",   "Columns"};
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
        // Where the text goes for a justification: the matching point of the rectangle.
        const auto place = [first, c, s, xmin, xmax, ymin, ymax](Attachment a) {
            Vec2 local{};
            switch (a) {
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
            switch (a) {
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
            return first + Vec2{c * local.x - s * local.y, s * local.x + c * local.y};
        };

        MTextData d;
        d.position = place(st.attach);
        d.rotation = st.rotation;
        d.attach = st.attach;
        const double fixed = st.heightChosen ? 0 : fixedStyleHeight(ctx, st.style);
        d.height = fixed > 0 ? fixed : st.height;
        d.style = st.style;
        d.width = st.width >= 0 ? st.width : (xmax - xmin);
        if (d.width < tol::kLinear)
            d.width = 0;
        const double blockW = d.width;
        const double blockH = ymax - ymin;
        // The rectangle is the whole text block: cut it into columns with a gap of two lines.
        // Static columns are as tall as the rectangle (or the height typed in the editor).
        const auto splitColumns = [blockW, blockH](MTextData& m, int cols, bool fixedHeight,
                                                   double colHeight) {
            m.width = blockW;
            m.columns = 1;
            m.columnGap = 0;
            m.columnHeight = 0;
            if (cols <= 1 || blockW <= 0)
                return;
            const double gap = 2 * m.height;
            const double each = (blockW - gap * (cols - 1)) / cols;
            if (each <= m.height)
                return;
            m.columns = static_cast<std::uint8_t>(std::min(cols, 16));
            m.columnGap = gap;
            m.width = each;
            if (fixedHeight)
                m.columnHeight = colHeight > 0 ? colHeight : blockH;
        };
        splitColumns(d, st.columns, false, 0);

        // The text box offers Height, Justify and Style while typing: it changes these settings
        // and the drawing follows them, so the command line is not asked again.
        auto attachName = [](Attachment a) {
            for (const Attach& k : kAttach)
                if (k.a == a)
                    return std::string(k.name);
            return std::string("TL");
        };
        ctx.host().setSetting(kEditHeightKey, std::to_string(d.height));
        ctx.host().setSetting(kEditAttachKey, attachName(d.attach));
        if (const TextStyle* ts = ctx.doc().textStyle(d.style))
            ctx.host().setSetting(kEditStyleKey, ts->name);
        ctx.host().setSetting(kEditColumnsKey, std::to_string(st.columns));
        ctx.host().setSetting(kEditColumnKindKey, "dynamic");
        ctx.host().setSetting(kEditColumnHeightKey, "0");
        ctx.host().setSetting(kEditMaskKey, "0");
        const auto current = [&ctx, d, place, splitColumns]() {
            MTextData m = d;
            if (auto h = ctx.host().setting(kEditHeightKey)) {
                const double v = std::atof(h->c_str());
                if (v > 0)
                    m.height = v;
            }
            if (auto a = ctx.host().setting(kEditAttachKey))
                for (const Attach& k : kAttach)
                    if (*a == k.name)
                        m.attach = k.a;
            if (auto n = ctx.host().setting(kEditStyleKey))
                if (const TextStyle* ts = ctx.doc().textStyleByName(*n))
                    m.style = ts->id;
            const auto num = [&ctx](const char* key) {
                const auto v = ctx.host().setting(key);
                return v ? std::atof(v->c_str()) : 0.0;
            };
            splitColumns(m, static_cast<int>(num(kEditColumnsKey)),
                         ctx.host().setting(kEditColumnKindKey).value_or("") == "static",
                         num(kEditColumnHeightKey));
            m.bgMode = ctx.host().setting(kEditMaskKey).value_or("0") == "1" ? 1 : 0;
            m.position = place(m.attach);
            return m;
        };

        cmd::StringOptions textOpts;
        // The text is typed in a box at the rectangle just drawn (the command line works too).
        textOpts.editor =
            cmd::TextEditorHint{{std::min(first.x, second.x), std::max(first.y, second.y)},
                                std::abs(second.x - first.x),
                                std::abs(second.y - first.y)};
        textOpts.textPreview = [&ctx, current](const std::string& typed, DisplayList& out) {
            MTextData shown = current();
            shown.utf8 = typed;
            Entity e{{}, std::move(shown)};
            e.common.layer = ctx.doc().header().currentLayer;
            geom::DisplayContext dc;
            dc.pixelSizeMm = std::max(ctx.host().view().scale, 1e-9);
            geom::buildDisplay(ctx.doc(), e, dc, out);
        };
        auto t = co_await ctx.getString("Enter text (\\P starts a new line)", std::move(textOpts));
        if (!t.ok())
            co_return;
        const MTextData chosen = current();
        const bool heightEdited = chosen.height != d.height;
        d = chosen;
        d.utf8 = t.text;
        if (fixed <= 0 || heightEdited)
            rememberHeight(ctx, d.height);
        ctx.host().setSetting(kAttachKey, attachName(d.attach));
        Entity created{{}, std::move(d)};
        applyCurrentProperties(ctx.doc().header(), created);
        applyStyleAnnotative(ctx.doc(), created);
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
    std::vector<std::string> defaultAliases() const override { return {"ED", "DDEDIT", "MTEDIT"}; }
    std::string_view description() const override { return "Edit the content of a text object"; }

    // TEXTEDITMODE ("textedit.mode"): 0 asks for another object after each edit until Enter or
    // Esc, 1 ends after one. An object selected before the command (a double-click, the Text
    // panel) is always a single edit. Undo takes back the last edit of this command.
    Task run(CommandContext& ctx) override {
        bool multiple = ctx.host().setting("textedit.mode").value_or("0") != "1";
        struct Done {
            EntityId id;
            std::string before;
        };
        std::vector<Done> done;
        for (bool first = true;; first = false) {
            cmd::SelectionOptions so;
            so.single = true;
            so.filter = isTextEntity;
            so.allowPreselection = first;
            so.keywords = done.empty() ? std::vector<std::string>{"Mode"}
                                       : std::vector<std::string>{"Undo", "Mode"};
            auto sel = co_await ctx.getSelection("Select an annotation object", so);
            if (sel.status == cmd::Status::Keyword) {
                if (sel.keyword == "Undo" && !done.empty()) {
                    const Done last = done.back();
                    done.pop_back();
                    (void)ctx.tx().modify(last.id,
                                          [&](Entity& en) { setContent(en, last.before); });
                } else if (sel.keyword == "Mode") {
                    cmd::KeywordOptions ko{.keywords = {"Single", "Multiple"},
                                           .defaultKeyword = multiple ? "Multiple" : "Single"};
                    auto m = co_await ctx.getKeyword("Enter a text edit mode", ko);
                    if (m.ok()) {
                        multiple = m.keyword == "Multiple";
                        ctx.host().setSetting("textedit.mode", multiple ? "0" : "1");
                    }
                }
                continue;
            }
            if (!sel.ok() || sel.ids.empty())
                co_return;
            const bool preselected = first && !ctx.usedPreselection().empty();
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
            if (const auto* m = entityAs<MTextData>(*e))
                o.editor = cmd::TextEditorHint{m->position, m->width, 0};
            else if (const auto* t = entityAs<TextData>(*e))
                o.editor =
                    cmd::TextEditorHint{{t->position.x, t->position.y + t->height}, 0, 0, false};
            auto r = co_await ctx.getString("Enter new text", o);
            if (!r.ok())
                co_return;
            const std::string value = r.text;
            auto res = ctx.tx().modify(sel.ids.front(), [&](Entity& en) { setContent(en, value); });
            if (!res)
                ctx.message(res.error().message);
            else
                done.push_back({sel.ids.front(), current});
            if (!multiple || preselected)
                co_return;
        }
    }

private:
    static void setContent(Entity& en, const std::string& value) {
        if (auto* t = entityAs<TextData>(en))
            t->utf8 = value;
        else if (auto* m = entityAs<MTextData>(en))
            m->utf8 = value;
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
