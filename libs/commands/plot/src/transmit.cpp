#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

#include "tbcad/io/zip.h"
#include "tbcad/layout/pagesetup.h"
#include "tbcad/platform/paths.h"
#include "tbcad/plot/plot.h"
#include "tbcad/text/text.h"
#include "tbcad/underlay/image.h"
#include "tbcad/underlay/pdf.h"
#include "tbcad/xref/xref.h"

namespace tbcad::plot {

namespace {

namespace fs = std::filesystem;

std::string lowerText(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string utf8(const fs::path& p) {
    return platform::pathToUtf8(p);
}

fs::path canonicalOf(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    return ec ? p.lexically_normal() : c;
}

class Collector {
public:
    Collector(TransmitSet& out) : out_(out) {}

    // Adds the file unless it is already in the package; false when it was there.
    bool add(const fs::path& source, std::string target, std::string kind, std::string note,
             bool moved = false) {
        if (!sources_.insert(canonicalOf(source)).second)
            return false;
        // Two different files with one name in refs/ or fonts/: number the later one.
        std::string unique = target;
        for (int n = 2; !targets_.insert(lowerText(unique)).second; ++n) {
            const fs::path t = platform::pathFromUtf8(target);
            unique = utf8(t.parent_path() /
                          (utf8(t.stem()) + "-" + std::to_string(n) + utf8(t.extension())));
            unique = platform::pathFromUtf8(unique).generic_string();
        }
        out_.files.push_back({source, unique, std::move(kind), std::move(note), moved});
        return true;
    }

    // The references of `doc` (read from `file`, packed at `targetDir`), and theirs in turn.
    void references(const Document& doc, const fs::path& file, const fs::path& targetDir,
                    int depth) {
        const std::string from = utf8(file.filename());
        for (BlockId id : xref::xrefs(doc)) {
            const BlockDef* def = doc.block(id);
            if (!def)
                continue;
            const fs::path found = xref::resolvePath(def->xrefPath, file);
            std::error_code ec;
            if (!fs::is_regular_file(found, ec)) {
                out_.missing.push_back(def->xrefPath + " (" + def->name + ", in " + from + ")");
                continue;
            }
            const std::string kind = underlay::isImagePath(def->xrefPath) ? "image"
                                     : underlay::isPdfPath(def->xrefPath) ? "pdf"
                                                                          : "xref";
            const fs::path rel = platform::pathFromUtf8(underlay::parseSpec(def->xrefPath).file);
            const bool inside = rel.is_relative() && !rel.empty() && *rel.begin() != "..";
            const fs::path target =
                inside ? (targetDir / rel).lexically_normal() : fs::path("refs") / found.filename();
            const std::string note = inside ? def->name + ", in " + from
                                            : def->name + ", in " + from + ": stored as " +
                                                  def->xrefPath + ", now refs/" +
                                                  utf8(found.filename()) + " (set the path again)";
            if (!add(found, target.generic_string(), kind, note, !inside))
                continue;
            if (kind != "xref" || depth >= 16)
                continue;
            auto nested = xref::loadSource(found);
            if (nested && nested.value().doc)
                references(*nested.value().doc, found, target.parent_path(), depth + 1);
        }
    }

private:
    TransmitSet& out_;
    std::set<fs::path> sources_{};
    std::set<std::string> targets_{};
};

std::string withSlashes(std::string s) {
    for (char& c : s)
        if (c == '\\')
            c = '/';
    return s;
}

void addFonts(Collector& c, TransmitSet& out, const Document& doc, const fs::path& drawingFile) {
    for (const auto& [id, st] : doc.textStyles()) {
        std::set<std::string> files;
        for (const std::string* name : {&st.fontFile, &st.shxFile, &st.bigFontFile})
            if (!name->empty())
                files.insert(*name);
        for (const std::string& name : files) {
            fs::path found = text::locateFontFile(name);
            std::error_code ec;
            if (found.empty()) {
                const fs::path asGiven = platform::pathFromUtf8(name);
                const fs::path nextTo = drawingFile.parent_path() / asGiven.filename();
                if (asGiven.is_absolute() && fs::is_regular_file(asGiven, ec))
                    found = asGiven;
                else if (fs::is_regular_file(nextTo, ec))
                    found = nextTo;
            }
            if (found.empty()) {
                out.missing.push_back("font " + name + " (text style " + st.name + ")");
                continue;
            }
            const std::string target = "fonts/" + utf8(found.filename());
            c.add(found, withSlashes(target), "font", "text style " + st.name);
        }
        if (st.fontFile.empty() && !st.fontFamily.empty() && !text::isStrokeFamily(st.fontFamily))
            out.notes.push_back("Text style " + st.name + " uses the installed font " +
                                st.fontFamily +
                                ", which is not included: install it on the other "
                                "computer.");
    }
}

void addPlotStyles(Collector& c, TransmitSet& out, const Document& doc, const cmd::IHost& host) {
    std::vector<std::pair<std::string, std::string>> wanted;  // table, used by
    wanted.emplace_back(plotStyleName(host), "the current plot settings");
    for (const auto& setup : layout::pageSetups(doc))
        wanted.emplace_back(setup.styleName, "page setup " + setup.name);
    for (const auto& [name, user] : wanted) {
        const std::string low = lowerText(name);
        if (low.empty() || low == "none" || plotstyle::builtinTable(name))
            continue;
        const fs::path file = styleTableFile(host, name);
        if (file.empty()) {
            out.missing.push_back("plot style table " + name + " (" + user + ")");
            continue;
        }
        if (!c.add(file, "plotstyles/" + utf8(file.filename()), "plot style", user)) {
            for (auto& f : out.files)
                if (f.kind == "plot style" && canonicalOf(f.source) == canonicalOf(file))
                    f.note += ", " + user;
        }
    }
}

}  // namespace

TransmitSet transmittalSet(const Document& doc, const fs::path& drawingFile,
                           const cmd::IHost& host) {
    TransmitSet out;
    Collector c(out);
    c.add(drawingFile, utf8(drawingFile.filename()), "drawing", "");
    c.references(doc, drawingFile, {}, 0);
    addFonts(c, out, doc, drawingFile);
    addPlotStyles(c, out, doc, host);
    return out;
}

std::string transmittalReport(const TransmitSet& set, const fs::path& drawingFile) {
    std::time_t now = std::time(nullptr);
    std::tm t{};
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    char when[32];
    std::strftime(when, sizeof when, "%Y-%m-%d %H:%M", &t);
    std::string r = "Transmittal of " + utf8(drawingFile.filename()) + "\n";
    r += std::string("Created ") + when + " with TBCAD\n\n";
    r += "Files (" + std::to_string(set.files.size()) + "):\n";
    std::uintmax_t total = 0;
    for (const auto& f : set.files) {
        std::error_code ec;
        const auto size = fs::file_size(f.source, ec);
        if (!ec)
            total += size;
        std::string kind = f.kind;
        kind.resize(std::max<std::size_t>(kind.size(), 11), ' ');
        r += "  " + kind + " " + f.target + (f.note.empty() ? "" : "  (" + f.note + ")") + "\n";
    }
    r += "  total " + std::to_string((total + 1023) / 1024) + " KB\n";
    if (!set.missing.empty()) {
        r += "\nNot found (" + std::to_string(set.missing.size()) + "):\n";
        for (const auto& m : set.missing)
            r += "  MISSING  " + m + "\n";
    }
    if (!set.notes.empty()) {
        r += "\nNotes:\n";
        for (const auto& n : set.notes)
            r += "  " + n + "\n";
    }
    return r;
}

namespace {

// ETRANSMIT [ARCHIVE]: packs the saved drawing with its xrefs (nested ones too), pictures, PDF
// underlays, the font files its text styles name and the plot style tables it plots with, into a
// folder or a .zip file, with transmittal.txt listing everything and what was not found.
class ETransmitCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ETRANSMIT"; }
    std::vector<std::string> defaultAliases() const override { return {"ARCHIVE", "-ETRANSMIT"}; }
    std::string_view description() const override {
        return "Pack the drawing with its xrefs, fonts and plot styles into a folder or zip";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        std::string saved = ctx.host().documentFile();
        if (saved.empty())  // hosts without a file of their own (scripts) say it in a setting
            saved = ctx.host().setting("document.file").value_or("");
        const fs::path hostFile = platform::pathFromUtf8(saved);
        std::error_code ec;
        if (hostFile.empty() || !fs::exists(hostFile, ec)) {
            ctx.message("Save the drawing first: ETRANSMIT packs the saved file.");
            co_return;
        }
        cmd::StringOptions so;
        so.defaultValue =
            utf8(hostFile.parent_path() / (utf8(hostFile.stem()) + "-transmittal.zip"));
        auto f = co_await ctx.getString("Destination folder or .zip file", so);
        if (!f.ok() || cmd::trim(f.text).empty())
            co_return;
        const std::string text = cmd::trim(f.text);
        const fs::path dest = platform::pathFromUtf8(text);
        const TransmitSet set = transmittalSet(ctx.doc(), hostFile, ctx.host());
        const std::string report = transmittalReport(set, hostFile);
        std::size_t moved = 0;
        for (const auto& file : set.files)
            moved += file.moved ? 1 : 0;
        std::string failed;
        if (lowerText(utf8(dest.extension())) == ".zip") {
            std::vector<io::ZipEntry> entries;
            for (const auto& file : set.files) {
                std::ifstream in(file.source, std::ios::binary);
                if (!in) {
                    failed += (failed.empty() ? "" : ", ") + file.target;
                    continue;
                }
                io::ZipEntry e;
                e.name = file.target;
                e.data.assign(std::istreambuf_iterator<char>(in), {});
                entries.push_back(std::move(e));
            }
            entries.push_back({"transmittal.txt", {report.begin(), report.end()}});
            if (!dest.parent_path().empty())
                fs::create_directories(dest.parent_path(), ec);
            if (auto w = io::writeZip(dest, entries); !w) {
                ctx.message("Cannot write " + text + ": " + w.error().message);
                co_return;
            }
        } else {
            fs::create_directories(dest, ec);
            if (ec) {
                ctx.message("Cannot create " + text + ": " + ec.message());
                co_return;
            }
            for (const auto& file : set.files) {
                const fs::path to = dest / platform::pathFromUtf8(file.target);
                std::error_code e;
                fs::create_directories(to.parent_path(), e);
                fs::copy_file(file.source, to, fs::copy_options::overwrite_existing, e);
                if (e)
                    failed += (failed.empty() ? "" : ", ") + file.target;
            }
            std::ofstream(dest / "transmittal.txt", std::ios::binary) << report;
        }
        std::string line = std::to_string(set.files.size()) + " file(s) packed into " + text;
        if (!set.missing.empty())
            line += ", " + std::to_string(set.missing.size()) + " not found";
        if (moved)
            line += ", " + std::to_string(moved) + " moved to refs (new paths needed)";
        if (!failed.empty())
            line += ". Could not copy: " + failed;
        ctx.message(line + ". See transmittal.txt.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeETransmitCommand() {
    return std::make_unique<ETransmitCommand>();
}

}  // namespace tbcad::plot
