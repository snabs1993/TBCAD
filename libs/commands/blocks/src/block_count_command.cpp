#include <algorithm>
#include <filesystem>
#include <fstream>

#include "commands.h"
#include "tbcad/block_cmds/palette.h"

namespace tbcad::block_cmds {

namespace {

Entity textAt(Vec2 at, const std::string& s, double height) {
    TextData t;
    t.position = at;
    t.alignPoint = at;
    t.height = height;
    t.utf8 = s;
    Entity e;
    e.data = Indirect<TextData>(std::move(t));
    return e;
}

Entity lineEntity(Vec2 a, Vec2 b) {
    Entity e;
    e.data = LineData{a, b};
    return e;
}

// BLOCKCOUNT [BCOUNT]: how many times each block is used, as a list, a CSV file or a table.
class BlockCountCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "BLOCKCOUNT"; }
    std::vector<std::string> defaultAliases() const override { return {"BCOUNT", "COUNT"}; }
    std::string_view description() const override { return "Count the inserts of each block"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        const std::vector<BlockCountRow> rows = blockCount(doc);
        if (rows.empty()) {
            ctx.message("No blocks are inserted in this drawing.");
            co_return;
        }
        std::size_t sumDirect = 0;
        for (const BlockCountRow& r : rows) {
            ctx.message(r.name + ": " + std::to_string(r.direct) + " placed, " +
                        std::to_string(r.total) + " in all");
            sumDirect += r.direct;
        }
        ctx.message(std::to_string(rows.size()) + " blocks, " + std::to_string(sumDirect) +
                    " inserts in model space");

        auto out = co_await ctx.getKeyword(
            "Output [Table/Csv/None] <None>",
            cmd::KeywordOptions{.keywords = {"Table", "Csv", "None"}, .defaultKeyword = "None"});
        if (!out.ok() || out.keyword == "None")
            co_return;

        if (out.keyword == "Csv") {
            auto f = co_await ctx.getString("Enter the CSV file name <blockcount.csv>",
                                            cmd::StringOptions{.defaultValue = "blockcount.csv"});
            if (!f.ok())
                co_return;
            std::string file = cmd::trim(f.text);
            std::ofstream o(std::filesystem::path(std::u8string(file.begin(), file.end())),
                            std::ios::binary);
            if (!o) {
                ctx.message("Cannot write " + file);
                co_return;
            }
            o << blockCountCsv(rows);
            ctx.message("Wrote " + file);
            co_return;
        }

        auto p = co_await ctx.getPoint("Specify the top left corner of the table");
        if (!p.ok())
            co_return;
        // Text height and column widths follow the longest name; 0.6 of the height per character.
        constexpr double h = 3.0, pitch = 5.0, pad = 2.0;
        std::size_t longest = std::string("Block").size();
        for (const BlockCountRow& r : rows)
            longest = std::max(longest, r.name.size());
        const double c0 = static_cast<double>(longest) * h * 0.6 + 2 * pad;
        const double cw = 8 * h * 0.6 + 2 * pad;
        const double width = c0 + 2 * cw;
        const Vec2 top = p.point;
        const double bottom = top.y - pitch * static_cast<double>(rows.size() + 1);
        Transaction& tx = ctx.tx();
        std::vector<EntityId> made;
        auto put = [&](Entity e) {
            if (auto r = tx.add(std::move(e)))
                made.push_back(r.value());
        };
        auto cell = [&](double col, int row, const std::string& s) {
            put(textAt({top.x + col + pad, top.y - pitch * (row + 1) + (pitch - h) / 2}, s, h));
        };
        cell(0, 0, "Block");
        cell(c0, 0, "Placed");
        cell(c0 + cw, 0, "In all");
        int row = 1;
        for (const BlockCountRow& r : rows) {
            cell(0, row, r.name);
            cell(c0, row, std::to_string(r.direct));
            cell(c0 + cw, row, std::to_string(r.total));
            ++row;
        }
        for (std::size_t i = 0; i <= rows.size() + 1; ++i) {
            double y = top.y - pitch * static_cast<double>(i);
            put(lineEntity({top.x, y}, {top.x + width, y}));
        }
        for (double x : {0.0, c0, c0 + cw, width})
            put(lineEntity({top.x + x, top.y}, {top.x + x, bottom}));
        ctx.host().setSelection(made);
        ctx.setUndoLabel("Block count table");
        ctx.message("Table made with " + std::to_string(rows.size()) + " rows");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeBlockCountCommand() {
    return std::make_unique<BlockCountCommand>();
}

}  // namespace tbcad::block_cmds
