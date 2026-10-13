# Paper-space layouts (Q30): layouts with viewports on top of core blocks and Header::custom,
# plotted through libs/export. See docs/layout.md.
tbcad_add_library(layout
  SOURCES src/layout.cpp src/layout_plot.cpp src/pagesetup.cpp
  DEPENDS tbcad::core tbcad::geom tbcad::export tbcad::blocks)

tbcad_add_tests(layout
  SOURCES
    tests/main.cpp
    tests/layout_tests.cpp
    tests/pagesetup_tests.cpp
  DEPENDS tbcad::pdf tbcad::io tbcad::blocks)
