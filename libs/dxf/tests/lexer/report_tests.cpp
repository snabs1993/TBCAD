#include <doctest/doctest.h>

#include "tbcad/dxf/report.h"

namespace tbcad::dxf {

TEST_CASE("report counts by severity and serialises to JSON") {
    Report r;
    r.info("a.info", "hello");
    r.warn("a.warn", "quote \" backslash \\ newline \n tab \t", 12);
    r.error("a.err", "bad", 0, 0x1F);
    CHECK(r.count(Severity::Info) == 1);
    CHECK(r.count(Severity::Warning) == 1);
    CHECK(r.hasErrors());
    CHECK(r.toJson() ==
          "{\"info\":1,\"warnings\":1,\"errors\":1,\"diagnostics\":["
          "{\"severity\":\"info\",\"code\":\"a.info\",\"message\":\"hello\",\"line\":0},"
          "{\"severity\":\"warning\",\"code\":\"a.warn\",\"message\":\"quote \\\" backslash \\\\ "
          "newline \\n tab \\t\",\"line\":12},"
          "{\"severity\":\"error\",\"code\":\"a.err\",\"message\":\"bad\",\"line\":0,"
          "\"handle\":\"1F\"}]}");
}

TEST_CASE("JSON output is valid UTF-8 even when a message holds raw file bytes") {
    Report r;
    r.warn("x", std::string("ok \xCB\xE9 \xE0\xB8\xAB end \xED\xA0\x80"));
    const std::string json = r.toJson();
    CHECK(json.find("ok \\ufffd\\ufffd \xE0\xB8\xAB end \\ufffd\\ufffd\\ufffd") !=
          std::string::npos);
}

TEST_CASE("report stops recording past the cap but keeps counting") {
    Report r;
    for (std::size_t i = 0; i < Report::kMaxDiagnostics + 100; ++i) {
        r.warn("w", "x");
    }
    CHECK(r.count(Severity::Warning) == Report::kMaxDiagnostics + 100);
    CHECK(r.diagnostics().size() == Report::kMaxDiagnostics + 1);
    CHECK(r.diagnostics().back().code == code::kTooManyDiagnostics);
}

}  // namespace tbcad::dxf
