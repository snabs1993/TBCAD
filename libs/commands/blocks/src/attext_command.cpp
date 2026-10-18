#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>

#include "commands.h"
#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/table/table.h"
#include "tbcad/text/annotation_size.h"
#include "tbcad/text/text.h"

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
    std::vector<std::string> defaultAliases() const override {
        return {"DX", "DATAEXTRACTION", "ATTOUT"};
    }
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

// ATTIN: reads a CSV written by ATTEXT and puts its attribute values back on the inserts it
// describes (matched by block name and position, in file order when several share a spot).
// Columns whose tag the insert does not have, constant attributes and empty cells stay as they are.
class AttInCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ATTIN"; }
    std::vector<std::string> defaultAliases() const override { return {"ATTIMPORT"}; }
    std::string_view description() const override {
        return "Read attribute values back from a CSV file written by ATTEXT";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        auto f = co_await ctx.getString("Enter the CSV file name <attributes.csv>",
                                        cmd::StringOptions{.defaultValue = "attributes.csv"});
        if (!f.ok())
            co_return;
        const std::string file = cmd::trim(f.text);
        std::ifstream in(std::filesystem::path(std::u8string(file.begin(), file.end())),
                         std::ios::binary);
        if (!in) {
            ctx.message("Cannot read " + file);
            co_return;
        }
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto rows = table::parseCsv(data);
        if (rows.size() < 2 || rows[0].size() < 7 || rows[0][0] != "Block" || rows[0][2] != "X") {
            ctx.message("This is not an ATTEXT file (the first line must start Block,Layer,X,Y).");
            co_return;
        }
        std::vector<EntityId> pending;
        for (EntityId id : doc.modelSpace())
            pending.push_back(id);
        std::size_t updated = 0, missing = 0;
        std::vector<std::pair<EntityId, std::vector<AttribValue>>> changes;
        for (std::size_t r = 1; r < rows.size(); ++r) {
            const auto& row = rows[r];
            if (row.size() < 7)
                continue;
            double x = 0, y = 0;
            try {
                x = std::stod(row[2]);
                y = std::stod(row[3]);
            } catch (...) {
                ++missing;
                continue;
            }
            EntityId hit{};
            for (auto it = pending.begin(); it != pending.end(); ++it) {
                const Entity* e = doc.find(*it);
                const auto* ins = e ? entityAs<InsertData>(*e) : nullptr;
                const BlockDef* def = ins ? doc.block(ins->block) : nullptr;
                if (!def || def->name != row[0] || std::abs(ins->position.x - x) > 1e-5 ||
                    std::abs(ins->position.y - y) > 1e-5)
                    continue;
                hit = *it;
                pending.erase(it);
                break;
            }
            if (!hit.value) {
                ++missing;
                continue;
            }
            std::vector<AttribValue> vals = entityAs<InsertData>(*doc.find(hit))->attribs;
            bool changed = false;
            for (std::size_t c = 7; c < rows[0].size() && c < row.size(); ++c) {
                if (row[c].empty())
                    continue;
                for (AttribValue& v : vals)
                    if (v.tag == rows[0][c] && v.value != row[c]) {
                        v.value = row[c];
                        changed = true;
                    }
            }
            if (changed)
                changes.emplace_back(hit, std::move(vals));
        }
        for (auto& [id, vals] : changes) {
            auto m = ctx.tx().modify(id, [&](Entity& e) {
                if (auto* ins = entityAs<InsertData>(e))
                    ins->attribs = std::move(vals);
            });
            if (m)
                ++updated;
        }
        if (updated)
            ctx.setUndoLabel("ATTIN");
        std::string msg = std::to_string(updated) + " inserts updated";
        if (missing)
            msg += ", " + std::to_string(missing) + " rows matched no insert";
        ctx.message(msg + ".");
    }
};

// ATTTABLE: the same extraction, but as a table in the drawing (a door or window schedule that
// updates by running it again): the block name and one column per attribute tag.
class AttTableCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ATTTABLE"; }
    std::string_view description() const override {
        return "Make a schedule table from the attribute values of block inserts";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        auto b = co_await ctx.getString("Block name or Enter for every block with attributes",
                                        cmd::StringOptions{.allowEmpty = true});
        if (!b.ok() && b.status != cmd::Status::None)
            co_return;
        const std::string only = cmd::trim(b.text);
        if (!only.empty() && !doc.blockByName(only)) {
            ctx.message("No block named \"" + only + "\".");
            co_return;
        }
        std::size_t count = 0;
        auto rows = table::parseCsv(attributeCsv(doc, only, count));
        if (count == 0 || rows.empty()) {
            ctx.message("No inserts with attributes were found.");
            co_return;
        }
        // Keep the block name, drop layer, position, rotation and scale (columns 1 to 6).
        for (auto& r : rows)
            if (r.size() > 7)
                r.erase(r.begin() + 1, r.begin() + 7);
            else if (r.size() > 1)
                r.erase(r.begin() + 1, r.end());
        constexpr double kBase = 2.5;
        const double textHeight = kBase * text::suggestedAnnotationScale(
                                              doc, ctx.host().view().scale,
                                              static_cast<bool>(ctx.host().drawingOwner()), kBase);
        const TextStyle* style = doc.textStyle(doc.header().currentTextStyle);
        auto where = co_await ctx.getPoint("Specify insertion point of the schedule (top-left)");
        if (!where.ok())
            co_return;
        TableData t = table::fromRows(rows, where.point, textHeight, style);
        t.style = doc.header().currentTextStyle;
        t.titleRow = false;
        Entity e;
        e.data = Indirect<TableData>(std::move(t));
        applyCurrentProperties(doc.header(), e);
        auto id = ctx.tx().add(std::move(e));
        if (!id) {
            ctx.message(id.error().message);
            co_return;
        }
        ctx.setUndoLabel("ATTTABLE");
        ctx.host().setSelection({id.value()});
        ctx.message("Schedule of " + std::to_string(count) + " inserts made.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeAttTableCommand() {
    return std::make_unique<AttTableCommand>();
}

std::unique_ptr<cmd::Command> makeAttInCommand() {
    return std::make_unique<AttInCommand>();
}

std::unique_ptr<cmd::Command> makeAttExtCommand() {
    return std::make_unique<AttExtCommand>();
}

}  // namespace tbcad::block_cmds
