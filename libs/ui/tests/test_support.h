#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "tbcad/cmd/command.h"
#include "tbcad/cmd/registry.h"
#include "tbcad/core/core.h"
#include "tbcad/geom/entity.h"
#include "tbcad/index/document_indexer.h"
#include "tbcad/index/spatial_index.h"

namespace tbcad::ui::test {

// TESTLINE: two points make a line; used instead of a real drawing command.
class TestLine final : public cmd::Command {
public:
    std::string_view name() const override { return "TESTLINE"; }
    std::vector<std::string> defaultAliases() const override { return {"TL"}; }
    std::string_view description() const override { return "test"; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        auto a = co_await ctx.getPoint("First point");
        if (!a.ok())
            co_return;
        cmd::PointOptions o;
        o.base = a.point;
        o.preview = [from = a.point](Vec2 c, DisplayList& out) {
            DisplayPolyline pl;
            pl.pts = {from, c};
            out.items.emplace_back(std::move(pl));
        };
        auto b = co_await ctx.getPoint("Second point", o);
        if (!b.ok())
            co_return;
        (void)ctx.tx().add(Entity{{}, LineData{a.point, b.point}});
    }
};

// TESTSEL: needs a selection and erases it.
class TestErase final : public cmd::Command {
public:
    std::string_view name() const override { return "TESTERASE"; }
    std::string_view description() const override { return "test"; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        auto sel = co_await ctx.getSelection("Select objects");
        if (!sel.ok())
            co_return;
        for (EntityId id : sel.ids)
            (void)ctx.tx().erase(id);
    }
};

inline std::unique_ptr<cmd::Registry> makeRegistry() {
    auto r = std::make_unique<cmd::Registry>();
    cmd::registerBuiltinCommands(*r);
    (void)r->add(std::make_unique<TestLine>());
    (void)r->add(std::make_unique<TestErase>());
    return r;
}

inline EntityId addLine(Document& doc, Vec2 a, Vec2 b, LayerId layer = {}) {
    auto tx = doc.begin("line");
    Entity e{{}, LineData{a, b}};
    e.common.layer = layer;
    auto id = tx.add(std::move(e));
    tx.commit();
    return id.value();
}

inline LayerId addLayer(Document& doc, const std::string& name, LayerId parent = {}) {
    auto tx = doc.begin("layer");
    Layer l;
    l.name = name;
    l.parent = parent;
    if (parent)
        l.color = Color::byParent();
    auto id = tx.addLayer(l);
    tx.commit();
    return id.value();
}

inline void modifyLayer(Document& doc, LayerId id, const std::function<void(Layer&)>& fn) {
    auto tx = doc.begin("layer");
    (void)tx.modifyLayer(id, fn);
    tx.commit();
}

struct Indexed {
    Document doc;
    index::SpatialIndex idx;
    std::unique_ptr<index::DocumentIndexer> indexer;
    Indexed() {
        indexer = std::make_unique<index::DocumentIndexer>(
            doc, idx, [this](const Entity& e) { return geom::bounds(e, doc); });
    }
};

inline std::filesystem::path tempDir(const std::string& name) {
    auto d = std::filesystem::temp_directory_path() / ("tbcad_ui_" + name);
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d;
}

}  // namespace tbcad::ui::test
