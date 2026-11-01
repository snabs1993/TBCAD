// Three programs (AppCore + CollabController each) in one process, over real links.
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <thread>

#include "collab_controller.h"
#include "tbcad/cmd/register_all.h"

using namespace tbcad;
using namespace tbcad::collabapp;
namespace fs = std::filesystem;

namespace {

struct Program {
    fs::path dir;
    cmd::Registry registry;
    std::unique_ptr<ui::AppCore> core;
    std::unique_ptr<CollabController> collab;

    explicit Program(const std::string& name) {
        dir = fs::temp_directory_path() / ("tbcad_collabapp_" + name);
        fs::remove_all(dir);
        fs::create_directories(dir);
        registerAllCommands(registry);
        core =
            std::make_unique<ui::AppCore>(registry, platform::AppPaths{dir / "cfg", dir / "data"});
        collab = std::make_unique<CollabController>(*core, std::function<void()>{});
        CollabController* c = collab.get();
        core->addDocumentClosingHook([c](Document& d) { c->drawingClosing(d); });
    }
    ~Program() {
        collab.reset();
        core.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    Document& doc() { return core->document(); }
};

template <class Pred>
bool until(std::initializer_list<Program*> ps, Pred done, int ms = 15000) {
    for (int i = 0; i < ms / 5; ++i) {
        for (Program* p : ps)
            p->collab->pump();
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

void drawLine(Document& d, Vec2 a, Vec2 b) {
    Transaction tx = d.begin("line");
    Entity e;
    e.common.layer = kLayerZero;
    e.data = LineData{a, b};
    REQUIRE(tx.add(std::move(e)).ok());
    tx.commit();
}

}  // namespace

TEST_CASE("controller: share, join, edit, chat and hand the host role over") {
    Program a("a"), b("b"), c("c");
    std::string err;
    drawLine(a.doc(), {0, 0}, {10, 0});
    REQUIRE_MESSAGE(a.collab->share(0, "test-pw", "A", true, err), err);
    a.collab->setAskBeforeJoin(false);
    const int port = a.collab->port();
    REQUIRE(b.collab->join("127.0.0.1", port, "test-pw", "B", err));
    REQUIRE(c.collab->join("127.0.0.1", port, "test-pw", "C", err));
    auto joined = [](Program& p) { return p.collab->mode() == CollabController::Mode::Joined; };
    REQUIRE(until({&a, &b, &c}, [&] { return joined(b) && joined(c); }));
    CHECK(b.doc().entityCount() == 1);

    drawLine(c.doc(), {0, 1}, {5, 1});
    REQUIRE(until({&a, &b, &c}, [&] { return b.doc().entityCount() == 2; }));
    CHECK(a.doc().entityCount() == 2);

    c.collab->sendChat("hello");
    REQUIRE(until({&a, &b, &c}, [&] { return !b.collab->chat().empty(); }));
    CHECK(b.collab->chat().back().text == "hello");
    CHECK(b.collab->nameOf(b.collab->chat().back().from) == "C");

    // A hands over to B.
    collab::PeerId bId = 0;
    for (const auto& p : a.collab->peers())
        if (p.name == "B")
            bId = p.id;
    REQUIRE(a.collab->handOver(bId, err));
    REQUIRE(until({&a, &b, &c}, [&] {
        return b.collab->mode() == CollabController::Mode::Hosting && joined(a) && joined(c) &&
               b.collab->peers().size() == 2;
    }));
    // Edits keep flowing through the new host.
    drawLine(c.doc(), {0, 2}, {5, 2});
    REQUIRE(until({&a, &b, &c}, [&] {
        return b.doc().entityCount() == 3 && a.doc().entityCount() == 3;
    }));
}

TEST_CASE("controller: a guest that lost the host keeps working and sends it on reconnect") {
    Program a("oa"), b("ob");
    std::string err;
    drawLine(a.doc(), {0, 0}, {10, 0});
    REQUIRE_MESSAGE(a.collab->share(0, "test-pw", "A", true, err), err);
    a.collab->setAskBeforeJoin(false);
    const int port = a.collab->port();
    REQUIRE(b.collab->join("127.0.0.1", port, "test-pw", "B", err));
    REQUIRE(until({&a, &b}, [&] { return b.collab->mode() == CollabController::Mode::Joined; }));
    const EntityId shared = b.doc().modelSpace().front();

    a.collab->stop();
    REQUIRE(until({&a, &b}, [&] { return b.collab->mode() == CollabController::Mode::Offline; }));

    // Offline: one new line, and a change to the line A also changes meanwhile.
    drawLine(b.doc(), {0, 5}, {5, 5});
    {
        Transaction tx = b.doc().begin("move");
        REQUIRE(tx.modify(shared, [](Entity& e) { std::get<LineData>(e.data).b = {10, 9}; }).ok());
        tx.commit();
    }
    {
        Transaction tx = a.doc().begin("move");
        REQUIRE(tx.modify(shared, [](Entity& e) { std::get<LineData>(e.data).b = {10, -9}; }).ok());
        tx.commit();
    }
    CHECK(b.collab->offlineEdits() == 2);

    REQUIRE_MESSAGE(a.collab->share(port, "test-pw", "A", true, err), err);
    a.collab->setAskBeforeJoin(false);
    REQUIRE(b.collab->reconnect(err));
    REQUIRE(until({&a, &b}, [&] { return a.doc().entityCount() == 2; }));
    REQUIRE(b.collab->conflicts().size() == 1);
    // A's change stands until B chooses.
    CHECK(std::get<LineData>(a.doc().find(shared)->data).b.y == -9);
    b.collab->keepMine(0);
    REQUIRE(until({&a, &b}, [&] {
        return std::get<LineData>(a.doc().find(shared)->data).b.y == 9;
    }));
}
