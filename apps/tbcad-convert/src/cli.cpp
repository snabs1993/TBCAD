#include "tbcad/convert/cli.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>

#include "export/export.h"
#include "tbcad/core/document.h"
#include "tbcad/dxf/encoding.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/io/tbcad.h"

namespace tbcad::convert {

namespace {

const char* kUsage =
    "usage: tbcad-convert input.dxf -o output.tbcad [options]\n"
    "       tbcad-convert input.tbcad -o output.dxf [export options]\n"
    "\n"
    "A .tbcad input is exported to DXF, anything else is imported from DXF.\n"
    "\n"
    "options:\n"
    "  -o, --output FILE       the file to write (required)\n"
    "  --report FILE           write a JSON report (counts, units, encoding, warnings)\n"
    "  -q, --quiet             print nothing except errors\n"
    "  -h, --help              show this text\n"
    "\n"
    "import options (DXF -> .tbcad):\n"
    "  --units UNIT            treat the drawing as being in UNIT (mm, cm, m, km, in, ft, yd, mi\n"
    "                          or a $INSUNITS number) instead of what the file says\n"
    "  --encoding NAME         text encoding of the file (utf-8, cp874, cp1252, ...)\n"
    "  --split-layers SEP      turn layer names like A~b into a sub-layer b of A (SEP = ~)\n"
    "  --dimension-graphics    also keep the dimension drawings stored in the file\n"
    "\n"
    "export options (.tbcad -> DXF):\n"
    "  --version V             r2000 (default, AC1015), r12 (AC1009) or r2007 (AC1021, UTF-8)\n"
    "  --utf8                  same as --version r2007\n"
    "  --layer-separator SEP   joins sub-layer names in DXF (default ~), e.g. S-FLOOR~edge\n"
    "\n"
    "exit codes: 0 converted, 1 converted with warnings, 2 failed\n";

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::optional<int> parseUnits(const std::string& text) {
    std::string t = lower(text);
    struct U {
        const char* name;
        int code;
    };
    static constexpr U kNames[] = {
        {"in", 1},   {"inch", 1}, {"inches", 1}, {"ft", 2}, {"foot", 2}, {"feet", 2}, {"mi", 3},
        {"mile", 3}, {"mm", 4},   {"cm", 5},     {"m", 6},  {"km", 7},   {"yd", 10},  {"dm", 14},
    };
    for (const U& u : kNames) {
        if (t == u.name)
            return u.code;
    }
    if (!t.empty() && std::all_of(t.begin(), t.end(), [](char c) { return std::isdigit(c); })) {
        int v = std::atoi(t.c_str());
        if (v >= 1 && v <= 20)
            return v;
    }
    return std::nullopt;
}

std::optional<dxf::Encoding> parseEncoding(const std::string& text) {
    std::string t = lower(text);
    if (t == "utf8" || t == "utf-8")
        return dxf::Encoding::Utf8;
    if (t.rfind("cp", 0) == 0)
        t = "ansi_" + t.substr(2);
    return dxf::encodingFromName(t);
}

bool readFile(const std::string& path, std::vector<std::byte>& bytes) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    bytes.resize(raw.size());
    std::transform(raw.begin(), raw.end(), bytes.begin(),
                   [](char c) { return static_cast<std::byte>(c); });
    return true;
}

bool writeText(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;
    f << text;
    return static_cast<bool>(f);
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

int runCli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    std::string input, output, reportPath;
    dxf::ImportOptions options;
    dxf::ExportOptions exportOptions;
    bool quiet = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value = [&](const char* what) -> std::optional<std::string> {
            if (i + 1 >= args.size()) {
                err << "tbcad-convert: " << a << " needs " << what << "\n";
                return std::nullopt;
            }
            return args[++i];
        };
        if (a == "-h" || a == "--help") {
            out << kUsage;
            return kExitOk;
        } else if (a == "-q" || a == "--quiet") {
            quiet = true;
        } else if (a == "-o" || a == "--output") {
            auto v = value("a file name");
            if (!v)
                return kExitFailed;
            output = *v;
        } else if (a == "--report") {
            auto v = value("a file name");
            if (!v)
                return kExitFailed;
            reportPath = *v;
        } else if (a == "--units") {
            auto v = value("a unit");
            if (!v)
                return kExitFailed;
            options.insunits = parseUnits(*v);
            if (!options.insunits) {
                err << "tbcad-convert: unknown unit \"" << *v << "\"\n";
                return kExitFailed;
            }
        } else if (a == "--encoding") {
            auto v = value("an encoding name");
            if (!v)
                return kExitFailed;
            options.encoding = parseEncoding(*v);
            if (!options.encoding) {
                err << "tbcad-convert: unknown encoding \"" << *v << "\"\n";
                return kExitFailed;
            }
        } else if (a == "--split-layers") {
            auto v = value("a separator");
            if (!v)
                return kExitFailed;
            options.splitLayers = *v;
        } else if (a == "--dimension-graphics") {
            options.dimensionGraphics = true;
        } else if (a == "--no-dimension-graphics") {
            options.dimensionGraphics = false;  // the default; accepted for old scripts
        } else if (a == "--version") {
            auto v = value("a DXF version");
            if (!v)
                return kExitFailed;
            std::optional<dxf::DxfVersion> version = dxf::parseVersion(*v);
            if (!version) {
                err << "tbcad-convert: unknown DXF version \"" << *v
                    << "\" (use r12, r2000 or r2007)\n";
                return kExitFailed;
            }
            exportOptions.version = *version;
        } else if (a == "--utf8") {
            exportOptions.version = dxf::DxfVersion::R2007;
        } else if (a == "--layer-separator") {
            auto v = value("a separator");
            if (!v)
                return kExitFailed;
            exportOptions.layerSeparator = *v;
        } else if (!a.empty() && a[0] == '-') {
            err << "tbcad-convert: unknown option " << a << "\n" << kUsage;
            return kExitFailed;
        } else if (input.empty()) {
            input = a;
        } else {
            err << "tbcad-convert: more than one input file\n";
            return kExitFailed;
        }
    }
    if (input.empty() || output.empty()) {
        err << "tbcad-convert: an input file and -o output are required\n" << kUsage;
        return kExitFailed;
    }

    if (lower(std::filesystem::path(input).extension().string()) == ".tbcad") {
        ExportArgs exportArgs;
        exportArgs.input = input;
        exportArgs.output = output;
        exportArgs.reportPath = reportPath;
        exportArgs.options = exportOptions;
        exportArgs.quiet = quiet;
        return runExport(exportArgs, out, err);
    }

    auto fail = [&](const std::string& message, const dxf::Report& report) {
        err << "tbcad-convert: " << message << "\n";
        if (!reportPath.empty()) {
            writeText(reportPath, "{\"failed\":true,\"error\":" + jsonString(message) +
                                      ",\"report\":" + report.toJson() + "}");
        }
        return kExitFailed;
    };

    std::vector<std::byte> bytes;
    dxf::Report report;
    if (!readFile(input, bytes))
        return fail("cannot read " + input, report);

    Result<dxf::ImportResult> result = dxf::importDxf(bytes, options, report);
    if (!result)
        return fail(input + ": " + result.error().message, report);
    dxf::ImportResult& imported = result.value();

    Result<void> written = io::writeTbcad(*imported.document, std::filesystem::path(output));
    if (!written)
        return fail("cannot write " + output + ": " + written.error().message, report);

    if (!reportPath.empty() && !writeText(reportPath, imported.summary.toJson(report))) {
        return fail("cannot write " + reportPath, report);
    }

    std::size_t warnings =
        report.count(dxf::Severity::Warning) + report.count(dxf::Severity::Error);
    if (!quiet) {
        for (const dxf::Diagnostic& d : report.diagnostics()) {
            if (d.severity == dxf::Severity::Info)
                continue;
            err << input;
            if (d.line > 0)
                err << ":" << d.line;
            err << ": " << (d.severity == dxf::Severity::Error ? "error" : "warning") << ": "
                << d.message << " [" << d.code << "]\n";
        }
        out << "tbcad-convert: " << imported.document->modelSpaceCount() << " entities, "
            << imported.summary.layers << " layers, " << imported.summary.blocks << " blocks, "
            << warnings << " warnings -> " << output << "\n";
    }
    return warnings > 0 ? kExitWarnings : kExitOk;
}

}  // namespace tbcad::convert
