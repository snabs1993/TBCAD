#include <doctest/doctest.h>

#include "tbcad/platform/external_link.h"

using namespace tbcad::platform;

TEST_CASE("web, mail and document links are accepted") {
    CHECK(checkLink("https://example.com/a?b=1").kind == LinkKind::Web);
    CHECK(checkLink("  HTTP://example.com ").kind == LinkKind::Web);
    CHECK(checkLink("mailto:a@b.c").kind == LinkKind::Mail);
    CHECK(checkLink("C:\\Docs\\spec.pdf").kind == LinkKind::File);
    CHECK(checkLink("C:\\Docs\\").kind == LinkKind::Folder);
    CHECK(checkLink("spec.pdf").kind == LinkKind::File);
    const auto f = checkLink("file:///C:/Docs/spec.pdf");
    CHECK(f.kind == LinkKind::File);
    CHECK(f.target == "C:/Docs/spec.pdf");
}

TEST_CASE("programs, scripts, shares and unknown schemes are refused") {
    for (const char* bad :
         {"C:\\Windows\\System32\\calc.exe", "run.BAT", "a.exe ", "a.exe.", "x.ps1", "x.lnk",
          "javascript:alert(1)", "ms-msdt:/id", "ftp://x/y", "\\\\server\\share\\a.pdf",
          "//server/share/a.pdf", "https:/nohost", "", "   ", "a\nb.pdf"}) {
        const auto r = checkLink(bad);
        CHECK_MESSAGE(r.kind == LinkKind::Refused, bad);
        CHECK_MESSAGE(!r.reason.empty(), bad);
    }
    CHECK(checkLink("C:\\notes.exe\\readme.txt").kind == LinkKind::File);
}
