#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tbcad/hello/hello.h"

TEST_CASE("banner names the product") {
    CHECK(tbcad::hello::banner() == "TBCAD 0.0.1");
}
