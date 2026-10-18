# Text layout and display (W4-4): fonts per style, TEXT / MTEXT layout, Thai-safe wrapping,
# display generators for geom::DisplayRegistry. See docs/text.md.
tbcad_add_library(text
  SOURCES
    src/clusters.cpp
    src/fonts.cpp
    src/markup.cpp
    src/thai_break.cpp
    src/layout.cpp
    src/display.cpp
    src/annotation_size.cpp
    src/field.cpp
  DEPENDS tbcad::core tbcad::geom tbcad::font)
target_compile_definitions(tbcad_text PRIVATE
  TBCAD_TEXT_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets/dictionaries")

tbcad_add_tests(text
  SOURCES
    tests/main.cpp
    tests/cluster_tests.cpp
    tests/markup_tests.cpp
    tests/thai_break_tests.cpp
    tests/layout_tests.cpp
    tests/layout_fuzz_tests.cpp
    tests/display_tests.cpp
    tests/engine_tests.cpp
    tests/font_style_tests.cpp)
