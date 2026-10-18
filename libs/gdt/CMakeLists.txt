# Geometric tolerances (GD&T feature control frames): the frame as drawing geometry, the table that
# remembers what a frame says, and the DXF TOLERANCE text. See docs/annotative.md.
tbcad_add_library(gdt
  SOURCES src/gdt.cpp
  DEPENDS tbcad::core)

tbcad_add_tests(gdt
  SOURCES
    tests/main.cpp
    tests/gdt_tests.cpp)
