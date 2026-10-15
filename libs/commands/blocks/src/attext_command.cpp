#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>

#include "commands.h"
#include "tbcad/blocks/tbcad_blocks.h"

namespace tbcad::block_cmds {

namespace {

std::string quote(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos)
        return s;
    std::string q = "\"";
    for (char c : s) {
        if (c == '"')
            q += '"';
        q += c;
    }
    return q + "\"";
}

std::string num(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.6f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0')
        s.pop_back();
    if (!s.empty() && s.back() == '.')
        s.pop_back();
    return s.empty() || s == "-0" ? "0" : s;
}

// One CSV row per insert of a block that has attributes (or of the named block): the block, layer,
// position, rotation and scale, then a column per attribute tag.
std::string attributeCsv(const Document& doc, const std::string& onlyBlock, std::size_t& rows) {
    struct Row {
        std::vector<std::string> fixed;
        std::map<std::string, std::string> values;
    };
    std::vector<Row> out;
    std::vector<std::string> tags;
    for (EntityId id : doc.modelSpace()) {
        const Entity* e = doc.find(id);
        const auto* ins = e ? entityAs<InsertData>(*e) : nullptr;
        const BlockDef* def = ins ? doc.block(ins->block) : nullptr;
        if (!def || def->anonymous)
            continue;
        if (!onlyBlock.empty() && def->name != onlyBlock)
            continue;
        Row row;
        for (const Entity* ad : blocks::attributeDefinitions(doc, def->id)) {
            const AttDefInfo& info = *entityAs<TextData>(*ad)->attdef;
            if (info.constant)
                row.values[info.tag] = info.defaultValue;
            if (std::find(tags.begin(), tags.end(), info.tag) == tags.end())
                tags.push_back(info.tag);
        }
        for (const AttribValue& v : ins->attribs)
            row.values[v.tag] = v.value;
        if (row.values.empty() && onlyBlock.empty())
            continue;  // a block without attributes has nothing to extract
        const Layer* layer = doc.layer(e->common.layer);
        row.fixed = {def->name,
                     layer ? layer->name : "0",
                     num(ins->position.x),
                     num(ins->position.y),
                     num(ins->rotation * 180.0 / kPi),
                     num(ins->scale.x),
                     num(ins->scale.y)};
        out.push_back(std::move(row));
    }
    std::string csv = "Block,Layer,X,Y,Rotation,ScaleX,ScaleY";
    for (const std::string& t : tags)
        csv += "," + quote(t);
    csv += "\r\n";
    for (const Row& r : out) {
        for (std::size_t i = 0; i < r.fixed.size(); ++i)
            csv += (i ? "," : "") + quote(r.fixed[i]);
        for (const std::string& t : tags) {
            auto it = r.values.find(t);
            csv += "," + (it == r.values.end() ? std::string() : quote(it->second));
        }
        csv += "\r\n";
    }
    rows = out.size();
    return csv;
}

// ATTEXT [DATAEXTRACTION, DX]: writes the attribute values of the block inserts to a CSV file
// that Excel opens, one row per insert.
class AttExtCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ATTEXT"; }
    std::vector<std::string> defaultAliases() const override { return {"DX"}; }
    std::string_view description() const override {
        return "Write the attribute values of block inserts to a CSV file";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        std::string only;
        auto b = co_await ctx.getString("Block name or Enter for every block with attributes",
                                        cmd::StringOptions{.allowEmpty = true});
        if (!b.ok() && b.status != cmd::Status::None)
            co_return;
        only = cmd::trim(b.text);
        if (!only.empty() && !doc.blockByName(only)) {
            ctx.message("No block named \"" + only + "\".");
            co_return;
        }
        std::size_t rows = 0;
        const std::string csv = attributeCsv(doc, only, rows);
        if (rows == 0) {
            ctx.message("No inserts with attributes were found.");
            co_return;
        }
        auto f = co_await ctx.getString("Enter the CSV file name <attributes.csv>",
                                        cmd::StringOptions{.defaultValue = "attributes.csv"});
        if (!f.ok())
            co_return;
        const std::string file = cmd::trim(f.text);
        std::ofstream o(std::filesystem::path(std::u8string(file.begin(), file.end())),
                        std::ios::binary);
        if (!o) {
            ctx.message("Cannot write " + file);
            co_return;
        }
        o << "\xEF\xBB\xBF" << csv;  // a BOM so Excel reads UTF-8 text correctly
        ctx.message("Wrote " + std::to_string(rows) + " rows to " + file);
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeAttExtCommand() {
    return std::make_unique<AttExtCommand>();
}

}  // namespace tbcad::block_cmds
