#include "tbcad/dwgopen/dwgopen.h"

#include <chrono>
#include <fstream>
#include <iterator>

#include "tbcad/dwg/dwg.h"
#include "tbcad/platform/paths.h"
#include "tbcad/xref/xref.h"

namespace tbcad::dwgopen {

namespace {

constexpr std::size_t kMaxMessages = 20;

std::vector<std::string> messagesOf(const dxf::Report& report) {
    std::vector<std::string> out;
    std::size_t shown = 0, total = 0;
    for (const dxf::Diagnostic& d : report.diagnostics()) {
        if (d.severity == dxf::Severity::Info)
            continue;
        ++total;
        if (shown < kMaxMessages) {
            out.push_back(
                std::string(d.severity == dxf::Severity::Error ? "Error: " : "Warning: ") +
                d.message);
            ++shown;
        }
    }
    if (total > shown)
        out.push_back("... and " + std::to_string(total - shown) + " more warnings.");
    return out;
}

// Every warning, error and skipped-object line, none dropped: what DWGREPORT hands over.
std::vector<std::string> fullReportOf(const dxf::Report& report) {
    std::vector<std::string> out;
    for (const dxf::Diagnostic& d : report.diagnostics()) {
        if (d.severity == dxf::Severity::Info && d.code != dwg::code::kSkippedObject)
            continue;
        out.push_back(std::string(d.severity == dxf::Severity::Error     ? "Error: "
                                  : d.severity == dxf::Severity::Warning ? "Warning: "
                                                                         : "") +
                      d.message);
    }
    return out;
}

std::string productOf(const std::string& code) {
    if (code == "AC1015")
        return "AutoCAD 2000";
    if (code == "AC1018")
        return "AutoCAD 2004";
    if (code == "AC1021")
        return "AutoCAD 2007";
    if (code == "AC1024")
        return "AutoCAD 2010";
    if (code == "AC1027")
        return "AutoCAD 2013";
    if (code == "AC1032")
        return "AutoCAD 2018";
    return "version " + code;
}

}  // namespace

Result<ui::ForeignOpenResult> readDwgFile(const std::filesystem::path& path) {
    auto stepAt = std::chrono::steady_clock::now();
    std::vector<std::pair<std::string, double>> timings;
    auto step = [&](const char* name) {
        const auto t = std::chrono::steady_clock::now();
        timings.emplace_back(name, std::chrono::duration<double, std::milli>(t - stepAt).count());
        stepAt = t;
    };
    std::ifstream f(platform::longPath(path), std::ios::binary);
    if (!f)
        return makeError(ErrorCode::Io, "The file cannot be read.");
    // One read of the whole file (was a character at a time, then copied).
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(size > 0 ? static_cast<std::size_t>(size) : 0);
    if (!bytes.empty())
        f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    step("load file bytes");

    dxf::Report report;
    dxf::ImportOptions options;
    options.diagnostics = false;  // nobody reads the structure trace when opening a drawing
    std::vector<std::pair<std::string, double>> inner;
    Result<dxf::ImportResult> imported = dwg::importDwg(bytes, options, report, &inner);
    step("decode, convert and import (importDwg)");
    for (auto& [name, ms] : inner)
        timings.emplace_back("  " + name, ms);
    if (!imported) {
        // One plain sentence for the message box; the reader's reason follows it.
        const dwg::VersionInfo v = dwg::detectVersion(bytes);
        std::string msg = v.code.empty() ? std::string("This is not a DWG file.")
                                         : "This DWG file (" + productOf(v.code) +
                                               " format) could not be opened.";
        msg += " " + imported.error().message;
        return makeError(ErrorCode::Parse, msg);
    }

    dxf::ImportResult& r = imported.value();
    ui::ForeignOpenResult out;
    out.format = "DWG " + r.summary.version;
    out.messages = messagesOf(report);
    out.report = fullReportOf(report);
    if (!out.report.empty())
        out.messages.push_back(
            "Some objects were skipped or changed; type DWGREPORT to copy the "
            "list so it can be sent for a fix.");
    out.messages.insert(out.messages.begin(), "DWG " + r.summary.version + ": " +
                                                  std::to_string(r.document->modelSpaceCount()) +
                                                  " objects, " + std::to_string(r.summary.layers) +
                                                  " layers, " + std::to_string(r.summary.blocks) +
                                                  " blocks.");
    out.document = std::move(r.document);
    out.timings = std::move(timings);
    return out;
}

void registerForeignFormats(ui::AppCore& core) {
    // External references whose files are found are loaded, as AutoCAD does on open (not in
    // readDwgFile itself: that is also the xref loader, which reloads nested xrefs on its own).
    core.setForeignReader(
        ".dwg",
        [](const std::filesystem::path& file) -> Result<ui::ForeignOpenResult> {
            auto r = readDwgFile(file);
            if (!r || !r.value().document)
                return r;
            const xref::OpenLoad load = xref::loadOnOpen(*r.value().document, file);
            for (const std::string& name : load.loaded)
                r.value().messages.push_back("Loaded external reference " + name + ".");
            for (const std::string& line : load.failed)
                r.value().messages.push_back("Cannot load external reference " + line + ".");
            return r;
        },
        /*readOnly=*/true);
    // A .dwg can also be an external reference.
    xref::registerLoader(".dwg", [](const std::filesystem::path& file) -> Result<xref::Source> {
        auto r = readDwgFile(file);
        if (!r)
            return r.error();
        xref::Source s;
        s.doc = std::move(r.value().document);
        return s;
    });
}

}  // namespace tbcad::dwgopen
