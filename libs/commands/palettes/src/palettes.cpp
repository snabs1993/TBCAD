#include "tbcad/palettes/palettes.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "tbcad/platform/json.h"

namespace tbcad::palettes {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trimmed(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return s.substr(a, b - a);
}

std::string number(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.10g", v);
    return buf;
}

const char* kindText(ToolKind k) {
    switch (k) {
        case ToolKind::Block:
            return "block";
        case ToolKind::Hatch:
            return "hatch";
        case ToolKind::Command:
            return "command";
    }
    return "command";
}

Tool commandTool(const char* name, const char* command) {
    Tool t;
    t.name = name;
    t.kind = ToolKind::Command;
    t.command = command;
    return t;
}

Tool hatchTool(const char* name, const char* pattern, double scale, double angle) {
    Tool t;
    t.name = name;
    t.kind = ToolKind::Hatch;
    t.pattern = pattern;
    t.scale = scale;
    t.angle = angle;
    return t;
}

}  // namespace

std::vector<Palette> defaultPalettes() {
    Palette draw;
    draw.name = "Draw";
    draw.tools = {commandTool("Line", "LINE"),           commandTool("Polyline", "POLYLINE"),
                  commandTool("Circle", "CIRCLE"),       commandTool("Arc", "ARC"),
                  commandTool("Rectangle", "RECTANGLE"), commandTool("Text", "TEXT")};
    Palette hatches;
    hatches.name = "Hatches";
    hatches.tools = {hatchTool("Steel (ANSI31)", "ANSI31", 1, 0),
                     hatchTool("Concrete", "AR-CONC", 1, 0), hatchTool("Brick", "AR-B816", 1, 0),
                     hatchTool("Solid", "SOLID", 1, 0)};
    return {std::move(draw), std::move(hatches)};
}

void PaletteStore::load() {
    warning_.clear();
    palettes_.clear();
    std::ifstream in(file_, std::ios::binary);
    if (!in) {
        palettes_ = defaultPalettes();
        return;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    auto j = platform::parseJson(ss.str());
    if (!j || !j.value().isObject()) {
        warning_ = "palettes.json could not be read; using the default palettes.";
        palettes_ = defaultPalettes();
        return;
    }
    const platform::Json* list = j.value().find("palettes");
    for (const platform::Json& pj : list ? list->asArray() : platform::Json::Array{}) {
        Palette p;
        if (const auto* n = pj.find("name"))
            p.name = n->asString();
        if (p.name.empty() || find(p.name))
            continue;
        if (const auto* tools = pj.find("tools")) {
            for (const platform::Json& tj : tools->asArray()) {
                Tool t;
                auto str = [&](const char* key) {
                    const auto* v = tj.find(key);
                    return v ? v->asString() : std::string();
                };
                auto num = [&](const char* key, double fallback) {
                    const auto* v = tj.find(key);
                    return v ? v->asNumber(fallback) : fallback;
                };
                t.name = str("name");
                const std::string kind = str("kind");
                t.kind = kind == "block"   ? ToolKind::Block
                         : kind == "hatch" ? ToolKind::Hatch
                                           : ToolKind::Command;
                t.layer = str("layer");
                t.block = str("block");
                t.file = str("file");
                t.pattern = str("pattern");
                t.command = str("command");
                t.scale = num("scale", 1);
                t.angle = num("angle", 0);
                if (!t.name.empty())
                    p.tools.push_back(std::move(t));
            }
        }
        palettes_.push_back(std::move(p));
    }
    if (palettes_.empty())
        palettes_ = defaultPalettes();
}

std::string PaletteStore::save() const {
    platform::Json::Array ps;
    for (const Palette& p : palettes_) {
        platform::Json::Array ts;
        for (const Tool& t : p.tools) {
            platform::Json::Object o;
            o["name"] = t.name;
            o["kind"] = kindText(t.kind);
            if (!t.layer.empty())
                o["layer"] = t.layer;
            if (!t.block.empty())
                o["block"] = t.block;
            if (!t.file.empty())
                o["file"] = t.file;
            if (!t.pattern.empty())
                o["pattern"] = t.pattern;
            if (!t.command.empty())
                o["command"] = t.command;
            o["scale"] = t.scale;
            o["angle"] = t.angle;
            ts.emplace_back(std::move(o));
        }
        platform::Json::Object po;
        po["name"] = p.name;
        po["tools"] = std::move(ts);
        ps.emplace_back(std::move(po));
    }
    platform::Json::Object root;
    root["palettes"] = std::move(ps);
    std::error_code ec;
    if (file_.has_parent_path())
        fs::create_directories(file_.parent_path(), ec);
    fs::path tmp = file_;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << platform::toJson(platform::Json(std::move(root)), true);
        if (!out)
            return "Cannot write " + tmp.string();
    }
    fs::rename(tmp, file_, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return "Cannot replace " + file_.string();
    }
    return {};
}

Palette* PaletteStore::find(const std::string& name) {
    for (Palette& p : palettes_)
        if (lower(p.name) == lower(trimmed(name)))
            return &p;
    return nullptr;
}

std::string PaletteStore::addPalette(const std::string& name) {
    const std::string n = trimmed(name);
    if (n.empty())
        return "Give the palette a name.";
    if (find(n))
        return "A palette named \"" + n + "\" already exists.";
    palettes_.push_back({n, {}});
    return {};
}

std::string PaletteStore::renamePalette(const std::string& from, const std::string& to) {
    Palette* p = find(from);
    if (!p)
        return "No palette named \"" + from + "\".";
    const std::string n = trimmed(to);
    if (n.empty())
        return "Give the palette a name.";
    if (Palette* other = find(n); other && other != p)
        return "A palette named \"" + n + "\" already exists.";
    p->name = n;
    return {};
}

std::string PaletteStore::removePalette(const std::string& name) {
    Palette* p = find(name);
    if (!p)
        return "No palette named \"" + name + "\".";
    palettes_.erase(palettes_.begin() + (p - palettes_.data()));
    return {};
}

std::string PaletteStore::addTool(const std::string& palette, Tool tool) {
    Palette* p = find(palette);
    if (!p)
        return "No palette named \"" + palette + "\".";
    tool.name = trimmed(tool.name);
    if (tool.name.empty())
        return "Give the tool a name.";
    if (tool.kind == ToolKind::Block && trimmed(tool.block).empty())
        return "Choose the block.";
    if (tool.kind == ToolKind::Command && trimmed(tool.command).empty())
        return "Type the command.";
    if (tool.kind == ToolKind::Hatch && trimmed(tool.pattern).empty())
        return "Type the hatch pattern.";
    for (const Tool& t : p->tools)
        if (lower(t.name) == lower(tool.name))
            return "The palette already has a tool named \"" + tool.name + "\".";
    p->tools.push_back(std::move(tool));
    return {};
}

std::string PaletteStore::replaceTool(const std::string& palette, const std::string& tool,
                                      Tool with) {
    Palette* p = find(palette);
    if (!p)
        return "No palette named \"" + palette + "\".";
    auto it = std::find_if(p->tools.begin(), p->tools.end(),
                           [&](const Tool& t) { return lower(t.name) == lower(trimmed(tool)); });
    if (it == p->tools.end())
        return "No tool named \"" + tool + "\".";
    // Check the new values as a new tool of a palette without the old one.
    Palette rest{p->name, {}};
    for (const Tool& t : p->tools)
        if (&t != &*it)
            rest.tools.push_back(t);
    PaletteStore check({});
    check.palettes_ = {std::move(rest)};
    if (std::string err = check.addTool(palette, with); !err.empty())
        return err;
    *it = std::move(check.palettes_[0].tools.back());
    return {};
}

std::string PaletteStore::removeTool(const std::string& palette, const std::string& tool) {
    Palette* p = find(palette);
    if (!p)
        return "No palette named \"" + palette + "\".";
    for (auto it = p->tools.begin(); it != p->tools.end(); ++it) {
        if (lower(it->name) == lower(trimmed(tool))) {
            p->tools.erase(it);
            return {};
        }
    }
    return "No tool named \"" + tool + "\".";
}

std::string PaletteStore::moveTool(const std::string& palette, const std::string& tool, int delta) {
    Palette* p = find(palette);
    if (!p)
        return "No palette named \"" + palette + "\".";
    for (std::size_t i = 0; i < p->tools.size(); ++i) {
        if (lower(p->tools[i].name) != lower(trimmed(tool)))
            continue;
        const long target = std::clamp<long>(static_cast<long>(i) + delta, 0,
                                             static_cast<long>(p->tools.size()) - 1);
        Tool moving = std::move(p->tools[i]);
        p->tools.erase(p->tools.begin() + static_cast<long>(i));
        p->tools.insert(p->tools.begin() + target, std::move(moving));
        return {};
    }
    return "No tool named \"" + tool + "\".";
}

std::vector<std::string> toolLines(const Tool& tool, const Document& doc) {
    std::vector<std::string> out;
    if (!tool.layer.empty()) {
        // -LAYER Set <layer>, then an empty line ends the command.
        out.insert(out.end(), {"-LAYER", "Set", tool.layer, ""});
    }
    switch (tool.kind) {
        case ToolKind::Block: {
            if (!tool.file.empty() && !doc.blockByName(tool.block))
                out.insert(out.end(), {"CONTENTIMPORT", tool.file, "Block", tool.block});
            out.insert(out.end(), {"INSERT", tool.block});
            if (std::abs(tool.scale - 1) > 1e-9)
                out.insert(out.end(), {"Scale", number(tool.scale)});
            if (std::abs(tool.angle) > 1e-9)
                out.insert(out.end(), {"Rotate", number(tool.angle)});
            break;
        }
        case ToolKind::Hatch: {
            out.push_back("HATCH");
            if (!tool.pattern.empty())
                out.insert(out.end(), {"Pattern", tool.pattern});
            if (std::abs(tool.scale - 1) > 1e-9)
                out.insert(out.end(), {"Scale", number(tool.scale)});
            if (std::abs(tool.angle) > 1e-9)
                out.insert(out.end(), {"Angle", number(tool.angle)});
            break;
        }
        case ToolKind::Command: {
            std::string cur;
            for (char c : tool.command + "|") {
                if (c == '|') {
                    std::string t = trimmed(cur);
                    if (!t.empty())
                        out.push_back(std::move(t));
                    cur.clear();
                } else {
                    cur.push_back(c);
                }
            }
            break;
        }
    }
    return out;
}

namespace {

std::string layerPathOf(const Document& doc, LayerId id) {
    std::string out;
    for (const Layer* l = doc.layer(id); l; l = l->parent ? doc.layer(l->parent) : nullptr)
        out = out.empty() ? l->name : l->name + "~" + out;  // the -LAYER path separator
    return out;
}

const char* commandFor(const Entity& e) {
    switch (kind(e)) {
        case EntityKind::Line:
            return "LINE";
        case EntityKind::Circle:
            return "CIRCLE";
        case EntityKind::Arc:
            return "ARC";
        case EntityKind::Polyline:
            return "PLINE";
        case EntityKind::Point:
            return "POINT";
        case EntityKind::Ellipse:
            return "ELLIPSE";
        case EntityKind::Spline:
            return "SPLINE";
        case EntityKind::Text:
            return "TEXT";
        case EntityKind::MText:
            return "MTEXT";
        case EntityKind::Leader:
            return "QLEADER";
        case EntityKind::Table:
            return "TABLE";
        case EntityKind::Ray:
            return "XLINE";
        case EntityKind::MLine:
            return "MLINE";
        case EntityKind::MLeader:
            return "MLEADER";
        case EntityKind::Dimension:
            switch (entityAs<DimensionData>(e)->kind) {
                case DimKind::Aligned:
                    return "DIMALIGNED";
                case DimKind::Radius:
                    return "DIMRADIUS";
                case DimKind::Diameter:
                    return "DIMDIAMETER";
                case DimKind::Angular2Line:
                case DimKind::Angular3Point:
                    return "DIMANGULAR";
                case DimKind::ArcLength:
                    return "DIMARC";
                case DimKind::Ordinate:
                    return "DIMORDINATE";
                default:
                    return "DIMLINEAR";
            }
        default:
            return nullptr;
    }
}

}  // namespace

std::vector<Tool> toolsFromObjects(const Document& doc, const std::vector<EntityId>& ids,
                                   const std::string& drawingFile) {
    std::vector<Tool> out;
    auto addUnique = [&](Tool t) {
        for (const Tool& o : out)
            if (lower(o.name) == lower(t.name))
                return;
        out.push_back(std::move(t));
    };
    for (EntityId id : ids) {
        const Entity* e = doc.find(id);
        if (!e)
            continue;
        Tool t;
        t.layer = layerPathOf(doc, e->common.layer);
        if (const auto* ins = entityAs<InsertData>(*e)) {
            const BlockDef* def = doc.block(ins->block);
            if (!def || def->anonymous || !def->xrefPath.empty())
                continue;
            t.kind = ToolKind::Block;
            t.name = t.block = def->name;
            t.file = drawingFile;
            t.scale = ins->scale.x;
            t.angle = ins->rotation * 180.0 / 3.14159265358979323846;
        } else if (const auto* h = entityAs<HatchData>(*e)) {
            t.kind = ToolKind::Hatch;
            t.pattern = h->solid ? std::string("SOLID") : h->pattern;
            if (t.pattern.empty())
                continue;
            t.name = t.pattern;
            t.scale = h->solid ? 1 : h->patternScale;
            t.angle = h->solid ? 0 : h->patternAngle * 180.0 / 3.14159265358979323846;
        } else if (const char* c = commandFor(*e)) {
            t.kind = ToolKind::Command;
            t.name = t.command = c;
        } else {
            continue;
        }
        if (std::abs(t.angle) < 1e-9)
            t.angle = 0;
        addUnique(std::move(t));
    }
    return out;
}

}  // namespace tbcad::palettes
