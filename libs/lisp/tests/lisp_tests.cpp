#include <doctest/doctest.h>

#include "tbcad/lisp/lisp.h"

using namespace tbcad::lisp;

namespace {
std::string run(Interp& in, std::string_view src) {
    return toString(in.evalText(src), true);
}
std::string run(std::string_view src) {
    Interp in;
    return run(in, src);
}
}  // namespace

TEST_CASE("arithmetic keeps integers and promotes to reals") {
    CHECK(run("(+ 1 2 3)") == "6");
    CHECK(run("(/ 7 2)") == "3");
    CHECK(run("(/ 7 2.0)") == "3.5");
    CHECK(run("(- 5)") == "-5");
    CHECK(run("(1+ 4)") == "5");
    CHECK(run("(rem 7 3)") == "1");
    CHECK(run("(fix 3.9)") == "3");
    CHECK(run("(expt 2 10)") == "1024");
    CHECK_THROWS_AS(run("(/ 1 0)"), Error);
}

TEST_CASE("defun with locals, recursion and dynamic scope") {
    Interp in;
    in.evalText("(defun fact (n) (if (<= n 1) 1 (* n (fact (1- n)))))");
    CHECK(run(in, "(fact 10)") == "3628800");
    in.evalText("(setq x 5) (defun f (a / x) (setq x a) x)");
    CHECK(run(in, "(f 9)") == "9");
    CHECK(run(in, "x") == "5");  // the local was restored
    CHECK(run(in, "(f)") == "nil");
}

TEST_CASE("lists, dotted pairs and assoc") {
    CHECK(run("(car '(1 2 3))") == "1");
    CHECK(run("(cdr '(1 2 3))") == "(2 3)");
    CHECK(run("(cadr '(1 2 3))") == "2");
    CHECK(run("(caddr '(1 2 3))") == "3");
    CHECK(run("(cons 1 2)") == "(1 . 2)");
    CHECK(run("(cdr (assoc 8 '((0 . \"LINE\") (8 . \"Walls\"))))") == "\"Walls\"");
    CHECK(run("(append '(1) '(2 3))") == "(1 2 3)");
    CHECK(run("(reverse '(1 2 3))") == "(3 2 1)");
    CHECK(run("(nth 1 '(a b c))") == "B");
    CHECK(run("(length '(1 2 3))") == "3");
    CHECK(run("(subst 9 2 '(1 2 3))") == "(1 9 3)");
    CHECK(run("(member 2 '(1 2 3))") == "(2 3)");
    CHECK(run("(mapcar '+ '(1 2) '(10 20))") == "(11 22)");
    CHECK(run("(apply '+ '(1 2 3))") == "6");
    CHECK(run("(mapcar (function (lambda (x) (* x x))) '(1 2 3))") == "(1 4 9)");
}

TEST_CASE("control flow: cond, while, repeat, foreach, and/or") {
    CHECK(run("(cond ((= 1 2) 'a) ((= 2 2) 'b) (t 'c))") == "B");
    CHECK(run("(setq i 0 s 0) (while (< i 5) (setq s (+ s i) i (1+ i))) s") == "10");
    CHECK(run("(setq n 0) (repeat 4 (setq n (+ n 2))) n") == "8");
    CHECK(run("(setq s 0) (foreach v '(1 2 3) (setq s (+ s v))) s") == "6");
    CHECK(run("(and 1 2 3)") == "3");
    CHECK(run("(and 1 nil 3)") == "nil");
    CHECK(run("(or nil 7)") == "7");
}

TEST_CASE("strings and number formatting") {
    CHECK(run("(strcat \"ab\" \"cd\")") == "\"abcd\"");
    CHECK(run("(substr \"hello\" 2 3)") == "\"ell\"");
    CHECK(run("(strlen \"abc\")") == "3");
    CHECK(run("(strcase \"aB\")") == "\"AB\"");
    CHECK(run("(strcase \"aB\" t)") == "\"ab\"");
    CHECK(run("(itoa 42)") == "\"42\"");
    CHECK(run("(atoi \"17\")") == "17");
    CHECK(run("(atof \"2.5\")") == "2.5");
    CHECK(run("(rtos 3.14159265 2 2)") == "\"3.14\"");
    CHECK(run("(rtos 5.0)") == "\"5\"");
    CHECK(run("(wcmatch \"WALL-1\" \"WALL*\")") == "T");
    CHECK(run("(wcmatch \"DOOR\" \"WALL*,D?OR\")") == "T");
    CHECK(run("\"a\\nb\"") == "\"a\\nb\"");
}

TEST_CASE("geometry helpers") {
    CHECK(run("(distance '(0 0) '(3 4))") == "5.0");
    CHECK(run("(angle '(0 0) '(0 1))").substr(0, 6) == "1.5707");
    CHECK(run("(polar '(0 0 0) 0 5)") == "(5.0 0.0 0.0)");
}

TEST_CASE("output goes through the out hook") {
    Interp in;
    std::string got;
    in.out = [&](const std::string& s) { got += s; };
    in.evalText("(princ \"hi \") (prin1 \"x\") (print 5) (terpri)");
    CHECK(got == "hi \"x\"\n5 \n");
}

TEST_CASE("comments and block comments are skipped") {
    CHECK(run("; line\n(+ 1 ;| inner |; 2) ; end") == "3");
}

TEST_CASE("errors: undefined function, malformed input, *error* handler, catch-all") {
    Interp in;
    CHECK_THROWS_AS(in.evalText("(nosuch 1)"), Error);
    CHECK_THROWS_AS(in.evalText("(+ 1"), Error);
    CHECK_THROWS_AS(in.evalText(")"), Error);
    CHECK_THROWS_AS(in.evalText("(car 5)"), Error);
    in.evalText("(defun *error* (m) (setq msg m))");
    CHECK_THROWS_AS(in.evalText("(/ 1 0)"), Error);
    CHECK(toString(in.get("msg"), false) == "divide by zero");
    CHECK(run(in, "(vl-catch-all-error-p (vl-catch-all-apply '/ '(1 0)))") == "T");
    CHECK(run(in, "(vl-catch-all-error-message (vl-catch-all-apply '/ '(1 0)))") ==
          "\"divide by zero\"");
}

TEST_CASE("runaway programs are stopped") {
    Interp in;
    in.stepBudget = 10000;
    CHECK_THROWS_AS(in.evalText("(while t (setq a 1))"), Error);
    Interp deep;
    deep.evalText("(defun f (n) (f (1+ n)))");
    CHECK_THROWS_AS(deep.evalText("(f 0)"), Error);
    CHECK_THROWS_AS(run(std::string(300, '(')), Error);
}

TEST_CASE("c: functions are announced and host subrs can be added") {
    Interp in;
    std::string seen;
    in.onCommandDefined = [&](const std::string& n) { seen = n; };
    in.defineSubr("twice", [](Interp&, Args& a) { return Value::integer(a[0].i * 2); });
    in.evalText("(defun c:hello () (twice 21))");
    CHECK(seen == "HELLO");
    CHECK(run(in, "(c:hello)") == "42");
}

TEST_CASE("load asks the loader and reports a missing file") {
    Interp in;
    in.loader = [](const std::string& name, std::string& out) {
        if (name != "lib.lsp")
            return false;
        out = "(defun lib-fn () 7)";
        return true;
    };
    in.evalText("(load \"lib.lsp\")");
    CHECK(run(in, "(lib-fn)") == "7");
    CHECK_THROWS_AS(in.evalText("(load \"nope.lsp\")"), Error);
    CHECK(run(in, "(load \"nope.lsp\" \"fallback\")") == "\"fallback\"");
}

TEST_CASE("garbage never crashes the reader or evaluator") {
    const char* junk[] = {"'",        "(.)",       "( . 1)",   "(1 . )",
                          "\"",       "(quote)",   "(setq)",   "(defun)",
                          "(cond 5)", "(foreach)", "(lambda)", "((lambda (x) x))",
                          "(car)"};
    for (const char* j : junk) {
        Interp in;
        try {
            in.evalText(j);
        } catch (const Error&) {
        }
    }
}
