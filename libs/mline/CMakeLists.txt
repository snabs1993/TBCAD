# Multilines (MLINE / MLSTYLE): styles, element geometry, display and explode. See docs/mline.md.
tbcad_add_library(mline
  SOURCES
    src/mline.cpp
    src/display.cpp
  DEPENDS tbcad::core tbcad::geom)

tbcad_add_tests(mline
  SOURCES
    tests/main.cpp
    tests/mline_tests.cpp)
