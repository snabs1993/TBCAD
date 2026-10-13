#include <algorithm>
#include <set>

#include "common.h"

namespace tbcad::select {

namespace {

constexpr const char* kKey = "isolate.hidden";

std::string join(const std::vector<EntityId>& ids) {
    std::string s;
    for (EntityId id : ids)
        s += (s.empty() ? "" : ",") + std::to_string(id.value);
    return s;
}

void remember(Transaction& tx, const std::vector<EntityId>& ids) {
    (void)tx.modifyHeader([&](Header& h) {
        if (ids.empty())
            h.custom.erase(kKey);
        else
            h.custom[kKey] = join(ids);
    });
}

// Shows the remembered objects again; returns how many.
int showHidden(cmd::CommandContext& ctx) {
    int n = 0;
    for (EntityId id : hiddenByIsolate(ctx.doc())) {
        const Entity* e = ctx.doc().find(id);
        if (!e || e->common.visible)
            continue;
        if (ctx.tx().modify(id, [](Entity& x) { x.common.visible = true; }))
            ++n;
    }
    remember(ctx.tx(), {});
    return n;
}

cmd::Task chooseObjects(cmd::CommandContext& ctx, std::vector<EntityId>& out, bool& ok) {
    auto sel = co_await ctx.getSelection("Select objects");
    ok = sel.ok() && !sel.ids.empty();
    if (ok)
        out = sel.ids;
}

}  // namespace

std::vector<EntityId> hiddenByIsolate(const Document& doc) {
    std::vector<EntityId> out;
    auto it = doc.header().custom.find(kKey);
    if (it == doc.header().custom.end())
        return out;
    const std::string& s = it->second;
    std::size_t pos = 0;
    while (pos < s.size()) {
        std::size_t end = s.find(',', pos);
        if (end == std::string::npos)
            end = s.size();
        try {
            out.push_back(EntityId{std::stoull(s.substr(pos, end - pos))});
        } catch (...) {
        }
        pos = end + 1;
    }
    return out;
}

namespace detail {
namespace {

// HIDE: hides the picked objects until UNISOLATE.
class Hide final : public cmd::Command {
public:
    std::string_view name() const override { return "HIDE"; }
    std::vector<std::string> defaultAliases() const override { return {"HIDEOBJECTS"}; }
    std::string_view description() const override { return "Hide the selected objects"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        std::vector<EntityId> picked;
        bool ok = false;
        co_await chooseObjects(ctx, picked, ok);
        if (!ok)
            co_return;
        std::vector<EntityId> hidden = hiddenByIsolate(ctx.doc());
        int n = 0;
        for (EntityId id : picked) {
            const Entity* e = ctx.doc().find(id);
            if (!e || !e->common.visible)
                continue;
            if (ctx.tx().modify(id, [](Entity& x) { x.common.visible = false; })) {
                hidden.push_back(id);
                ++n;
            }
        }
        remember(ctx.tx(), hidden);
        ctx.host().setSelection({});
        ctx.message(std::to_string(n) + " object(s) hidden. UNISOLATE shows them again.");
    }
};

// ISOLATE: keeps only the picked objects shown.
class Isolate final : public cmd::Command {
public:
    std::string_view name() const override { return "ISOLATE"; }
    std::vector<std::string> defaultAliases() const override { return {"ISOLATEOBJECTS"}; }
    std::string_view description() const override {
        return "Show only the selected objects and hide the rest";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        std::vector<EntityId> picked;
        bool ok = false;
        co_await chooseObjects(ctx, picked, ok);
        if (!ok)
            co_return;
        showHidden(ctx);  // start from the whole drawing, not from an earlier isolation
        const std::set<EntityId> keep(picked.begin(), picked.end());
        std::vector<EntityId> hidden;
        for (EntityId id : ctx.doc().modelSpace()) {
            const Entity* e = ctx.doc().find(id);
            if (!e || !e->common.visible || keep.count(id))
                continue;
            if (ctx.tx().modify(id, [](Entity& x) { x.common.visible = false; }))
                hidden.push_back(id);
        }
        remember(ctx.tx(), hidden);
        ctx.message(std::to_string(hidden.size()) +
                    " object(s) hidden. UNISOLATE shows them again.");
    }
};

// UNISOLATE: shows what HIDE and ISOLATE hid.
class Unisolate final : public cmd::Command {
public:
    std::string_view name() const override { return "UNISOLATE"; }
    std::vector<std::string> defaultAliases() const override {
        return {"UNHIDE", "UNISOLATEOBJECTS"};
    }
    std::string_view description() const override {
        return "Show the objects hidden by HIDE or ISOLATE";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        if (hiddenByIsolate(ctx.doc()).empty()) {
            ctx.message("No objects are hidden by HIDE or ISOLATE.");
            co_return;
        }
        const int n = showHidden(ctx);
        ctx.message(std::to_string(n) + " object(s) shown.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeHide() {
    return std::make_unique<Hide>();
}
std::unique_ptr<cmd::Command> makeIsolate() {
    return std::make_unique<Isolate>();
}
std::unique_ptr<cmd::Command> makeUnisolate() {
    return std::make_unique<Unisolate>();
}

}  // namespace detail
}  // namespace tbcad::select
