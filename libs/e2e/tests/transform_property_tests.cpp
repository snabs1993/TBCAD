// MOVE, ROTATE, SCALE and MIRROR on the hard object types (hatch with islands, MTEXT, attributes,
// nested and non-uniformly scaled blocks, leaders, multileaders, splines): the drawing stays
// finite, the inverse operation brings it back, undo is exact and a file round trip after every
// edit keeps everything.
#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/core/dump.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/transform/grips.h"

using namespace tbcad;

namespace {

// Two dumps are equal when every number agrees to a relative tolerance and the text around the
// numbers is identical.
bool sameDump(const std::string& a, const std::string& b, double tol, std::string* why = nullptr) {
    std::size_t i = 0, j = 0;
    auto isNumStart = [](const std::string& s, std::size_t k) {
        const char c = s[k];
        if (std::isdigit(static_cast<unsigned char>(c)))
            return true;
        return (c == '-' || c == '+' || c == '.') && k + 1 < s.size() &&
               (std::isdigit(static_cast<unsigned char>(s[k + 1])) || s[k + 1] == '.');
    };
    while (i < a.size() && j < b.size()) {
        if (isNumStart(a, i) && isNumStart(b, j)) {
            char* ea = nullptr;
            char* eb = nullptr;
            const double x = std::strtod(a.c_str() + i, &ea);
            const double y = std::strtod(b.c_str() + j, &eb);
            if (ea == a.c_str() + i || eb == b.c_str() + j) {
                if (a[i] != b[j])
                    goto fail;
                ++i;
                ++j;
                continue;
            }
            // Angles may differ by a full turn; compare their sine and cosine instead.
            const double scale = 1.0 + std::max(std::abs(x), std::abs(y));
            if (std::abs(x - y) > tol * scale &&
                std::abs(std::abs(x - y) - 2 * 3.14159265358979323846) > tol * scale)
                goto fail;
            i = static_cast<std::size_t>(ea - a.c_str());
            j = static_cast<std::size_t>(eb - b.c_str());
            continue;
        }
        if (a[i] != b[j])
            goto fail;
        ++i;
        ++j;
    }
    if (i == a.size() && j == b.size())
        return true;
fail:
    if (why) {
        const std::size_t from = i > 60 ? i - 60 : 0;
        *why = "a: ..." + a.substr(from, 140) + "\nb: ..." + b.substr(j > 60 ? j - 60 : 0, 140);
    }
    return false;
}

bool hasNonFinite(const std::string& dump) {
    std::string low = dump;
    for (auto& c : low)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return low.find("nan") != std::string::npos || low.find("inf") != std::string::npos;
}

// The dump's entity lines ("e15 L1 ...") without their ids, sorted.
std::string withoutIds(const std::string& dump) {
    std::vector<std::string> lines;
    std::size_t from = 0;
    while (from < dump.size()) {
        std::size_t to = dump.find('\n', from);
        if (to == std::string::npos)
            to = dump.size();
        std::string line = dump.substr(from, to - from);
        if (line.size() > 1 && line[0] == 'e' && std::isdigit(static_cast<unsigned char>(line[1])))
            line.erase(0, line.find(' ') + 1);
        lines.push_back(std::move(line));
        from = to + 1;
    }
    std::sort(lines.begin(), lines.end());
    std::string out;
    for (const auto& l : lines)
        out += l + "\n";
    return out;
}

struct Rig {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    std::vector<EntityId> ids;  // model space objects to edit

    EntityId add(EntityData data, BlockId owner = {}) {
        auto tx = doc.begin("setup");
        auto id = tx.add(Entity{{}, std::move(data)}, owner);
        REQUIRE(id.ok());
        tx.commit();
        return id.value();
    }
    EntityId addTop(EntityData data) {
        ids.push_back(add(std::move(data)));
        return ids.back();
    }
    BlockId block(const char* name) {
        auto tx = doc.begin("block");
        BlockDef def;
        def.name = name;
        auto b = tx.addBlock(def);
        REQUIRE(b.ok());
        tx.commit();
        return b.value();
    }
    InsertData insert(BlockId b, Vec2 pos, Vec2 scale, double rot) {
        InsertData i;
        i.block = b;
        i.position = pos;
        i.scale = scale;
        i.rotation = rot;
        return i;
    }

    Rig() : host(doc, (registerAllCommands(reg), reg)) {
        // A hatch with two islands made of different edge types.
        HatchData h;
        HatchLoop outer;
        outer.edges = {HatchLineEdge{{0, 0}, {60, 0}}, HatchLineEdge{{60, 0}, {60, 40}},
                       HatchLineEdge{{60, 40}, {0, 40}}, HatchLineEdge{{0, 40}, {0, 0}}};
        HatchLoop islandA;
        islandA.outer = false;
        islandA.edges = {HatchArcEdge{{15, 20}, 6, 0, kPi, true},
                         HatchArcEdge{{15, 20}, 6, kPi, kTwoPi, true}};
        HatchLoop islandB;
        islandB.outer = false;
        HatchPolylineEdge pe;
        pe.v = {
            {{35, 10}, 0.3, 0, 0}, {{50, 10}, 0, 0, 0}, {{50, 30}, 0, 0, 0}, {{35, 30}, 0, 0, 0}};
        islandB.edges = {pe};
        h.loops = {outer, islandA, islandB};
        h.solid = false;
        h.pattern = "ANSI31";
        h.patternScale = 1.5;
        h.patternAngle = 0.3;
        h.patternOrigin = {3, 4};
        addTop(Indirect<HatchData>(h));
        HatchData grad;
        grad.loops = {outer};
        grad.gradient = GradientData{};
        addTop(Indirect<HatchData>(grad));

        MTextData mt;
        mt.position = {5, 60};
        mt.height = 3;
        mt.width = 40;
        mt.rotation = 0.4;
        mt.attach = Attachment::MiddleCenter;
        mt.utf8 = "{\\b1;Bold} text\\Psecond \\S1/2; line";
        addTop(Indirect<MTextData>(mt));
        TextData tx;
        tx.position = {5, 70};
        tx.alignPoint = {5, 70};
        tx.height = 2;
        tx.rotation = 0.2;
        tx.oblique = 0.1;
        tx.widthFactor = 0.8;
        tx.utf8 = "plain";
        addTop(Indirect<TextData>(tx));

        // Blocks: LEAF has an attribute definition, NEST inserts LEAF non-uniformly.
        const BlockId leaf = block("LEAF");
        add(LineData{{0, 0}, {10, 0}}, leaf);
        add(CircleData{{5, 3}, 2}, leaf);
        add(ArcData{{0, 0}, 4, 0.2, 2.0}, leaf);
        TextData ad;
        ad.position = {1, 5};
        ad.alignPoint = {1, 5};
        ad.height = 1.5;
        ad.utf8 = "TAG";
        AttDefInfo info;
        info.tag = "TAG";
        info.prompt = "tag";
        info.defaultValue = "dflt";
        ad.attdef = info;
        add(Indirect<TextData>(ad), leaf);
        const BlockId nest = block("NEST");
        add(LineData{{0, 0}, {0, 20}}, nest);
        add(Indirect<InsertData>(insert(leaf, {3, 2}, {2, 0.5}, 0.7)), nest);
        add(Indirect<InsertData>(insert(leaf, {20, 2}, {-1, 1}, 0)), nest);

        for (const auto& [pos, sc, rot] :
             std::vector<std::tuple<Vec2, Vec2, double>>{{{80, 0}, {1, 1}, 0},
                                                         {{100, 0}, {2, 0.5}, 0.5},
                                                         {{120, 0}, {-1, 1}, 0},
                                                         {{140, 0}, {1, -2}, 1.0},
                                                         {{160, 0}, {-1.5, -1.5}, 2.0}}) {
            InsertData i = insert(leaf, pos, sc, rot);
            i.attribs = {AttribValue{"TAG", "val"}};
            addTop(Indirect<InsertData>(i));
        }
        addTop(Indirect<InsertData>(insert(nest, {80, 40}, {1.5, 0.75}, 0.3)));
        addTop(Indirect<InsertData>(insert(nest, {120, 40}, {-1, 2}, 1.2)));
        addTop(Indirect<InsertData>(insert(nest, {160, 40}, {-0.5, -0.5}, 0)));

        SplineData sp;
        sp.degree = 3;
        sp.control = {{0, 100}, {10, 120}, {25, 95}, {40, 125}, {55, 100}};
        sp.knots = {0, 0, 0, 0, 0.5, 1, 1, 1, 1};
        addTop(Indirect<SplineData>(sp));
        SplineData fit;
        fit.degree = 3;
        fit.fit = {{70, 100}, {80, 115}, {95, 100}, {110, 118}};
        addTop(Indirect<SplineData>(fit));
        SplineData rat = sp;
        rat.weights = {1, 2, 1, 0.5, 1};
        for (auto& c : rat.control)
            c.x += 130;
        addTop(Indirect<SplineData>(rat));

        LeaderData ld;
        ld.vertices = {{0, 140}, {10, 150}, {20, 150}};
        ld.utf8 = "note";
        ld.textPos = {21, 150};
        addTop(Indirect<LeaderData>(ld));
        MLeaderData ml;
        ml.lines = {{{30, 140}, {40, 150}}, {{30, 160}, {40, 150}}};
        ml.landing = {40, 150};
        ml.contentPos = {50, 150};
        ml.utf8 = "multi\\Pleader";
        ml.textHeight = 2.5;
        addTop(Indirect<MLeaderData>(ml));
        MLeaderData mb = ml;
        mb.content = MLeaderData::Content::Block;
        mb.block = leaf;
        mb.blockScale = 1.5;
        mb.blockRotation = 0.5;
        for (auto& l : mb.lines)
            for (auto& p : l)
                p.x += 40;
        mb.landing.x += 40;
        mb.contentPos.x += 40;
        addTop(Indirect<MLeaderData>(mb));

        PolylineData pl;
        pl.v = {{{0, 170}, 0.4, 0, 0}, {{20, 170}, 0, 1, 2}, {{20, 190}, -0.7, 0, 0}};
        pl.closed = true;
        addTop(pl);
        EllipseData el;
        el.center = {60, 180};
        el.majorAxis = {10, 4};
        el.ratio = 0.5;
        el.startParam = 0.3;
        el.endParam = 4.0;
        addTop(Indirect<EllipseData>(el));
    }

    // diag(-1, 1) and rotate-by-pi diag(1, -1) are the same transform; write every insert with a
    // positive x scale so that two equal drawings compare equal.
    void canonInserts() {
        auto tx = doc.begin("canon");
        for (EntityId id : doc.modelSpace()) {
            (void)tx.modify(id, [](Entity& e) {
                if (auto* i = entityAs<InsertData>(e); i && i->scale.x < 0) {
                    i->scale = {-i->scale.x, -i->scale.y};
                    i->rotation = std::remainder(i->rotation + kPi, kTwoPi);
                }
            });
        }
        tx.commit();
    }
    std::string dump() const { return canonicalDump(doc); }
    void select() { host.setSelection(ids); }
    bool fileRoundTrips(std::string* why) {
        const auto file = std::filesystem::temp_directory_path() / "tbcad_transform_prop.tbcad";
        REQUIRE(io::writeTbcad(doc, file).ok());
        io::IoReport rep;
        auto back = io::readTbcad(file, rep);
        std::error_code ec;
        std::filesystem::remove(file, ec);
        REQUIRE(back.ok());
        const std::string a = canonicalDump(*back.value());
        const std::string b = dump();
        if (a == b)
            return true;
        if (why)
            (void)sameDump(a, b, 0, why);
        return false;
    }
};

std::string pt(double x, double y) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%.17g,%.17g", x, y);
    return buf;
}

}  // namespace

TEST_CASE(
    "every hard object type survives MOVE, ROTATE, SCALE and MIRROR: finite, undoable, saved") {
    struct Op {
        const char* name;
        std::vector<cmd::ScriptItem> in;
    };
    const Op ops[] = {
        {"MOVE", {"0,0", "13.5,-7.25"}},   {"ROTATE", {"3,4", "37"}},
        {"ROTATE", {"0,0", "90"}},         {"SCALE", {"1,1", "2.5"}},
        {"SCALE", {"10,10", "0.25"}},      {"MIRROR", {"0,0", "10,3", "No"}},
        {"MIRROR", {"5,5", "5,50", "No"}}, {"MIRROR", {"0,0", "10,0", "No"}},
    };
    for (const auto& op : ops) {
        CAPTURE(op.name);
        Rig r;
        const std::string before = r.dump();
        r.select();
        REQUIRE(r.host.run(op.name, op.in).finished);
        const std::string after = r.dump();
        CHECK_FALSE(hasNonFinite(after));
        CHECK(after != before);
        std::string why;
        CHECK_MESSAGE(r.fileRoundTrips(&why), why);
        REQUIRE(r.host.undo());
        CHECK(r.dump() == before);
        REQUIRE(r.host.redo());
        CHECK(r.dump() == after);
    }
}

TEST_CASE("the inverse operation brings every hard object type back") {
    struct Pair {
        const char* name;
        std::vector<cmd::ScriptItem> fwd, back;
    };
    const Pair pairs[] = {
        {"MOVE", {"0,0", "13.5,-7.25"}, {"13.5,-7.25", "0,0"}},
        {"ROTATE", {"3,4", "37"}, {"3,4", "-37"}},
        {"SCALE", {"10,10", "2.5"}, {"10,10", "0.4"}},
        {"SCALE", {"0,0", "0.125"}, {"0,0", "8"}},
        {"MIRROR", {"0,0", "10,3", "Yes"}, {"0,0", "10,3", "Yes"}},
        {"MIRROR", {"7,7", "7,60", "Yes"}, {"7,7", "7,60", "Yes"}},
    };
    for (const auto& p : pairs) {
        CAPTURE(p.name);
        Rig r;
        r.canonInserts();
        const std::string before = r.dump();
        r.select();
        REQUIRE(r.host.run(p.name, p.fwd).finished);
        CHECK(r.dump() != before);
        // MIRROR with Yes erases the sources: select what is there now.
        const std::vector<EntityId> now = r.doc.modelSpace();
        r.host.setSelection(now);
        REQUIRE(r.host.run(p.name, p.back).finished);
        r.canonInserts();
        std::string again = r.dump();
        std::string why;
        // A mirrored drawing gets new ids: compare the entities without them.
        if (std::string(p.name) == "MIRROR") {
            CHECK_MESSAGE(sameDump(withoutIds(before), withoutIds(again), 1e-7, &why), p.name, "\n",
                          why);
            continue;
        }
        CHECK_MESSAGE(sameDump(before, again, 1e-7, &why), p.name, "\n", why);
    }
}

TEST_CASE("random sequences of edits keep the drawing finite and every step undoes exactly") {
    std::mt19937 rng(20261018);
    auto num = [&](double lo, double hi) {
        return std::uniform_real_distribution<double>(lo, hi)(rng);
    };
    for (int round = 0; round < 8; ++round) {
        Rig r;
        std::vector<std::string> states{r.dump()};
        const std::size_t base = r.host.undoStack().undoCount();
        for (int step = 0; step < 6; ++step) {
            r.select();
            switch (rng() % 4) {
                case 0:
                    (void)r.host.run("MOVE", {pt(0, 0), pt(num(-50, 50), num(-50, 50))});
                    break;
                case 1:
                    (void)r.host.run(
                        "ROTATE", {pt(num(-30, 30), num(-30, 30)), std::to_string(num(-180, 180))});
                    break;
                case 2:
                    (void)r.host.run("SCALE",
                                     {pt(num(-30, 30), num(-30, 30)), std::to_string(num(0.3, 3))});
                    break;
                default:
                    (void)r.host.run("MIRROR", {pt(num(-20, 20), num(-20, 20)),
                                                pt(num(30, 60), num(30, 60)), "Yes"});
                    break;
            }
            const std::string d = r.dump();
            CHECK_FALSE(hasNonFinite(d));
            std::string why;
            CHECK_MESSAGE(r.fileRoundTrips(&why), "round ", round, " step ", step, "\n", why);
            states.push_back(d);
        }
        // Walk back through every state, then forward again.
        for (std::size_t k = states.size() - 1; k > 0; --k) {
            REQUIRE(r.host.undo());
            CHECK(r.dump() == states[k - 1]);
        }
        CHECK(r.host.undoStack().undoCount() == base);
        for (std::size_t k = 1; k < states.size(); ++k) {
            REQUIRE(r.host.redo());
            CHECK(r.dump() == states[k]);
        }
    }
}

TEST_CASE("dragging every grip of every hard object type is finite, undoes exactly and is saved") {
    Rig base;
    const std::size_t count = base.ids.size();
    std::size_t total = 0;
    for (std::size_t k = 0; k < count; ++k) {
        const std::size_t n =
            transform::collectGrips(base.doc, std::vector<EntityId>{base.ids[k]}).size();
        total += n;
        for (std::size_t g = 0; g < n; ++g) {
            Rig r;
            for (const char* mode : {"", "Move", "Rotate", "Scale"}) {
                CAPTURE(k);
                CAPTURE(g);
                CAPTURE(mode);
                const auto gs = transform::collectGrips(r.doc, std::vector<EntityId>{r.ids[k]});
                REQUIRE(g < gs.size());
                const std::string before = r.dump();
                r.host.setSelection({r.ids[k]});
                const Vec2 from = gs[g].pos;
                std::vector<cmd::ScriptItem> in{pt(from.x, from.y)};
                if (*mode) {
                    in.emplace_back(mode);
                    if (std::string(mode) != "Move")
                        in.emplace_back(pt(from.x + 1, from.y + 1));
                }
                in.emplace_back(pt(from.x + 3.5, from.y - 2.25));
                (void)r.host.run("GRIPEDIT", in);
                const std::string after = r.dump();
                CHECK_FALSE(hasNonFinite(after));
                std::string why;
                CHECK_MESSAGE(r.fileRoundTrips(&why), why);
                if (after != before) {
                    REQUIRE(r.host.undo());
                    CHECK(r.dump() == before);
                }
            }
        }
    }
    CHECK(total > 40);
}
