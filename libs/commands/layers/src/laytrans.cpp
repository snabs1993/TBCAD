// LAYTRANS: brings the layers of a drawing in line with an office standard. A CSV file lists
// "old,new[,colour[,linetype[,lineweight]]]" per line; the old layer is renamed to the new name, or
// merged into it when the new layer exists already. Export writes the current layers in the same
// form to start from.
#include <filesystem>
#include <fstream>
#include <sstream>

#include "commands.h"
#include "tbcad/layers/layer_ops.h"

namespace tbcad::layers {

namespace {

std::filesystem::path pathOf(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

std::vector<std::string> splitFields(const std::string& line) {
    const char sep = line.find(',') != std::string::npos ? ',' : ';';
    std::vector<std::string> out;
    std::string field;
    bool quoted = false;
    for (char c : line) {
        if (c == '"') {
            quoted = !quoted;
        } else if (c == sep && !quoted) {
            out.push_back(cmd::trim(field));
            field.clear();
        } else if (c != '\r') {
            field += c;
        }
    }
    out.push_back(cmd::trim(field));
    return out;
}

class LayTransCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "LAYTRANS"; }
    std::string_view description() const override {
        return "Rename or merge layers to a standard from a CSV file";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::KeywordOptions ko;
        ko.keywords = {"Load", "Export"};
        ko.defaultKeyword = "Load";
        auto k =
            co_await ctx.getKeyword("Layer translator [Load a standard/Export layers] <Load>", ko);
        if (!k.ok())
            co_return;
        if (k.keyword == "Export") {
            auto f = co_await ctx.getString("Enter the CSV file name <layers.csv>",
                                            cmd::StringOptions{.defaultValue = "layers.csv"});
            if (!f.ok())
                co_return;
            const std::string file = cmd::trim(f.text);
            std::ofstream out(pathOf(file), std::ios::binary);
            if (!out) {
                ctx.message("Cannot write " + file);
                co_return;
            }
            out << "\xEF\xBB\xBF" << "Old,New,Color,Linetype,Lineweight\r\n";
            std::size_t n = 0;
            for (LayerId id : layerTreeOrder(ctx.doc())) {
                const Layer* l = ctx.doc().layer(id);
                if (!l || id == kLayerZero)
                    continue;
                const std::string path = layerPath(ctx.doc(), id);
                out << path << "," << path << "," << colorName(l->color) << ","
                    << linetypeName(ctx.doc(), l->linetype) << "," << lineweightName(l->lineweight)
                    << "\r\n";
                ++n;
            }
            ctx.message("Wrote " + std::to_string(n) + " layers to " + file +
                        ". Change the New column (and colour, linetype, lineweight) and load it.");
            co_return;
        }

        auto f = co_await ctx.getString("Enter the CSV file name of the layer standard");
        if (!f.ok())
            co_return;
        const std::string file = cmd::trim(f.text);
        std::ifstream in(pathOf(file), std::ios::binary);
        if (!in) {
            ctx.message("Cannot read " + file);
            co_return;
        }
        ctx.setUndoLabel("LAYTRANS");
        std::string line;
        std::size_t renamed = 0, merged = 0, styled = 0, skipped = 0;
        bool first = true;
        while (std::getline(in, line)) {
            if (first && line.compare(0, 3, "\xEF\xBB\xBF") == 0)
                line.erase(0, 3);
            first = false;
            if (cmd::trim(line).empty())
                continue;
            const auto f2 = splitFields(line);
            if (f2.size() < 2 || f2[0].empty() || f2[1].empty() || f2[0] == "Old")
                continue;
            auto from = resolveLayer(ctx.doc(), f2[0]);
            if (!from) {
                ++skipped;  // a standard may list layers this drawing does not have
                continue;
            }
            LayerId target = from.value();
            if (f2[0] != f2[1]) {
                if (auto existing = resolveLayer(ctx.doc(), f2[1]); existing) {
                    if (existing.value() == from.value())
                        continue;
                    if (auto r = mergeLayer(ctx.doc(), ctx.tx(), from.value(), existing.value());
                        !r) {
                        ctx.message(f2[0] + ": " + r.error().message);
                        ++skipped;
                        continue;
                    }
                    target = existing.value();
                    ++merged;
                } else if (auto r = renameLayer(ctx.doc(), ctx.tx(), from.value(), f2[1]); !r) {
                    ctx.message(f2[0] + ": " + r.error().message);
                    ++skipped;
                    continue;
                } else {
                    ++renamed;
                }
            }
            bool style = false;
            if (f2.size() > 2 && !f2[2].empty())
                if (auto c = parseColor(f2[2]); c)
                    style = bool(setLayerColor(ctx.doc(), ctx.tx(), target, c.value())) || style;
            if (f2.size() > 3 && !f2[3].empty())
                if (auto lt = resolveLinetype(ctx.doc(), ctx.tx(), f2[3]); lt)
                    style =
                        bool(setLayerLinetype(ctx.doc(), ctx.tx(), target, lt.value())) || style;
            if (f2.size() > 4 && !f2[4].empty())
                if (auto w = parseLineweight(f2[4]); w)
                    style =
                        bool(setLayerLineweight(ctx.doc(), ctx.tx(), target, w.value())) || style;
            if (style)
                ++styled;
        }
        ctx.message(std::to_string(renamed) + " renamed, " + std::to_string(merged) + " merged, " +
                    std::to_string(styled) + " restyled, " + std::to_string(skipped) + " skipped.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeLayTransCommand() {
    return std::make_unique<LayTransCommand>();
}

}  // namespace tbcad::layers
