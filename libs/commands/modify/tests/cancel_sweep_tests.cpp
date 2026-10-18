#include <doctest/doctest.h>

#include <cctype>
#include <string>
#include <vector>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/core/dump.h"

using namespace tbcad;
using cmd::ScriptItem;

namespace {

struct Rig {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    EntityId a, b, c, p;
    Rig() : host(doc, reg) {
        registerAllCommands(reg);
        a = add(LineData{{0, 0}, {20, 0}});
        b = add(LineData{{0, 0}, {0, 20}});
        c = add(CircleData{{40, 10}, 6});
        PolylineData pl;
        pl.v = {{{60, 0}, 0, 0, 0}, {{80, 0}, 0.4, 0, 0}, {{80, 20}, 0, 0, 0}, {{60, 20}, 0, 0, 0}};
        pl.closed = true;
        p = add(pl);
    }
    EntityId add(EntityData d) {
        Entity e;
        e.data = std::move(d);
        Transaction tx = doc.begin("setup");
        auto id = tx.add(std::move(e));
        REQUIRE(id.ok());
        tx.commit();
        return id.value();
    }
};

bool finite(const std::string& dump) {
    std::string low = dump;
    for (char& ch : low)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return low.find("nan") == std::string::npos && low.find("inf") == std::string::npos;
}

}  // namespace

// Esc or Enter at every prompt of a command must end it cleanly: whatever it made so far is
// valid and finite, and undoing everything gives the drawing back exactly as it was.
TEST_CASE(
    "Esc or Enter at any prompt of the edit commands leaves a valid, fully undoable drawing") {
    struct Case {
        const char* cmd;
        std::vector<ScriptItem> (*script)(const Rig&);
    };
    const Case cases[] = {
        {"OFFSET",
         [](const Rig& r) {
             return std::vector<ScriptItem>{"3", ScriptItem::pick(r.a, {5, 0}), "5,9",
                                            ScriptItem::pick(r.c, {46, 10}), "40,25"};
         }},
        {"TRIM",
         [](const Rig& r) {
             return std::vector<ScriptItem>{ScriptItem::select({r.b}), ScriptItem::enter(),
                                            ScriptItem::pick(r.a, {15, 0}), ScriptItem::enter()};
         }},
        {"EXTEND",
         [](const Rig& r) {
             return std::vector<ScriptItem>{ScriptItem::select({r.b}), ScriptItem::enter(),
                                            ScriptItem::pick(r.a, {2, 0}), ScriptItem::enter()};
         }},
        {"FILLET",
         [](const Rig& r) {
             return std::vector<ScriptItem>{"R", "4", ScriptItem::pick(r.a, {10, 0}),
                                            ScriptItem::pick(r.b, {0, 10})};
         }},
        {"CHAMFER",
         [](const Rig& r) {
             return std::vector<ScriptItem>{"D", "3", "3", ScriptItem::pick(r.a, {10, 0}),
                                            ScriptItem::pick(r.b, {0, 10})};
         }},
        {"BREAK",
         [](const Rig& r) {
             return std::vector<ScriptItem>{ScriptItem::pick(r.a, {4, 0}), "12,0"};
         }},
        {"LENGTHEN",
         [](const Rig& r) {
             return std::vector<ScriptItem>{"DE", "5", ScriptItem::pick(r.a, {18, 0}),
                                            ScriptItem::enter()};
         }},
        {"MIRROR",
         [](const Rig& r) {
             return std::vector<ScriptItem>{ScriptItem::select({r.a, r.c}), ScriptItem::enter(),
                                            "0,-5", "10,-5", "N"};
         }},
        {"ROTATE",
         [](const Rig& r) {
             return std::vector<ScriptItem>{ScriptItem::select({r.p}), ScriptItem::enter(), "70,10",
                                            "30"};
         }},
        {"SCALE",
         [](const Rig& r) {
             return std::vector<ScriptItem>{ScriptItem::select({r.c}), ScriptItem::enter(), "40,10",
                                            "2"};
         }},
        {"MOVE",
         [](const Rig& r) {
             return std::vector<ScriptItem>{ScriptItem::select({r.a, r.b}), ScriptItem::enter(),
                                            "0,0", "10,10"};
         }},
        {"COPY",
         [](const Rig& r) {
             return std::vector<ScriptItem>{ScriptItem::select({r.a, r.b}),
                                            ScriptItem::enter(),
                                            "0,0",
                                            "10,10",
                                            "20,20",
                                            ScriptItem::enter()};
         }},
        {"ERASE",
         [](const Rig& r) {
             return std::vector<ScriptItem>{ScriptItem::select({r.a, r.c}), ScriptItem::enter()};
         }},
    };
    int changedRuns = 0;
    for (const Case& cs : cases) {
        const std::size_t n = [&] {
            Rig probe;
            return cs.script(probe).size();
        }();
        for (std::size_t k = 0; k <= n; ++k) {
            for (int ending = 0; ending < 2; ++ending) {
                CAPTURE(cs.cmd);
                CAPTURE(k);
                CAPTURE(ending);
                Rig r;
                const std::string before = canonicalDump(r.doc);
                std::vector<ScriptItem> all = cs.script(r);
                std::vector<ScriptItem> items(all.begin(), all.begin() + static_cast<long>(k));
                items.push_back(ending == 0 ? ScriptItem::esc() : ScriptItem::enter());
                items.push_back(ScriptItem::esc());  // a second Esc must be harmless
                auto res = r.host.run(cs.cmd, items);
                CHECK(res.finished);
                CHECK(finite(canonicalDump(r.doc)));
                changedRuns += canonicalDump(r.doc) != before ? 1 : 0;
                for (int i = 0; i < 20 && r.host.undo(); ++i) {
                }
                CHECK(canonicalDump(r.doc) == before);
            }
        }
    }
    CHECK(changedRuns >= 10);  // the sweep really edits the drawing before it cancels
}

// Typing nonsense where a point, number or keyword is asked must be refused or ignored, never
// crash, corrupt the drawing or leave the command stuck.
TEST_CASE("nonsense typed at any prompt of the edit commands is harmless") {
    const char* garbage[] = {"abc", "1,",  "@",  "<45",     ",",  "1e999",
                             "-0",  "nan", "  ", "0,0,0,0", "R R"};
    const char* names[] = {"OFFSET", "TRIM", "EXTEND", "FILLET", "CHAMFER", "BREAK", "LENGTHEN"};
    for (const char* name : names) {
        for (const char* g : garbage) {
            for (int at = 0; at < 3; ++at) {
                CAPTURE(name);
                CAPTURE(g);
                CAPTURE(at);
                Rig r;
                const std::string before = canonicalDump(r.doc);
                std::vector<ScriptItem> items;
                for (int i = 0; i < at; ++i)
                    items.push_back(ScriptItem::enter());
                items.push_back(g);
                items.push_back(ScriptItem::pick(r.a, {5, 0}));
                items.push_back(g);
                items.push_back(ScriptItem::esc());
                items.push_back(ScriptItem::esc());
                auto res = r.host.run(name, items);
                CHECK(res.finished);
                CHECK(finite(canonicalDump(r.doc)));
                for (int i = 0; i < 20 && r.host.undo(); ++i) {
                }
                CHECK(canonicalDump(r.doc) == before);
            }
        }
    }
}
