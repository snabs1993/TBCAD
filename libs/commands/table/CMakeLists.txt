tbcad_add_library(table_commands
  SOURCES
    src/table_commands.cpp
  DEPENDS tbcad::cmd tbcad::table tbcad::blocks tbcad::geom tbcad::text tbcad::ui)

tbcad_add_tests(table_commands
  SOURCES
    tests/main.cpp
    tests/table_command_tests.cpp
  DEPENDS tbcad::cmd)
