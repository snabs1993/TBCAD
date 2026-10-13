tbcad_add_library(modify_commands
  SOURCES
    src/break_join.cpp
    src/fillet.cpp
    src/misc.cpp
    src/misc2.cpp
    src/offset.cpp
    src/register.cpp
    src/trim.cpp
    src/util.cpp
  DEPENDS tbcad::cmd tbcad::modify tbcad::geom tbcad::core)
tbcad_add_tests(modify_commands
  SOURCES
    tests/main.cpp
    tests/command_tests.cpp
  DEPENDS tbcad_command_registry)
