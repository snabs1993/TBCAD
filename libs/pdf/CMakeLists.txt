include(deps/miniz)

tbcad_add_library(pdf SOURCES src/document.cpp src/page.cpp src/format.cpp src/truetype.cpp
  DEPENDS tbcad::core)
find_package(Threads REQUIRED)
target_link_libraries(tbcad_pdf PRIVATE miniz Threads::Threads)

# Tests inflate content streams with miniz and run `qpdf --check` when available.
# CI sets TBCAD_REQUIRE_QPDF=1 so a missing qpdf fails there instead of skipping.
tbcad_add_tests(pdf SOURCES tests/pdf_tests.cpp DEPENDS miniz)
target_compile_definitions(tbcad_pdf_tests PRIVATE
  TBCAD_TEST_FONT_DIR="${CMAKE_SOURCE_DIR}/assets/fonts")
target_include_directories(tbcad_pdf_tests PRIVATE src)
