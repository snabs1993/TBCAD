# Text layout and display (W4-4): fonts per style, TEXT / MTEXT layout, Thai-safe wrapping,
# display generators for geom::DisplayRegistry. See docs/text.md.
tbcad_add_library(text
  SOURCES
    src/clusters.cpp
    src/fonts.cpp
    src/markup.cpp
    src/layout.cpp
    src/display.cpp
  DEPENDS tbcad::core tbcad::geom tbcad::font)

tbcad_add_tests(text
  SOURCES
    tests/main.cpp
    tests/cluster_tests.cpp
    tests/markup_tests.cpp
    tests/layout_tests.cpp
    tests/display_tests.cpp)
