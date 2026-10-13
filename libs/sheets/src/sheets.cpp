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
            e.layout = trim(line.substr(bar + 1));
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

Result<SheetSet> buildSheetList(const std::vector<SheetEntry>& entries,
                                const exportlib::PlotSettings& base,
                                const layout::StyleResolver& styles) {
    std::map<std::filesystem::path, std::unique_ptr<Document>> docs;
    SheetSet set;
    for (const SheetEntry& e : entries) {
        const std::string what =
            e.file.filename().string() + (e.layout.empty() ? "" : " / " + e.layout);
        auto it = docs.find(e.file);
        if (it == docs.end()) {
            io::IoReport ior;
            auto doc = io::readTbcad(e.file, ior);
            if (!doc)
                return Error{doc.error().code, what + ": " + doc.error().message};
            it = docs.emplace(e.file, std::move(doc.value())).first;
        }
        const auto setup = e.layout.empty() ? layout::pageSetupOfModel(*it->second)
                                            : layout::pageSetupOfLayout(*it->second, e.layout);
        const exportlib::PlotSettings s = setup ? layout::settingsFor(base, *setup, styles) : base;
        auto sheet = e.layout.empty() ? exportlib::buildSheet(*it->second, s)
                                      : layout::buildLayoutSheet(*it->second, e.layout, s);
        if (!sheet)
            return Error{sheet.error().code, what + ": " + sheet.error().message};
        if (sheet.value().layout.overflows)
            set.warnings.push_back(what + " is larger than its paper");
        set.sheets.push_back(std::move(sheet.value()));
    }
    return set;
}

Result<SheetSetReport> plotSheetList(const std::vector<SheetEntry>& entries,
                                     const exportlib::PlotSettings& base,
                                     const std::filesystem::path& pdfFile,
                                     const layout::StyleResolver& styles) {
    auto set = buildSheetList(entries, base, styles);
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
