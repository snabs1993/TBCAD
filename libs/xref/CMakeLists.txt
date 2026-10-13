# External references: a block whose content is a snapshot of another drawing (.tbcad or .dxf),
# with attach, reload when the file changes, detach and bind. See docs/xref.md.
tbcad_add_library(xref
  SOURCES
    src/xref.cpp
  DEPENDS tbcad::core tbcad::io tbcad::dxf tbcad::blocks tbcad::underlay)
tbcad_add_tests(xref
  SOURCES
    tests/main.cpp
    tests/xref_tests.cpp)
