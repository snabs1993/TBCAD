#include <doctest/doctest.h>

#include <fstream>

#include "tbcad/cmd/cmd.h"
#include "tbcad/lispcmd/lispcmd.h"

using namespace tbcad;

namespace {
class TestLine final : public cmd::Command {
public:
    std::string_view name() const override { return "TESTLINE"; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        auto a = co_await ctx.getPoint("From");
        if (!a.ok())
            co_return;
        auto b = co_await ctx.getPoint("To");
        if (!b.ok())
            co_return;
        (void)ctx.tx().add(Entity{{}, LineData{a.point, b.point}});
    }
};
void lispcmdTestRegisterLine(cmd::Registry& reg) {
    (void)reg.add(std::make_unique<TestLine>());
}

struct Fixture {
    Document doc;
    cmd::Registry reg;
    std::unique_ptr<cmd::ScriptHost> host;
    Fixture() {
        cmd::registerBuiltinCommands(reg);
        lispcmd::registerCommands(reg);
        host = std::make_unique<cmd::ScriptHost>(doc, reg);
    }
    cmd::ScriptResult lisp(const std::string& expr) { return host->run("LISP", {expr}); }
};
}  // namespace

TEST_CASE("LISP evaluates an expression and prints the result") {
    Fixture f;
    auto r = f.lisp("(+ 1 2)");
    CHECK(r.finished);
    CHECK(f.host->logContains("3"));
    f.lisp("(setq x 10)");
    f.host->clearLog();
    f.lisp("(* x 2)");  // globals live across runs
    CHECK(f.host->logContains("20"));
}

TEST_CASE("errors are reported and do not change the drawing") {
    Fixture f;
    f.lisp("(nosuchfunction 1)");
    CHECK(f.host->logContains("error"));
    CHECK(f.doc.entityCount() == 0);
}

TEST_CASE("princ output is shown line by line") {
    Fixture f;
    f.lisp("(progn (princ \"Hello \") (princ \"world\\n\") (princ \"again\"))");
    CHECK(f.host->logContains("Hello world"));
    CHECK(f.host->logContains("again"));
}

TEST_CASE("entmake draws, entget reads, entmod edits, entdel erases, all undoable") {
    Fixture f;
    f.lisp("(entmake '((0 . \"LINE\") (8 . \"Walls\") (10 0.0 0.0 0.0) (11 100.0 0.0 0.0)))");
    REQUIRE(f.doc.entityCount() == 1);
    CHECK(f.doc.layerByName("Walls") != nullptr);
    f.host->clearLog();
    f.lisp("(cdr (assoc 11 (entget (entlast))))");
    CHECK(f.host->logContains("(100.0 0.0 0.0)"));
    f.lisp("(entmod (subst '(11 50.0 50.0 0.0) '(11 100.0 0.0 0.0) (entget (entlast))))");
    const Entity* e = f.doc.find(f.doc.modelSpace().back());
    REQUIRE(e);
    CHECK(entityAs<LineData>(*e)->b == Vec2{50, 50});
    f.lisp("(entdel (entlast))");
    CHECK(f.doc.entityCount() == 0);
    CHECK(f.host->undo());
    CHECK(f.doc.entityCount() == 1);
}

TEST_CASE("entmake supports circle, arc, point, text and polyline") {
    Fixture f;
    f.lisp(
        "(progn (entmake '((0 . \"CIRCLE\") (10 5.0 5.0 0.0) (40 . 3.0))) "
        "(entmake '((0 . \"ARC\") (10 0.0 0.0 0.0) (40 . 2.0) (50 . 0.0) (51 . 1.5))) "
        "(entmake '((0 . \"POINT\") (10 1.0 1.0 0.0))) "
        "(entmake '((0 . \"TEXT\") (10 0.0 0.0 0.0) (40 . 5.0) (1 . \"Hi\"))) "
        "(entmake '((0 . \"LWPOLYLINE\") (70 . 1) (10 0.0 0.0) (10 10.0 0.0) (10 10.0 10.0))))");
    CHECK(f.doc.entityCount() == 5);
    f.host->clearLog();
    f.lisp("(cdr (assoc 0 (entget (entlast))))");
    CHECK(f.host->logContains("LWPOLYLINE"));
}

TEST_CASE("ssget X filters by type and layer, ssname walks the set") {
    Fixture f;
    f.lisp(
        "(progn (entmake '((0 . \"LINE\") (8 . \"A\") (10 0.0 0.0) (11 1.0 0.0))) "
        "(entmake '((0 . \"LINE\") (8 . \"B\") (10 0.0 0.0) (11 2.0 0.0))) "
        "(entmake '((0 . \"CIRCLE\") (8 . \"A\") (10 0.0 0.0) (40 . 1.0))))");
    f.host->clearLog();
    f.lisp("(sslength (ssget \"X\" '((0 . \"LINE\"))))");
    CHECK(f.host->logContains("2"));
    f.host->clearLog();
    f.lisp("(sslength (ssget \"X\" '((8 . \"A\"))))");
    CHECK(f.host->logContains("2"));
    f.host->clearLog();
    f.lisp("(if (ssget \"X\" '((0 . \"ARC\"))) \"found\" \"none\")");
    CHECK(f.host->logContains("none"));
    f.host->clearLog();
    f.lisp(
        "(progn (setq ss (ssget \"X\" '((0 . \"CIRCLE\")))) (cdr (assoc 40 (entget (ssname ss "
        "0)))))");
    CHECK(f.host->logContains("1.0"));
}

TEST_CASE("getpoint, getreal and getstring wait for the user") {
    Fixture f;
    f.host->run("LISP", {"(progn (setq p (getpoint \"Where\")) (setq r (getreal \"Radius\")) "
                         "(entmake (list '(0 . \"CIRCLE\") (cons 10 p) (cons 40 r))))",
                         "10,20", "7.5"});
    REQUIRE(f.doc.entityCount() == 1);
    const Entity* e = f.doc.find(f.doc.modelSpace().back());
    auto* c = entityAs<CircleData>(*e);
    REQUIRE(c);
    CHECK(c->center == Vec2{10, 20});
    CHECK(c->radius == doctest::Approx(7.5));
    CHECK(f.host->prompts().size() >= 3);
}

TEST_CASE("Esc at a prompt cancels the program and keeps the drawing as it was") {
    Fixture f;
    auto r = f.host->run("LISP",
                         {"(progn (getpoint \"Where\") (entmake '((0 . \"POINT\") (10 0.0 0.0))))",
                          cmd::ScriptItem::esc()});
    CHECK(r.finished);
    CHECK(f.doc.entityCount() == 0);
    CHECK(f.host->logContains("*Cancel*"));
}

TEST_CASE("defun c:NAME makes a command that can be typed and undone") {
    Fixture f;
    f.lisp("(defun c:mkpt () (entmake '((0 . \"POINT\") (10 3.0 4.0))) (princ \"made\\n\"))");
    REQUIRE(f.reg.find("MKPT") != nullptr);
    f.host->clearLog();
    auto r = f.host->run("MKPT", {});
    CHECK(r.finished);
    CHECK(f.doc.entityCount() == 1);
    CHECK(f.host->logContains("made"));
    CHECK(f.host->undo());
    CHECK(f.doc.entityCount() == 0);
}

TEST_CASE("command queues lines the command line then types") {
    Fixture f;
    cmd::registerBuiltinCommands(f.reg);
    lispcmdTestRegisterLine(f.reg);
    f.lisp("(command \"_.TESTLINE\" \"0,0\" \"10,0\")");
    CHECK(f.doc.entityCount() == 1);
}

TEST_CASE("APPLOAD loads a file, the startup suite and acad.lsp are run by loadStartup") {
    auto dir = std::filesystem::temp_directory_path() / "tbcad_lispcmd_test";
    std::filesystem::create_directories(dir);
    {
        std::ofstream(dir / "tool.lsp")
            << "(defun c:fromfile () (entmake '((0 . \"POINT\") (10 1.0 1.0))))";
        std::ofstream(dir / "acad.lsp") << "(defun c:fromacad () 1)";
        std::ofstream(dir / "startup-suite.txt") << (dir / "tool.lsp").string() << "\n";
    }
    SUBCASE("APPLOAD") {
        Fixture f;
        f.host->setSettingsFolder(dir);
        f.host->run("APPLOAD", {(dir / "tool.lsp").string()});
        CHECK(f.reg.find("FROMFILE") != nullptr);
        CHECK(f.host->logContains("Loaded"));
        f.host->run("APPLOAD", {(dir / "missing.lsp").string()});
        CHECK(f.host->logContains("error"));
    }
    SUBCASE("startup") {
        cmd::Registry reg;
        lispcmd::registerCommands(reg);
        std::vector<std::string> printed;
        lispcmd::loadStartup(reg, dir, [&](const std::string& s) { printed.push_back(s); });
        CHECK(reg.find("FROMACAD") != nullptr);
        CHECK(reg.find("FROMFILE") != nullptr);
        CHECK(printed.empty());
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("a program that never ends is stopped") {
    Fixture f;
    f.lisp("(while t (setq a 1))");
    CHECK(f.host->logContains("error"));
}

TEST_CASE("an expression typed in parentheses on the command line runs as LISP") {
    Fixture f;
    f.host->runner().submit(cmd::Input::typed("(+ 20 22)"));
    CHECK(f.host->logContains("42"));
    CHECK_FALSE(f.host->runner().active());
}
