// Random DIMSTYLE sessions with dimensions drawn in between: every step is one undo entry that
// walks back and forward exactly, bad values are refused without changing anything, and the
// result survives .tbcad and DXF round trips.
#include <doctest/doctest.h>

#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/core/dump.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"
#include "tbcad/io/tbcad.h"

using namespace tbcad;
using cmd::ScriptItem;

namespace {

std::string pt(std::mt19937& rng, int range) {
    return std::to_string(static_cast<int>(rng() % (2 * range)) - range) + "," +
           std::to_string(static_cast<int>(rng() % (2 * range)) - range);
}

// A property and a value that may or may not be acceptable.
std::pair<std::string, std::string> randomProperty(std::mt19937& rng) {
    static const std::vector<std::pair<std::string, std::vector<std::string>>> kProps = {
        {"TextHeight", {"3.5", "0", "-1", "abc", "250", "0.001"}},
        {"TextGap", {"0.5", "0", "-2", "x"}},
        {"ArrowType", {"Filled", "Open", "Tick", "Dot", "None", "Star"}},
        {"ArrowSize", {"2", "0", "10", "-3"}},
        {"ExtOffset", {"0", "1.5", "-1"}},
        {"ExtExtension", {"0", "2"}},
        {"LineExtension", {"0", "1"}},
        {"Placement", {"Above", "Centered", "Below"}},
        {"Alignment", {"WithLine", "Horizontal", "Vertical"}},
        {"Units", {"mm", "cm", "m", "in", "ft", "km"}},
        {"Precision", {"0", "3", "8", "9", "-1", "1.5"}},
        {"Prefix", {"~_", "none", "R=", "ระยะ_"}},
        {"Suffix", {"_mm", "none", "มม."}},
        {"RoundOff", {"0", "0.5", "5"}},
        {"Scale", {"1", "50", "0", "0.01", "-2"}},
        {"LeadingZeros", {"Suppress", "Show", "maybe"}},
        {"TrailingZeros", {"Suppress", "Show"}},
        {"LineColor", {"ByBlock", "ByLayer", "1", "255", "10,20,30", "256", "1,2"}},
        {"ExtColor", {"3", "ByLayer"}},
        {"TextColor", {"7", "0,255,0"}},
    };
    const auto& p = kProps[rng() % kProps.size()];
    return {p.first, p.second[rng() % p.second.size()]};
}

}  // namespace

TEST_CASE("DIMSTYLE sessions with dimensions undo, redo and round trip") {
    for (unsigned seed : {5u, 17u, 41u, 83u}) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        Document doc;
        cmd::Registry reg;
        registerAllCommands(reg);
        cmd::ScriptHost host(doc, reg);

        std::vector<std::string> states{canonicalDump(doc)};
        std::vector<std::string> names;
        std::vector<std::string> styleNames{"Standard"};
        for (int step = 0; step < 50; ++step) {
            const std::size_t before = host.undoStack().undoCount();
            std::string name;
            switch (rng() % 6) {
                case 0: {
                    name = "S" + std::to_string(step);
                    (void)host.run("DIMSTYLE", {"New", name, "Done"});
                    styleNames.push_back(name);
                    break;
                }
                case 1:
                case 2: {
                    std::vector<cmd::ScriptItem> items{"Edit",
                                                       styleNames[rng() % styleNames.size()]};
                    for (int k = 0, n = 1 + static_cast<int>(rng() % 4); k < n; ++k) {
                        auto [key, value] = randomProperty(rng);
                        items.push_back(key);
                        items.push_back(value);
                    }
                    items.push_back("Done");
                    items.push_back("Done");
                    name = "Edit";
                    (void)host.run("DIMSTYLE", items);
                    break;
                }
                case 3:
                    name = "Rename";
                    (void)host.run("DIMSTYLE", {"Rename", styleNames[rng() % styleNames.size()],
                                                "R" + std::to_string(step), "Done"});
                    break;
                case 4:
                    name = "Delete";
                    (void)host.run("DIMSTYLE",
                                   {"Delete", styleNames[rng() % styleNames.size()], "Current",
                                    styleNames[rng() % styleNames.size()], "Done"});
                    break;
                default:
                    name = "DIMLINEAR";
                    (void)host.run("DIMLINEAR", {pt(rng, 500), pt(rng, 500), pt(rng, 500)});
            }
            (void)host.feed({ScriptItem::esc(), ScriptItem::esc()});
            // Names may have changed or vanished; ask the document.
            styleNames.clear();
            for (const auto& [id, s] : doc.dimStyles())
                styleNames.push_back(s.name);
            const std::size_t after = host.undoStack().undoCount();
            const std::string now = canonicalDump(doc);
            INFO("step " << step << " " << name);
            if (after == before) {
                CHECK(now == states.back());
            } else {
                CHECK(after == before + 1);
                states.push_back(now);
                names.push_back(name);
            }
        }
        CHECK(states.size() > 10);
        const std::string endState = states.back();

        // Round trips of the final drawing.
        const auto file = std::filesystem::temp_directory_path() /
                          ("tbcad_dimstyle_" + std::to_string(seed) + ".tbcad");
        REQUIRE(io::writeTbcad(doc, file).ok());
        io::IoReport ir;
        auto back = io::readTbcad(file, ir);
        REQUIRE(back.ok());
        CHECK(canonicalDump(*back.value()) == endState);
        std::error_code ec;
        std::filesystem::remove(file, ec);

        dxf::Report er;
        auto out = dxf::exportDxf(doc, dxf::ExportOptions{}, er);
        REQUIRE(out.ok());
        const std::string& text = out.value().bytes;
        std::vector<std::byte> bytes(text.size());
        for (std::size_t i = 0; i < text.size(); ++i)
            bytes[i] = static_cast<std::byte>(text[i]);
        dxf::Report imr;
        auto in = dxf::importDxf(bytes, dxf::ImportOptions{}, imr);
        REQUIRE_MESSAGE(in.ok(), (in.ok() ? "" : in.error().message));
        std::size_t dimsBefore = 0, dimsAfter = 0;
        doc.forEachEntity(
            [&](const Entity& e) { dimsBefore += entityAs<DimensionData>(e) != nullptr; });
        in.value().document->forEachEntity(
            [&](const Entity& e) { dimsAfter += entityAs<DimensionData>(e) != nullptr; });
        CHECK(dimsAfter == dimsBefore);
        for (const auto& [id, s] : doc.dimStyles()) {
            INFO("style " << s.name);
            const DimStyle* t = in.value().document->dimStyleByName(s.name);
            REQUIRE(t);
            CHECK(t->textHeight == doctest::Approx(s.textHeight));
            CHECK(t->arrowSize == doctest::Approx(s.arrowSize));
            CHECK(t->precision == s.precision);
            CHECK(t->prefix == s.prefix);
            CHECK(t->suffix == s.suffix);
            CHECK(t->overallScale == doctest::Approx(s.overallScale));
            CHECK(t->roundOff == doctest::Approx(s.roundOff));
            CHECK(t->textAlign == s.textAlign);
            CHECK(t->textPlacement == s.textPlacement);
            CHECK(t->suppressLeadingZeros == s.suppressLeadingZeros);
            CHECK(t->suppressTrailingZeros == s.suppressTrailingZeros);
            // True colours become the nearest index (DXF dimension styles hold no RGB).
            if (s.dimLineColor.kind != ColorKind::Rgb)
                CHECK(t->dimLineColor.kind == s.dimLineColor.kind);
            // True colours become the nearest index (DXF dimension styles hold no RGB).
            if (s.extLineColor.kind != ColorKind::Rgb)
                CHECK(t->extLineColor.kind == s.extLineColor.kind);
            // True colours become the nearest index (DXF dimension styles hold no RGB).
            if (s.textColor.kind != ColorKind::Rgb)
                CHECK(t->textColor.kind == s.textColor.kind);
        }

        // Undo back to the start and forward again.
        for (std::size_t i = states.size() - 1; i > 0; --i) {
            INFO("undoing " << names[i - 1]);
            REQUIRE(host.undo());
            CHECK(canonicalDump(doc) == states[i - 1]);
        }
        for (std::size_t i = 1; i < states.size(); ++i) {
            REQUIRE(host.redo());
            CHECK(canonicalDump(doc) == states[i]);
        }
        CHECK(canonicalDump(doc) == endState);
    }
}
