# Dimensions and leaders (W5-5): measurement, text formatting, geometry from definition points and
# DimStyle, display generators and the explode hook. See docs/dim.md.
tbcad_add_library(dim
  SOURCES
    src/format.cpp
    src/layout.cpp
    src/display.cpp
  DEPENDS tbcad::core tbcad::geom tbcad::text)

tbcad_add_tests(dim
  SOURCES
    tests/main.cpp
    tests/format_tests.cpp
    tests/layout_tests.cpp
    tests/display_tests.cpp
    tests/explode_tests.cpp)
