#include <doctest/doctest.h>

#include <chrono>
#include <fstream>

#include "tbcad/tmpl/tmpl.h"

using namespace tbcad;
namespace fs = std::filesystem;

namespace {
struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
               (std::string("tbcad-tmpl-") +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};
}  // namespace

TEST_CASE("the built-in template has the standard layers, linetypes and styles") {
    auto doc = tmpl::builtinTemplate();
    REQUIRE(doc);
    CHECK(doc->layerByName("0"));
    for (const char* n : {"Defpoints", "Walls", "Dimensions", "Text", "Hatch", "Centerlines",
                          "Hidden", "Construction"})
        CHECK_MESSAGE(doc->layerByName(n), n);
    CHECK_FALSE(doc->layerByName("Defpoints")->plot);
    CHECK_FALSE(doc->layerByName("Construction")->plot);
    CHECK(doc->layerByName("Centerlines")->linetype == doc->linetypeByName("Center")->id);
    CHECK(doc->linetypeByName("Dashed"));
    CHECK(doc->textStyleByName("Notes"));
    CHECK(doc->dimStyleByName("Architectural")->arrowType == ArrowType::ArchTick);
    CHECK(doc->dimStyleByName("Standard")->arrowType == ArrowType::ClosedFilled);
    CHECK(doc->header().units == LengthUnit::Millimeter);
    CHECK(doc->header().currentLayer == kLayerZero);
}

TEST_CASE(
    "the built-in template: whole-number dimensions, and dimensions, hatches and text on their "
    "layers") {
    auto doc = tmpl::builtinTemplate();
    const DimStyle* std = doc->dimStyle(kDimStyleStandard);
    REQUIRE(std);
    CHECK(std->precision == 0);
    CHECK(std->textPlacement == DimTextPlacement::AboveLine);
    for (auto [key, name] : {std::pair{kDimLayerKey, "Dimensions"},
                             {kHatchLayerKey, "Hatch"},
                             {kTextLayerKey, "Text"}}) {
        Entity e;
        e.data = key == std::string(kDimLayerKey)     ? EntityData(LeaderData{})
                 : key == std::string(kHatchLayerKey) ? EntityData(Indirect<HatchData>(HatchData{}))
                                                      : EntityData(Indirect<TextData>(TextData{}));
        applyCurrentProperties(doc->header(), e);
        const Layer* l = doc->layer(e.common.layer);
        REQUIRE(l);
        CHECK(l->name == name);
    }
}

TEST_CASE("saving and loading a template keeps rows and entities") {
    TempDir dir;
    auto doc = tmpl::builtinTemplate();
    {
        Transaction tx = doc->begin("add");
        Entity e;
        e.data = LineData{{0, 0}, {10, 0}};
        (void)tx.add(std::move(e));
        (void)tx.commit();
    }
    auto saved = tmpl::saveTemplate(*doc, dir.path / "Office");
    REQUIRE(saved);
    CHECK(saved.value().filename() == "Office.tbt");
    auto loaded = tmpl::loadTemplate(saved.value());
    REQUIRE(loaded);
    CHECK(loaded.value()->layerByName("Walls"));
    CHECK(loaded.value()->entityCount() == 1);
}

TEST_CASE("listTemplates finds only .tbt files, sorted, and survives a missing folder") {
    TempDir dir;
    for (const char* n : {"b.tbt", "A.TBT", "c.tbcad", "d.txt"})
        std::ofstream(dir.path / n) << "x";
    auto list = tmpl::listTemplates(dir.path);
    REQUIRE(list.size() == 2);
    CHECK(list[0].name == "A");
    CHECK(list[1].name == "b");
    CHECK(tmpl::listTemplates(dir.path / "nope").empty());
}

TEST_CASE("template names are made safe") {
    CHECK(tmpl::sanitizeTemplateName("Plan / 1:100?") == "Plan _ 1_100_");
    CHECK(tmpl::sanitizeTemplateName("  ") == "template");
    CHECK(tmpl::sanitizeTemplateName("แบบบ้าน") == "แบบบ้าน");
    CHECK(tmpl::isTemplatePath("x.TBT"));
    CHECK_FALSE(tmpl::isTemplatePath("x.tbcad"));
}

TEST_CASE("loading a missing or corrupt template reports an error") {
    TempDir dir;
    CHECK_FALSE(tmpl::loadTemplate(dir.path / "none.tbt"));
    std::ofstream(dir.path / "bad.tbt") << "not a drawing";
    CHECK_FALSE(tmpl::loadTemplate(dir.path / "bad.tbt"));
}
