#include "tbcad/sheets/sheets.h"

#include <fstream>
#include <map>
#include <memory>
#include <sstream>

#include "tbcad/io/tbcad.h"
#include "tbcad/layout/layout.h"

namespace tbcad::sheets {

namespace {

Error invalid(std::string msg) {
    return Error{ErrorCode::InvalidArgument, std::move(msg)};
}

std::string trim(const std::string& s) {
    const char* ws = " \t\r";
    auto a = s.find_first_not_of(ws);
    if (a == std::string::npos)
        return {};
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

}  // namespace

Result<std::vector<SheetEntry>> parseSheetList(const std::string& text,
                                               const std::filesystem::path& baseDir) {
    std::vector<SheetEntry> out;
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        SheetEntry e;
        std::string file = line;
        auto bar = line.find('|');
        if (bar != std::string::npos) {
            file = trim(line.substr(0, bar));
            std::string rest = line.substr(bar + 1);
            auto bar2 = rest.find('|');
            if (bar2 != std::string::npos) {
                e.pageSetup = trim(rest.substr(bar2 + 1));
                rest = rest.substr(0, bar2);
            }
            e.layout = trim(rest);
            if (e.layout == "Model" || e.layout == "*Model*")
                e.layout.clear();
        }
        if (file.empty())
            return invalid("Sheet list line " + std::to_string(lineNo) + ": no file name");
        e.file = std::filesystem::path(file);
        if (e.file.is_relative())
            e.file = baseDir / e.file;
        out.push_back(std::move(e));
    }
    if (out.empty())
        return invalid("The sheet list is empty");
    return out;
}

Result<std::vector<SheetEntry>> readSheetList(const std::filesystem::path& listFile) {
    std::ifstream in(listFile, std::ios::binary);
    if (!in)
        return Error{ErrorCode::Io, "Cannot read the sheet list " + listFile.string()};
    std::ostringstream buf;
    buf << in.rdbuf();
    return parseSheetList(buf.str(), listFile.parent_path());
}

std::string formatSheetList(const std::vector<SheetEntry>& entries,
                            const std::filesystem::path& baseDir) {
    std::string out = "# TBCAD sheet list: file | layout (empty = model) | page setup\n";
    for (const SheetEntry& e : entries) {
        std::filesystem::path file = e.file;
        if (!baseDir.empty()) {
            const std::filesystem::path rel = e.file.lexically_relative(baseDir);
            if (!rel.empty() && *rel.begin() != "..")
                file = rel;
        }
        const std::u8string u = file.generic_u8string();
        out += std::string(u.begin(), u.end());
        if (!e.layout.empty() || !e.pageSetup.empty())
            out += " | " + e.layout;
        if (!e.pageSetup.empty())
            out += " | " + e.pageSetup;
        out += "\n";
    }
    return out;
}

std::string writeSheetList(const std::vector<SheetEntry>& entries,
                           const std::filesystem::path& listFile) {
    std::ofstream out(listFile, std::ios::binary | std::ios::trunc);
    if (!out)
        return "Cannot write " + listFile.string();
    out << formatSheetList(entries, listFile.parent_path());
    return out ? std::string() : "Cannot write " + listFile.string();
}

std::vector<SheetEntry> sheetsOf(const Document& doc, const std::filesystem::path& file) {
    std::vector<SheetEntry> out{{file, {}, {}}};
    for (const layout::Layout& l : layout::layouts(doc))
        out.push_back({file, l.name, {}});
    return out;
}

Result<SheetSet> buildSheetList(const std::vector<SheetEntry>& entries,
                                const exportlib::PlotSettings& base,
                                const layout::StyleResolver& styles, const BuildOptions& options) {
    std::map<std::filesystem::path, std::unique_ptr<Document>> docs;
    SheetSet set;
    auto sameFile = [](const std::filesystem::path& a, const std::filesystem::path& b) {
        if (a.empty() || b.empty())
            return false;
        std::error_code ec;
        return a.lexically_normal() == b.lexically_normal() ||
               std::filesystem::equivalent(a, b, ec);
    };
    for (const SheetEntry& e : entries) {
        const std::string what =
            e.file.filename().string() + (e.layout.empty() ? "" : " / " + e.layout);
        const Document* doc = nullptr;
        if (options.current && sameFile(e.file, options.currentFile)) {
            doc = options.current;
        } else {
            auto it = docs.find(e.file);
            if (it == docs.end()) {
                Result<std::unique_ptr<Document>> loaded = Error{ErrorCode::Io, ""};
                if (options.load) {
                    loaded = options.load(e.file);
                } else {
                    io::IoReport ior;
                    loaded = io::readTbcad(e.file, ior);
                }
                if (!loaded)
                    return Error{loaded.error().code, what + ": " + loaded.error().message};
                it = docs.emplace(e.file, std::move(loaded.value())).first;
            }
            doc = it->second.get();
        }
        std::optional<layout::PageSetup> setup;
        if (!e.pageSetup.empty()) {
            setup = layout::findPageSetup(*doc, e.pageSetup);
            if (!setup && options.setupSource)
                setup = layout::findPageSetup(*options.setupSource, e.pageSetup);
            if (!setup)
                return invalid(what + ": no page setup named \"" + e.pageSetup + "\"");
        } else {
            setup = e.layout.empty() ? layout::pageSetupOfModel(*doc)
                                     : layout::pageSetupOfLayout(*doc, e.layout);
        }
        const exportlib::PlotSettings s = setup ? layout::settingsFor(base, *setup, styles) : base;
        auto sheet = e.layout.empty() ? exportlib::buildSheet(*doc, s)
                                      : layout::buildLayoutSheet(*doc, e.layout, s);
        if (!sheet)
            return Error{sheet.error().code, what + ": " + sheet.error().message};
        if (sheet.value().layout.overflows)
            set.warnings.push_back(what + " is larger than its paper");
        if (options.decorate)
            options.decorate(sheet.value(), e, s);
        set.sheets.push_back(std::move(sheet.value()));
    }
    return set;
}

Result<SheetSetReport> plotSheetList(const std::vector<SheetEntry>& entries,
                                     const exportlib::PlotSettings& base,
                                     const std::filesystem::path& pdfFile,
                                     const layout::StyleResolver& styles,
                                     const BuildOptions& options) {
    auto set = buildSheetList(entries, base, styles, options);
    if (!set)
        return set.error();
    auto written = exportlib::writePdfPages(set.value().sheets, pdfFile.string());
    if (!written)
        return written.error();
    SheetSetReport report;
    report.pages = set.value().sheets.size();
    report.warnings = std::move(set.value().warnings);
    return report;
}

}  // namespace tbcad::sheets
