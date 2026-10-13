tbcad_add_library(transform
  SOURCES
    src/apply.cpp
    src/commands.cpp
    src/grips.cpp
    src/grip_command.cpp
    src/ui.cpp
  DEPENDS tbcad::cmd tbcad::geom tbcad::ui)

tbcad_add_tests(transform
  SOURCES
    tests/main.cpp
    tests/transform_tests.cpp
    tests/grip_tests.cpp
  DEPENDS tbcad_command_registry)
