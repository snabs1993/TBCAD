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
    std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i)
        bytes[i] = static_cast<std::byte>(raw[i]);
    step("load file bytes");

    dxf::Report report;
    dxf::ImportOptions options;
    options.diagnostics = false;  // nobody reads the structure trace when opening a drawing
    Result<dxf::ImportResult> imported = dwg::importDwg(bytes, options, report);
    step("decode, convert and import (importDwg)");
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
    core.setForeignReader(".dwg", readDwgFile, /*readOnly=*/true);
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
