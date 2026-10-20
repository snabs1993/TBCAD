#include "tbcad/convert/cli.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>

#include "export/export.h"
#include "tbcad/core/document.h"
#include "tbcad/dwg/dwg.h"
#include "tbcad/dxf/encoding.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/platform/paths.h"
#include "tbcad/xref/xref.h"
#include "timing.h"

namespace tbcad::convert {

namespace {

const char* kUsage =
    "usage: tbcad-convert input.dxf -o output.tbcad [options]\n"
    "       tbcad-convert input.dwg -o output.tbcad|output.dxf [options]\n"
    "       tbcad-convert input.tbcad -o output.dxf [export options]\n"
    "\n"
    "A .tbcad input is exported to DXF, a .dwg input is read (R2000 and later, read-only) and\n"
    "written as .tbcad or .dxf, anything else is imported from DXF.\n"
    "\n"
    "options:\n"
    "  -o, --output FILE       the file to write (required)\n"
    "  --reader-revision       print which revision of the .dwg reader this build has\n"
    "  --lz21-search FILE...   try other literal-run cuts on R2007 files, print scores\n"
    "  --dwg-info FILE         describe a .dwg file's structure (for diagnosing files that fail)\n"
    "  --report FILE           write a JSON report (counts, units, encoding, warnings)\n"
    "  --timeout SECONDS       give up (exit 2) when the conversion takes longer; 0 = never\n"
    "                          (default 600)\n"
    "  -q, --quiet             print nothing except errors\n"
    "  --timing                print the time and memory of each step (read, convert, write)\n"
    "  --no-xrefs              leave external references unloaded (by default an xref whose\n"
    "                          file is found, also next to the drawing, is loaded into it)\n"
    "  -h, --help              show this text\n"
    "\n"
    "import options (DXF -> .tbcad):\n"
    "  --units UNIT            treat the drawing as being in UNIT (mm, cm, m, km, in, ft, yd, mi\n"
    "                          or a $INSUNITS number) instead of what the file says\n"
    "  --encoding NAME         text encoding of the file (utf-8, cp874, cp1252, ...)\n"
    "  --split-layers SEP      turn layer names like A~b into a sub-layer b of A (SEP = ~)\n"
    "  --dimension-graphics    also keep the dimension drawings stored in the file\n"
    "  --dim-compare           list in the --report how TBCAD's dimensions differ from the\n"
    "                          drawings stored in the file\n"
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

bool readFile(const std::string& path, std::vector<std::byte>& bytes);
std::string jsonString(const std::string& s);

// The report without the "external reference ... is not loaded" warnings of the xrefs that were
// loaded after all (an info line says so instead).
dxf::Report withoutLoadedXrefWarnings(dxf::Report report, const std::vector<std::string>& loaded) {
    dxf::Report out;
    for (dxf::Diagnostic& d : report.takeItems()) {
        bool drop = false;
        if (d.code == dxf::code::kXref)
            for (const std::string& name : loaded)
                drop = drop || d.message.starts_with("external reference \"" + name + "\" ");
        if (!drop)
            out.add(d.severity, d.code, std::move(d.message), d.line, d.handle);
    }
    for (const std::string& name : loaded)
        out.info(dxf::code::kXref, "external reference \"" + name + "\" loaded from its file");
    return out;
}

// The JSON report with what was done about external references added at its end.
std::string withXrefs(std::string json, const xref::OpenLoad& load, std::size_t entities) {
    if (json.empty() || json.back() != '}')
        return json;
    auto list = [](const std::vector<std::string>& names) {
        std::string o = "[";
        for (std::size_t i = 0; i < names.size(); ++i)
            o += (i ? "," : "") + jsonString(names[i]);
        return o + "]";
    };
    json.pop_back();
    json += ",\"xrefs\":{\"loaded\":" + list(load.loaded) + ",\"missing\":" + list(load.missing) +
            ",\"failed\":" + list(load.failed) + ",\"entities\":" + std::to_string(entities) + "}}";
    return json;
}

// Lets an xref point at a .dwg file (the app registers the same in its DWG opener).
void registerDwgXrefLoader() {
    static const bool once = [] {
        xref::registerLoader(".dwg", [](const std::filesystem::path& file) -> Result<xref::Source> {
            std::vector<std::byte> bytes;
            if (!readFile(platform::pathToUtf8(file), bytes))
                return makeError(ErrorCode::Io, "cannot read " + platform::pathToUtf8(file));
            dxf::Report report;
            auto r = dwg::importDwg(bytes, dxf::ImportOptions{}, report);
            if (!r)
                return r.error();
            xref::Source s;
            s.doc = std::move(r.value().document);
            return s;
        });
        return true;
    }();
    (void)once;
}

bool readFile(const std::string& path, std::vector<std::byte>& bytes) {
    std::ifstream f(platform::longPath(platform::pathFromUtf8(path)), std::ios::binary);
    if (!f)
        return false;
    // One read straight into the buffer: copying through a char vector doubled the peak memory.
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size < 0)
        return false;
    f.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(size));
    f.read(reinterpret_cast<char*>(bytes.data()), size);
    return f.gcount() == size;
}

bool writeText(const std::string& path, const std::string& text) {
    std::ofstream f(platform::longPath(platform::pathFromUtf8(path)), std::ios::binary);
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

Watchdog::Watchdog(std::chrono::seconds limit, std::function<void()> onExpire) {
    if (limit.count() <= 0)
        return;
    thread_ = std::thread([this, limit, onExpire = std::move(onExpire)] {
        std::unique_lock lock(m_);
        if (!cv_.wait_for(lock, limit, [this] { return done_; }))
            onExpire();
    });
}

Watchdog::~Watchdog() {
    {
        std::lock_guard lock(m_);
        done_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

int runCli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    std::string input, output, reportPath;
    long timeoutSeconds = 600;
    dxf::ImportOptions options;
    dxf::ExportOptions exportOptions;
    bool quiet = false;
    bool timing = false;
    bool loadXrefs = true;

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
        } else if (a == "--reader-revision") {
            out << dwg::readerRevision() << "\n";
            return kExitOk;
        } else if (a == "--lz21-search") {
            std::vector<std::vector<std::byte>> raws;
            while (i + 1 < args.size() && args[i + 1].rfind("-", 0) != 0) {
                std::vector<std::byte> raw;
                if (!readFile(args[++i], raw)) {
                    err << "tbcad-convert: cannot read " << args[i] << "\n";
                    return kExitFailed;
                }
                raws.push_back(std::move(raw));
            }
            std::vector<std::span<const std::byte>> spans(raws.begin(), raws.end());
            out << dwg::searchCuts(spans);
            return kExitOk;
        } else if (a == "--dwg-info") {
            auto v = value("a .dwg file");
            if (!v)
                return kExitFailed;
            std::vector<std::byte> raw;
            if (!readFile(*v, raw)) {
                err << "tbcad-convert: cannot read " << *v << "\n";
                return kExitFailed;
            }
            out << dwg::describe(raw);
            return kExitOk;
        } else if (a == "-q" || a == "--quiet") {
            quiet = true;
        } else if (a == "--timing") {
            timing = true;
        } else if (a == "--no-xrefs") {
            loadXrefs = false;
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
        } else if (a == "--timeout") {
            auto v = value("a number of seconds");
            if (!v)
                return kExitFailed;
            char* end = nullptr;
            timeoutSeconds = std::strtol(v->c_str(), &end, 10);
            if (v->empty() || *end != '\0' || timeoutSeconds < 0) {
                err << "tbcad-convert: --timeout needs a number of seconds (0 = never)\n";
                return kExitFailed;
            }
        } else if (a == "--dimension-graphics") {
            options.dimensionGraphics = true;
        } else if (a == "--dim-compare") {
            options.dimensionCompare = true;
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
        exportArgs.timing = timing;
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

    // A pathological file may not keep the converter busy for ever: report it and stop.
    Watchdog watchdog(std::chrono::seconds(timeoutSeconds), [&] {
        err << "tbcad-convert: " << input << ": gave up after " << timeoutSeconds
            << " s (use --timeout to allow longer)\n";
        err.flush();
        if (!reportPath.empty())
            writeText(reportPath, "{\"failed\":true,\"error\":\"timeout\"}");
        std::_Exit(kExitFailed);
    });

    PhaseTimer timer(err, timing);
    std::vector<std::byte> bytes;
    dxf::Report report;
    if (!readFile(input, bytes))
        return fail("cannot read " + input, report);
    timer.phase("read file");

    const bool fromDwg = lower(std::filesystem::path(input).extension().string()) == ".dwg";
    Result<dxf::ImportResult> result =
        fromDwg ? dwg::importDwg(bytes, options, report) : dxf::importDxf(bytes, options, report);
    if (!result)
        return fail(input + ": " + result.error().message, report);
    dxf::ImportResult& imported = result.value();
    std::vector<std::byte>().swap(bytes);  // the file's text is not needed any more
    timer.phase(fromDwg ? "read dwg" : "import dxf");

    // As AutoCAD does on open: external references whose files are found are loaded.
    xref::OpenLoad xrefLoad;
    if (loadXrefs) {
        registerDwgXrefLoader();
        xrefLoad = xref::loadOnOpen(*imported.document, platform::pathFromUtf8(input));
        if (!xrefLoad.loaded.empty() || !xrefLoad.failed.empty())
            timer.phase("load xrefs");
        if (!xrefLoad.loaded.empty())
            report = withoutLoadedXrefWarnings(std::move(report), xrefLoad.loaded);
    }

    // A DWG file whose objects could not be read must not leave an empty drawing behind. A drawing
    // that really is empty reads without complaints, so only an empty result with warnings fails.
    if (fromDwg && imported.document->modelSpaceCount() == 0 &&
        report.count(dxf::Severity::Warning) + report.count(dxf::Severity::Error) > 0) {
        std::size_t read = 0;
        for (const auto& entry : imported.summary.entities)
            read += entry.second.read;
        if (read == 0)
            return fail(input + ": no drawing objects could be read from this file", report);
    }

    if (fromDwg && lower(std::filesystem::path(output).extension().string()) == ".dxf") {
        Result<dxf::ExportResult> exported =
            dxf::exportDxf(*imported.document, exportOptions, report);
        if (!exported)
            return fail("cannot export " + output + ": " + exported.error().message, report);
        if (!writeText(output, exported.value().bytes))
            return fail("cannot write " + output, report);
    } else {
        Result<void> written = io::writeTbcad(*imported.document, platform::pathFromUtf8(output));
        if (!written)
            return fail("cannot write " + output + ": " + written.error().message, report);
    }
    timer.phase("write");

    std::size_t xrefEntities = 0;
    for (const std::string& name : xrefLoad.loaded)
        if (const BlockDef* def = imported.document->blockByName(name))
            xrefEntities += def->entities.size();
    if (!reportPath.empty() && !writeText(reportPath, withXrefs(imported.summary.toJson(report),
                                                                xrefLoad, xrefEntities))) {
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
        for (const std::string& name : xrefLoad.loaded)
            err << input << ": xref \"" << name << "\" loaded from its file\n";
        for (const std::string& line : xrefLoad.failed)
            err << input << ": warning: xref " << line << " (left unloaded)\n";
        out << "tbcad-convert: " << imported.document->modelSpaceCount() << " entities, "
            << imported.summary.layers << " layers, " << imported.summary.blocks << " blocks, ";
        if (!xrefLoad.loaded.empty() || !xrefLoad.missing.empty() || !xrefLoad.failed.empty())
            out << "xrefs " << xrefLoad.loaded.size() << " loaded (" << xrefEntities
                << " entities), " << xrefLoad.missing.size() + xrefLoad.failed.size()
                << " not loaded, ";
        out << warnings << " warnings -> " << output << "\n";
    }
    return warnings > 0 ? kExitWarnings : kExitOk;
}

}  // namespace tbcad::convert
