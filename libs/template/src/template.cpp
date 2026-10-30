#include <algorithm>
#include <cctype>
#include <system_error>

#include "tbcad/io/tbcad.h"
#include "tbcad/layout/layout.h"
#include "tbcad/tmpl/tmpl.h"

namespace tbcad::tmpl {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

LinetypeId addLinetype(Transaction& tx, const char* name, const char* description,
                       std::vector<double> pattern) {
    Linetype lt;
    lt.name = name;
    lt.description = description;
    lt.pattern = std::move(pattern);
    auto id = tx.addLinetype(std::move(lt));
    return id ? id.value() : kLinetypeContinuous;
}

LayerId addLayer(Transaction& tx, const char* name, int color, LinetypeId linetype, bool plot,
                 LineWeight weight = LineWeight::defaultWeight()) {
    Layer l;
    l.name = name;
    l.color = Color::index(static_cast<std::uint8_t>(color));
    l.linetype = linetype;
    l.plot = plot;
    l.lineweight = weight;
    auto id = tx.addLayer(std::move(l));
    return id ? id.value() : LayerId{};
}

}  // namespace

std::unique_ptr<Document> builtinTemplate(LengthUnit units) {
    const bool inch = units == LengthUnit::Inch;
    auto doc = std::make_unique<Document>();
    Transaction tx = doc->begin("Template");

    // Pattern lengths: + dash, - gap, 0 dot. Millimetres, or the inch values of acad.lin.
    const double k = inch ? 1.0 / 24.0 : 1.0;  // 12 mm dash = 0.5 in
    const LinetypeId dashed = addLinetype(tx, "Dashed", "Dashed", {12 * k, -6 * k});
    const LinetypeId hidden = addLinetype(tx, "Hidden", "Hidden", {6 * k, -3 * k});
    const LinetypeId center =
        addLinetype(tx, "Center", "Center", {inch ? 1.25 : 24, -6 * k, 6 * k, -6 * k});
    (void)addLinetype(tx, "Phantom", "Phantom",
                      {inch ? 1.25 : 24, -6 * k, 6 * k, -6 * k, 6 * k, -6 * k});
    (void)addLinetype(tx, "Dot", "Dot", {0, -6 * k});
    (void)addLinetype(tx, "DashDot", "Dash dot", {12 * k, -6 * k, 0, -6 * k});

    const LinetypeId cont = kLinetypeContinuous;
    addLayer(tx, "Defpoints", 7, cont, false);
    addLayer(tx, "Walls", 7, cont, true, LineWeight::mm(0.5));
    addLayer(tx, "Doors-Windows", 4, cont, true);
    addLayer(tx, "Furniture", 8, cont, true);
    const LayerId dimLayer = addLayer(tx, "Dimensions", 3, cont, true);
    const LayerId textLayer = addLayer(tx, "Text", 2, cont, true);
    const LayerId hatchLayer = addLayer(tx, "Hatch", 9, cont, true);
    addLayer(tx, "Centerlines", 1, center, true);
    addLayer(tx, "Hidden", 6, hidden, true);
    addLayer(tx, "Grid", 5, dashed, true);
    addLayer(tx, "Construction", 8, dashed, false);
    addLayer(tx, "Title-Block", 7, cont, true, LineWeight::mm(0.35));
    addLayer(tx, "Viewports", 7, cont, false);

    TextStyle notes;
    notes.name = "Notes";
    (void)tx.addTextStyle(std::move(notes));

    if (inch) {
        // AutoCAD's imperial Standard: 0.18" text and arrows, four decimals.
        (void)tx.modifyDimStyle(kDimStyleStandard, [](DimStyle& d) {
            d.textHeight = 0.18;
            d.textGap = 0.09;
            d.arrowSize = 0.18;
            d.extLineOffset = 0.0625;
            d.extLineExtension = 0.18;
            d.baselineSpacing = 0.38;
            d.centerMark = 0.09;
            d.precision = 4;
            d.textPlacement = DimTextPlacement::AboveLine;
        });
    } else {
        // Millimetre drawings: whole numbers ("8000"), text lifted above the dimension line.
        (void)tx.modifyDimStyle(kDimStyleStandard, [](DimStyle& d) {
            d.precision = 0;
            d.textPlacement = DimTextPlacement::AboveLine;
        });
    }

    // A second dimension style with architectural ticks; Standard keeps filled arrows.
    DimStyle arch = *doc->dimStyle(kDimStyleStandard);
    arch.id = {};
    arch.name = "Architectural";
    arch.arrowType = ArrowType::ArchTick;
    arch.precision = inch ? 2 : 0;
    (void)tx.addDimStyle(std::move(arch));

    (void)tx.modifyHeader([&](Header& h) {
        h.units = inch ? LengthUnit::Inch : LengthUnit::Millimeter;
        h.currentLayer = kLayerZero;
        // Dimensions, hatches and text land on their own layers, as DIMLAYER, HPLAYER and
        // TEXTLAYER set them, while layer 0 is current.
        h.custom[kDimLayerKey] = std::to_string(dimLayer.value);
        h.custom[kHatchLayerKey] = std::to_string(hatchLayer.value);
        h.custom[kTextLayerKey] = std::to_string(textLayer.value);
        h.custom[kCommandLayersAutoKey] = "1";
    });
    // Two empty sheets, as in AutoCAD's templates.
    for (const char* name : {"Layout1", "Layout2"})
        (void)layout::createLayout(tx, name, inch ? "ANSI B" : "A3", true);
    (void)tx.commit();
    return doc;
}

std::unique_ptr<Document> builtinTemplate(std::string_view name) {
    if (name == kBuiltinTemplate)
        return builtinTemplate(LengthUnit::Millimeter);
    if (name == kBuiltinInchTemplate)
        return builtinTemplate(LengthUnit::Inch);
    return nullptr;
}

bool isBuiltinTemplate(std::string_view name) {
    return name == kBuiltinTemplate || name == kBuiltinInchTemplate;
}

std::vector<TemplateInfo> listTemplates(const fs::path& dir) {
    std::vector<TemplateInfo> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return out;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || !isTemplatePath(it->path()))
            continue;
        out.push_back({it->path().stem().string(), it->path()});
    }
    std::sort(out.begin(), out.end(), [](const TemplateInfo& a, const TemplateInfo& b) {
        return lower(a.name) < lower(b.name);
    });
    return out;
}

Result<std::unique_ptr<Document>> loadTemplate(const fs::path& path) {
    io::IoReport report;
    return io::readTbcad(path, report);
}

Result<fs::path> saveTemplate(const Document& doc, fs::path path) {
    if (!isTemplatePath(path))
        path += kTemplateExtension;
    std::error_code ec;
    if (path.has_parent_path())
        fs::create_directories(path.parent_path(), ec);
    io::WriteOptions options;
    options.keepBackup = false;
    auto r = io::writeTbcad(doc, path, options);
    if (!r)
        return r.error();
    return path;
}

bool isTemplatePath(const fs::path& path) {
    return lower(path.extension().string()) == kTemplateExtension;
}

std::string sanitizeTemplateName(const std::string& name) {
    std::string out;
    for (unsigned char c : name) {
        if (c >= 0x80 || std::isalnum(c) || c == ' ' || c == '-' || c == '_')
            out.push_back(static_cast<char>(c));
        else
            out.push_back('_');
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    while (!out.empty() && out.front() == ' ')
        out.erase(out.begin());
    return out.empty() ? "template" : out;
}

}  // namespace tbcad::tmpl
