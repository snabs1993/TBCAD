// DATALINK and DATALINKUPDATE: a table whose cells come from a CSV or XLSX file and are read again
// on demand.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "tbcad/geom/geom.h"
#include "tbcad/io/zip.h"
#include "tbcad/table/table.h"
#include "tbcad/tablecmd/tablecmd.h"
#include "tbcad/text/text.h"

namespace tbcad::tablecmd {

namespace {

std::filesystem::path pathOf(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string unescapeXml(std::string_view s) {
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            o += s[i];
            continue;
        }
        const std::size_t semi = s.find(';', i);
        if (semi == std::string_view::npos || semi - i > 10) {
            o += s[i];
            continue;
        }
        const std::string_view ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp")
            o += '&';
        else if (ent == "lt")
            o += '<';
        else if (ent == "gt")
            o += '>';
        else if (ent == "quot")
            o += '"';
        else if (ent == "apos")
            o += '\'';
        else if (ent.size() > 1 && ent[0] == '#') {
            const bool hex = ent[1] == 'x' || ent[1] == 'X';
            unsigned long cp = 0;
            try {
                cp = std::stoul(std::string(ent.substr(hex ? 2 : 1)), nullptr, hex ? 16 : 10);
            } catch (...) {
                cp = 0x3F;
            }
            if (cp < 0x80)
                o += static_cast<char>(cp);
            else if (cp < 0x800) {
                o += static_cast<char>(0xC0 | (cp >> 6));
                o += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                o += static_cast<char>(0xE0 | (cp >> 12));
                o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                o += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                o += static_cast<char>(0xF0 | ((cp >> 18) & 0x07));
                o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                o += static_cast<char>(0x80 | (cp & 0x3F));
            }
        } else {
            o += s[i];
            continue;
        }
        i = semi;
    }
    return o;
}

// The elements `<name ...>...</name>` (or `<name .../>`) of `xml`, in order, as {attributes,
// inner}.
struct Element {
    std::string_view attrs, inner;
};
std::vector<Element> elements(std::string_view xml, std::string_view name) {
    std::vector<Element> out;
    const std::string open = "<" + std::string(name);
    const std::string close = "</" + std::string(name) + ">";
    std::size_t pos = 0;
    while ((pos = xml.find(open, pos)) != std::string_view::npos) {
        const std::size_t after = pos + open.size();
        if (after >= xml.size() || !(xml[after] == ' ' || xml[after] == '>' || xml[after] == '/')) {
            pos = after;
            continue;
        }
        const std::size_t gt = xml.find('>', after);
        if (gt == std::string_view::npos)
            break;
        Element e;
        const bool selfClosed = xml[gt - 1] == '/';
        e.attrs = xml.substr(after, gt - after - (selfClosed ? 1 : 0));
        if (selfClosed) {
            pos = gt + 1;
        } else {
            const std::size_t end = xml.find(close, gt);
            if (end == std::string_view::npos)
                break;
            e.inner = xml.substr(gt + 1, end - gt - 1);
            pos = end + close.size();
        }
        out.push_back(e);
    }
    return out;
}

std::string attr(std::string_view attrs, std::string_view name) {
    const std::string key = " " + std::string(name) + "=\"";
    const std::size_t p = attrs.find(key);
    if (p == std::string_view::npos)
        return {};
    const std::size_t b = p + key.size();
    const std::size_t e = attrs.find('"', b);
    return e == std::string_view::npos ? std::string() : std::string(attrs.substr(b, e - b));
}

// All the <t> texts inside `inner`, joined (a rich-text string is several runs).
std::string textOf(std::string_view inner) {
    std::string o;
    for (const Element& t : elements(inner, "t"))
        o += unescapeXml(t.inner);
    return o;
}

// "AB12" -> column 27 (0-based); -1 when there is no column.
int columnOf(const std::string& ref) {
    int c = 0, n = 0;
    for (char ch : ref) {
        if (ch < 'A' || ch > 'Z')
            break;
        c = c * 26 + (ch - 'A' + 1);
        ++n;
    }
    return n ? c - 1 : -1;
}

std::vector<std::vector<std::string>> readXlsx(const std::vector<std::uint8_t>& bytes,
                                               std::string& error) {
    auto zip = io::readZip(bytes);
    if (!zip) {
        error = "Not a valid XLSX file";
        return {};
    }
    const std::string* shared = nullptr;
    std::string sheetName;
    std::string sharedStore;
    std::string sheet;
    for (const io::ZipEntry& e : zip.value()) {
        const std::string n = lower(e.name);
        if (n == "xl/sharedstrings.xml") {
            sharedStore.assign(e.data.begin(), e.data.end());
            shared = &sharedStore;
        } else if (n.rfind("xl/worksheets/", 0) == 0 && n.size() > 4 &&
                   n.compare(n.size() - 4, 4, ".xml") == 0) {
            if (sheetName.empty() || n < sheetName) {
                sheetName = n;
                sheet.assign(e.data.begin(), e.data.end());
            }
        }
    }
    if (sheetName.empty()) {
        error = "The XLSX file has no sheet";
        return {};
    }
    std::vector<std::string> strings;
    if (shared)
        for (const Element& si : elements(*shared, "si"))
            strings.push_back(textOf(si.inner));

    std::vector<std::vector<std::string>> rows;
    std::size_t nextRow = 0;
    for (const Element& row : elements(sheet, "row")) {
        std::size_t r = nextRow;
        if (const std::string rn = attr(row.attrs, "r"); !rn.empty()) {
            try {
                r = static_cast<std::size_t>(std::max(1, std::stoi(rn))) - 1;
            } catch (...) {
            }
        }
        r = std::min<std::size_t>(r, 100000);
        nextRow = r + 1;
        if (rows.size() <= r)
            rows.resize(r + 1);
        int nextCol = 0;
        for (const Element& c : elements(row.inner, "c")) {
            int col = columnOf(attr(c.attrs, "r"));
            if (col < 0)
                col = nextCol;
            col = std::min(col, 999);
            nextCol = col + 1;
            const std::string type = attr(c.attrs, "t");
            std::string value;
            if (type == "inlineStr") {
                value = textOf(c.inner);
            } else {
                const auto v = elements(c.inner, "v");
                if (v.empty())
                    continue;
                value = unescapeXml(v[0].inner);
                if (type == "s") {
                    std::size_t idx = 0;
                    try {
                        idx = static_cast<std::size_t>(std::stoul(value));
                    } catch (...) {
                        idx = strings.size();
                    }
                    value = idx < strings.size() ? strings[idx] : std::string();
                } else if (type == "b") {
                    value = value == "1" ? "TRUE" : "FALSE";
                }
            }
            auto& line = rows[r];
            if (line.size() <= static_cast<std::size_t>(col))
                line.resize(static_cast<std::size_t>(col) + 1);
            line[static_cast<std::size_t>(col)] = std::move(value);
        }
    }
    // Drop the empty rows at the end, which a sheet with formatting often has.
    while (!rows.empty() && std::all_of(rows.back().begin(), rows.back().end(),
                                        [](const std::string& s) { return s.empty(); }))
        rows.pop_back();
    return rows;
}

// Rebuilds the cells and the grid of `t` from `rows`, keeping its place, rotation, text height,
// text style and cell styles. A header row, if any, stays one.
void applyRows(TableData& t, const std::vector<std::vector<std::string>>& rows,
               const TextStyle* style) {
    TableData fresh = table::fromRows(rows, t.origin, t.textHeight, style);
    fresh.rotation = t.rotation;
    fresh.margin = t.margin;
    fresh.style = t.style;
    fresh.titleRow = false;
    fresh.headerRow = t.headerRow;
    fresh.titleStyle = t.titleStyle;
    fresh.headerStyle = t.headerStyle;
    fresh.dataStyle = t.dataStyle;
    fresh.linkFile = t.linkFile;
    t = std::move(fresh);
}

class DataLinkCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DATALINK"; }
    std::string_view description() const override {
        return "Link a table to a CSV or XLSX file and fill it from the file";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::EntityOptions eo;
        eo.filter = [](const Entity& e) { return kind(e) == EntityKind::Table; };
        auto o = co_await ctx.getEntity("Select table to link", eo);
        if (!o.ok())
            co_return;
        auto f = co_await ctx.getString("Enter the CSV or XLSX file name");
        if (!f.ok())
            co_return;
        const std::string file = cmd::trim(f.text);
        std::string error;
        const auto rows = readLinkRows(file, error);
        if (rows.empty()) {
            ctx.message(error.empty() ? "The file has no rows." : error);
            co_return;
        }
        const TextStyle* style = ctx.doc().textStyle(ctx.doc().header().currentTextStyle);
        ctx.setUndoLabel("DATALINK");
        auto ok = ctx.tx().modify(o.id, [&](Entity& e) {
            if (auto* t = entityAs<TableData>(e)) {
                t->linkFile = file;
                applyRows(*t, rows, style);
            }
        });
        if (!ok) {
            ctx.message(ok.error().message);
            co_return;
        }
        ctx.message("Table linked to " + file + " (" + std::to_string(rows.size()) + " rows)");
    }
};

class DataLinkUpdateCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DATALINKUPDATE"; }
    std::string_view description() const override {
        return "Read the linked files again into every linked table";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        std::vector<EntityId> ids;
        ctx.doc().forEachEntity([&](const Entity& e) {
            if (const auto* t = entityAs<TableData>(e); t && !t->linkFile.empty())
                ids.push_back(e.common.id);
        });
        if (ids.empty()) {
            ctx.message("No table is linked to a file. Use DATALINK.");
            co_return;
        }
        const TextStyle* style = ctx.doc().textStyle(ctx.doc().header().currentTextStyle);
        ctx.setUndoLabel("DATALINKUPDATE");
        int updated = 0;
        for (EntityId id : ids) {
            const Entity* e = ctx.doc().find(id);
            const auto* t = e ? entityAs<TableData>(*e) : nullptr;
            if (!t)
                continue;
            const std::string file = t->linkFile;
            std::string error;
            const auto rows = readLinkRows(file, error);
            if (rows.empty()) {
                ctx.message((error.empty() ? "The file has no rows: " : error + ": ") + file);
                continue;
            }
            if (ctx.tx().modify(id, [&](Entity& m) {
                    if (auto* mt = entityAs<TableData>(m))
                        applyRows(*mt, rows, style);
                }))
                ++updated;
        }
        ctx.message(std::to_string(updated) + " of " + std::to_string(ids.size()) +
                    " linked tables updated");
    }
};

}  // namespace

std::vector<std::vector<std::string>> readLinkRows(const std::string& file, std::string& error) {
    error.clear();
    std::ifstream in(pathOf(file), std::ios::binary);
    if (!in) {
        error = "Cannot read " + file;
        return {};
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string data = buf.str();
    if (lower(pathOf(file).extension().string()) == ".xlsx")
        return readXlsx(std::vector<std::uint8_t>(data.begin(), data.end()), error);
    return table::parseCsv(data);
}

void registerDataLinkCommands(cmd::Registry& registry) {
    (void)registry.add(std::make_unique<DataLinkCommand>());
    (void)registry.add(std::make_unique<DataLinkUpdateCommand>());
}

}  // namespace tbcad::tablecmd
