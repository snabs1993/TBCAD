#include <cmath>
#include <cstdlib>
#include <string>

#include "commands.h"
#include "tbcad/cmd/cmd.h"
#include "tbcad/text/text.h"

namespace tbcad::textcmd {

namespace {

using cmd::CommandContext;
using cmd::Status;
using cmd::Task;

bool looksLikeFontFile(const std::string& s) {
    auto ends = [&](const char* ext) {
        std::size_t n = std::char_traits<char>::length(ext);
        if (s.size() < n)
            return false;
        for (std::size_t i = 0; i < n; ++i)
            if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != ext[i])
                return false;
        return true;
    };
    return ends(".ttf") || ends(".otf") || ends(".ttc") || ends(".tbsf");
}

// STYLE: create or change a text style and make it current.
class StyleCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "STYLE"; }
    std::vector<std::string> defaultAliases() const override { return {"ST"}; }
    std::string_view description() const override { return "Create or change a text style"; }

    Task run(CommandContext& ctx) override {
        const Document& doc = ctx.doc();
        const TextStyle* cur = doc.textStyle(doc.header().currentTextStyle);
        cmd::StringOptions no;
        no.defaultValue = cur ? cur->name : std::string("Standard");
        auto n = co_await ctx.getString("Enter name of text style or [?]", no);
        if (!n.ok())
            co_return;
        if (n.text == "?") {
            for (const auto& [id, s] : doc.textStyles()) {
                std::string line = (id == doc.header().currentTextStyle ? "* " : "  ") + s.name +
                                   "  font: " +
                                   (!s.fontFile.empty()     ? s.fontFile
                                    : !s.fontFamily.empty() ? s.fontFamily
                                                            : std::string("Sarabun (default)")) +
                                   "  width " + std::to_string(s.widthFactor) +
                                   (s.height > 0 ? "  height " + std::to_string(s.height) : "");
                ctx.message(line);
            }
            co_return;
        }
        const TextStyle* existing = doc.textStyleByName(n.text);
        TextStyle row = existing ? *existing : TextStyle{};
        row.name = existing ? existing->name : n.text;

        cmd::StringOptions fo;
        fo.defaultValue = !row.fontFile.empty()     ? row.fontFile
                          : !row.fontFamily.empty() ? row.fontFamily
                                                    : std::string("Sarabun");
        auto f = co_await ctx.getString("Specify font family or font file", fo);
        if (!f.ok())
            co_return;
        row.fontFile.clear();
        row.fontFamily.clear();
        if (looksLikeFontFile(f.text))
            row.fontFile = f.text;
        else if (f.text != "Sarabun")
            row.fontFamily = f.text;
        if (!row.fontFile.empty() || !row.fontFamily.empty()) {
            if (!text::fontAvailable(row.fontFamily, row.fontFile))
                ctx.message("Font not found; the bundled Sarabun is used instead: " + f.text);
        }

        cmd::RealOptions wo;
        wo.defaultValue = row.widthFactor;
        wo.min = 0.01;
        wo.max = 100;
        auto w = co_await ctx.getReal("Specify width factor", wo);
        if (!w.ok())
            co_return;
        row.widthFactor = w.value;

        cmd::AngleOptions oo;
        oo.defaultValue = row.oblique;
        auto o = co_await ctx.getAngle("Specify oblique angle", oo);
        if (!o.ok())
            co_return;
        row.oblique = o.value;

        cmd::DistanceOptions ho;
        ho.defaultValue = row.height;
        auto ht = co_await ctx.getDistance(
            "Specify fixed height, 0 = variable <" + std::to_string(row.height) + ">", ho);
        if (!ht.ok())
            co_return;
        row.height = ht.value;

        TextStyleId id{};
        if (existing) {
            id = existing->id;
            auto r = ctx.tx().modifyTextStyle(id, [&](TextStyle& s) {
                s.fontFile = row.fontFile;
                s.fontFamily = row.fontFamily;
                s.widthFactor = row.widthFactor;
                s.oblique = row.oblique;
                s.height = row.height;
            });
            if (!r) {
                ctx.message(r.error().message);
                co_return;
            }
        } else {
            auto r = ctx.tx().addTextStyle(row);
            if (!r) {
                ctx.message(r.error().message);
                co_return;
            }
            id = r.value();
        }
        auto h = ctx.tx().modifyHeader([&](Header& hd) { hd.currentTextStyle = id; });
        if (!h)
            ctx.message(h.error().message);
        else
            ctx.message("Current text style: " + row.name);
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeStyleCommand() {
    return std::make_unique<StyleCommand>();
}

}  // namespace tbcad::textcmd
