tbcad_add_library(layoutcmd
  SOURCES src/commands.cpp src/ui.cpp
  DEPENDS tbcad::cmd tbcad::core tbcad::geom tbcad::layout tbcad::export tbcad::ui tbcad::render tbcad::clipboard tbcad::dxf tbcad::io)

tbcad_add_tests(layoutcmd
  SOURCES
    tests/main.cpp
    tests/layout_command_tests.cpp
  DEPENDS tbcad_layoutcmd)
