#include "tbcad/dxfexport/dxfexport.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>

#include "tbcad/cmd/cmd.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"
#include "tbcad/io/tbcad.h"

namespace tbcad::dxfexport {

namespace {

namespace fs = std::filesystem;

// Typed names are UTF-8 on every platform.
fs::path pathOf(const std::string& utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string summaryLine(const dxf::Report& report) {
    return std::to_string(report.count(dxf::Severity::Warning)) + " warning(s), " +
           std::to_string(report.count(dxf::Severity::Error)) + " error(s)";
}

// EXPORTDXF: file name, then the DXF version. Writes the whole drawing; nothing in the document
// changes.
class ExportDxf final : public cmd::Command {
public:
    std::string_view name() const override { return "EXPORTDXF"; }
    std::string_view description() const override { return "Save the drawing as a DXF file"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::StringOptions so;
        so.defaultValue = "drawing.dxf";
        auto path = co_await ctx.getString("Enter DXF file name", std::move(so));
        if (!path.ok())
            co_return;

        // A bare name gets .dxf, so the file opens by double-click.
        std::string fileName = path.text;
        if (pathOf(fileName).extension().empty())
            fileName += ".dxf";
        if (std::error_code exists; fs::exists(pathOf(fileName), exists)) {
            cmd::KeywordOptions yn;
            yn.keywords = {"Yes", "No"};
            yn.defaultKeyword = "No";
            auto answer =
                co_await ctx.getKeyword(fileName + " already exists. Replace it?", std::move(yn));
            if (answer.status != cmd::Status::Keyword || answer.keyword != "Yes") {
                ctx.message("Export cancelled; the existing file was not changed.");
                co_return;
            }
        }

        cmd::KeywordOptions ko;
        ko.keywords = {"R2000", "R12", "R2007"};
        ko.defaultKeyword = "R2000";
        auto ver = co_await ctx.getKeyword("DXF version", std::move(ko));
        if (ver.status != cmd::Status::Keyword)
            co_return;

        dxf::ExportOptions opt;
        opt.version = dxf::parseVersion(ver.keyword).value_or(dxf::DxfVersion::R2000);
        dxf::Report report;
        auto res = dxf::exportDxf(ctx.doc(), opt, report);
        if (!res) {
            ctx.message("EXPORTDXF failed: " + res.error().message);
            co_return;
        }
        const fs::path out = pathOf(fileName);
        // Same safe write as Save: synced temporary file, then swapped in, the old file kept as
        // .bak when ISAVEBAK is on.
        io::WriteOptions wo;
        wo.keepBackup = ctx.host().setting("files.backup").value_or("1") != "0";
        const auto& bytes = res.value().bytes;
        auto saved = io::saveTbcadBytes(
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                          bytes.size()),
            out, wo);
        if (!saved) {
            ctx.message("Cannot write " + fileName + ": " + saved.error().message);
            co_return;
        }
        ctx.message("Saved " + fileName + " (" + std::string(dxf::versionCode(opt.version)) +
                    "), " + summaryLine(report));
    }
};

// IMPORTDXF: reads a DXF file and adds its contents to the open drawing (one undo step).
class ImportDxf final : public cmd::Command {
public:
    std::string_view name() const override { return "IMPORTDXF"; }
    std::string_view description() const override { return "Add the contents of a DXF file"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto path = co_await ctx.getString("Enter DXF file name");
        if (!path.ok())
            co_return;

        std::ifstream f(pathOf(path.text), std::ios::binary);
        if (!f) {
            ctx.message("Cannot open " + path.text);
            co_return;
        }
        std::vector<std::byte> bytes;
        {
            std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            bytes.resize(raw.size());
            std::memcpy(bytes.data(), raw.data(), raw.size());
        }
        dxf::Report report;
        auto res = dxf::importDxf(bytes, {}, report);
        if (!res) {
            ctx.message("IMPORTDXF failed: " + res.error().message);
            co_return;
        }
        auto merged = mergeDocument(ctx.doc(), ctx.tx(), *res.value().document);
        if (!merged) {
            ctx.message("IMPORTDXF failed: " + merged.error().message);
            co_return;
        }
        ctx.message("Imported " + std::to_string(merged.value().entities) + " object(s) from " +
                    path.text + ", " + summaryLine(report));
    }
};

}  // namespace

void registerCommands(cmd::Registry& registry) {
    (void)registry.add(std::make_unique<ExportDxf>());
    (void)registry.add(std::make_unique<ImportDxf>());
}

}  // namespace tbcad::dxfexport
