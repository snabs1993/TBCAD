# Drafting aids: typed coordinate input, object snap, ortho / polar tracking, grid and grid snap.
# Pure logic on top of core, geom, index and cmd; no window or ImGui. The commands and the UI
# that use it live in libs/commands/drafting.
tbcad_add_library(drafting
  SOURCES
    src/settings.cpp
    src/point_parser.cpp
    src/snap_geometry.cpp
    src/osnap.cpp
    src/dyninput.cpp
    src/filters.cpp
    src/grid.cpp
    src/ucs.cpp
    src/markers.cpp
  DEPENDS tbcad::core tbcad::geom tbcad::index tbcad::cmd tbcad::render)

tbcad_add_tests(drafting
  SOURCES
    tests/main.cpp
    tests/parser_tests.cpp
    tests/osnap_tests.cpp
    tests/tracking_tests.cpp
    tests/dyninput_tests.cpp
    tests/grid_tests.cpp
    tests/ucs_tests.cpp)
