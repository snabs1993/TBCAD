tbcad_add_library(constraints_commands
  SOURCES src/constraint_commands.cpp
  DEPENDS tbcad::cmd tbcad::constraints tbcad::geom tbcad::expr)

tbcad_add_tests(constraints_commands
  SOURCES
    tests/main.cpp
    tests/constraint_command_tests.cpp
  DEPENDS tbcad::cmd tbcad::io tbcad::transform)
