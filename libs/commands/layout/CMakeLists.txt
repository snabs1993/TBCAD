tbcad_add_library(layoutcmd
  SOURCES src/commands.cpp
  DEPENDS tbcad::cmd tbcad::core tbcad::layout tbcad::export)

tbcad_add_tests(layoutcmd
  SOURCES
    tests/main.cpp
    tests/layout_command_tests.cpp
  DEPENDS tbcad_layoutcmd)
