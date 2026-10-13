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

Result<SheetSetReport> plotSheetList(const std::vector<SheetEntry>& entries,
                                     const exportlib::PlotSettings& base,
                                     const std::filesystem::path& pdfFile) {
    std::map<std::filesystem::path, std::unique_ptr<Document>> docs;
    std::vector<exportlib::PlotSheet> sheets;
    SheetSetReport report;
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
        auto sheet = e.layout.empty() ? exportlib::buildSheet(*it->second, base)
                                      : layout::buildLayoutSheet(*it->second, e.layout, base);
        if (!sheet)
            return Error{sheet.error().code, what + ": " + sheet.error().message};
        if (sheet.value().layout.overflows)
            report.warnings.push_back(what + " is larger than its paper");
        sheets.push_back(std::move(sheet.value()));
    }
    auto written = exportlib::writePdfPages(sheets, pdfFile.string());
    if (!written)
        return written.error();
    report.pages = sheets.size();
    return report;
}

}  // namespace tbcad::sheets
