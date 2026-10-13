#include "tbcad/dwgopen/dwgopen.h"

#include <fstream>
#include <iterator>

#include "tbcad/dwg/dwg.h"
#include "tbcad/dxf/writer.h"

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
            out.push_back(std::string(d.severity == dxf::Severity::Error ? "Error: " : "Warning: ") +
                          d.message);
            ++shown;
        }
    }
    if (total > shown)
        out.push_back("... and " + std::to_string(total - shown) + " more warnings.");
    return out;
}

}  // namespace

Result<ui::ForeignOpenResult> readDwgFile(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return makeError(ErrorCode::Io, "The file cannot be read.");
    std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i)
        bytes[i] = static_cast<std::byte>(raw[i]);

    dxf::Report report;
    dxf::Result<dxf::ImportResult> imported = dwg::importDwg(bytes, {}, report);
    if (!imported)
        return makeError(ErrorCode::Parse, imported.error().message);

    dxf::ImportResult& r = imported.value();
    ui::ForeignOpenResult out;
    out.format = "DWG " + r.summary.version;
    out.messages = messagesOf(report);
    out.messages.insert(out.messages.begin(),
                        "DWG " + r.summary.version + ": " + std::to_string(r.document->modelSpaceCount()) +
                            " objects, " + std::to_string(r.summary.layers) + " layers, " +
                            std::to_string(r.summary.blocks) + " blocks.");
    out.document = std::move(r.document);
    return out;
}

Result<std::vector<std::string>> writeDxfFile(const Document& doc,
                                              const std::filesystem::path& path) {
    dxf::Report report;
    dxf::Result<dxf::ExportResult> exported = dxf::exportDxf(doc, {}, report);
    if (!exported)
        return makeError(ErrorCode::Generic, exported.error().message);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f)
        return makeError(ErrorCode::Io, "The file cannot be written.");
    f << exported.value().bytes;
    f.flush();
    if (!f)
        return makeError(ErrorCode::Io, "Writing the file failed.");
    return messagesOf(report);
}

void registerForeignFormats(ui::AppCore& core) {
    core.setForeignReader(".dwg", readDwgFile, /*readOnly=*/true);
    core.setDxfWriter(writeDxfFile);
}

}  // namespace tbcad::dwgopen
