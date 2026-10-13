#include "export.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "tbcad/convert/cli.h"
#include "tbcad/dxf/report.h"
#include "tbcad/io/tbcad.h"

namespace tbcad::convert {

namespace {

bool writeFile(const std::string& path, const std::string& bytes) {
    // Write next to the target first so an interrupted run never leaves half a DXF behind.
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f)
            return false;
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!f)
            return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        std::filesystem::rename(tmp, path, ec);
    }
    return !ec;
}

std::string jsonString(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') {
            o.push_back('\\');
            o.push_back(c);
        } else if (static_cast<unsigned char>(c) < 0x20) {
            o += ' ';
        } else {
            o.push_back(c);
        }
    }
    return o + "\"";
}

}  // namespace

int runExport(const ExportArgs& args, std::ostream& out, std::ostream& err) {
    auto fail = [&](const std::string& message) {
        err << "tbcad-convert: " << message << "\n";
        if (!args.reportPath.empty()) {
            std::ofstream f(args.reportPath, std::ios::binary);
            f << "{\"failed\":true,\"error\":" << jsonString(message) << "}";
        }
        return kExitFailed;
    };

    io::IoReport ioReport;
    Result<std::unique_ptr<Document>> doc =
        io::readTbcad(std::filesystem::path(args.input), ioReport);
    if (!doc)
        return fail("cannot read " + args.input + ": " + doc.error().message);

    dxf::Report report;
    dxf::Result<dxf::ExportResult> result = dxf::exportDxf(*doc.value(), args.options, report);
    if (!result)
        return fail(result.error().message);

    if (!writeFile(args.output, result.value().bytes))
        return fail("cannot write " + args.output);

    const dxf::ExportSummary& summary = result.value().summary;
    if (!args.reportPath.empty()) {
        std::ofstream f(args.reportPath, std::ios::binary);
        f << summary.toJson(report);
        if (!f)
            return fail("cannot write " + args.reportPath);
    }

    std::size_t warnings =
        report.count(dxf::Severity::Warning) + report.count(dxf::Severity::Error);
    if (!args.quiet) {
        for (const dxf::Diagnostic& d : report.diagnostics()) {
            if (d.severity == dxf::Severity::Info)
                continue;
            err << args.input << ": " << (d.severity == dxf::Severity::Error ? "error" : "warning")
                << ": " << d.message << " [" << d.code << "]\n";
        }
        std::size_t written = 0;
        for (const auto& [type, n] : summary.entities)
            written += n;
        out << "tbcad-convert: " << written << " DXF records, " << summary.layers << " layers, "
            << summary.blocks << " blocks (" << summary.version << "), " << warnings
            << " warnings -> " << args.output << "\n";
    }
    return warnings > 0 ? kExitWarnings : kExitOk;
}

}  // namespace tbcad::convert
