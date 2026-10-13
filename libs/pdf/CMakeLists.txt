include(deps/miniz)

tbcad_add_library(pdf SOURCES src/document.cpp src/page.cpp src/format.cpp DEPENDS tbcad::core)
target_link_libraries(tbcad_pdf PRIVATE miniz)

# Tests inflate content streams with miniz and run `qpdf --check` when available.
# CI sets TBCAD_REQUIRE_QPDF=1 so a missing qpdf fails there instead of skipping.
tbcad_add_tests(pdf SOURCES tests/pdf_tests.cpp DEPENDS miniz)
