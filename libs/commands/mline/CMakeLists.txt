tbcad_add_library(mline_commands
  SOURCES
    src/mline_commands.cpp
    src/mledit.cpp
  DEPENDS tbcad::cmd tbcad::mline tbcad::blocks tbcad::geom tbcad::ui)

tbcad_add_tests(mline_commands
  SOURCES
    tests/main.cpp
    tests/mline_command_tests.cpp
  DEPENDS tbcad::cmd)
