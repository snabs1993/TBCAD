tbcad_add_library(cmd
  SOURCES
    src/builtin.cpp
    src/context.cpp
    src/input.cpp
    src/registry.cpp
    src/runner.cpp
    src/script_host.cpp
    src/undo.cpp
  DEPENDS tbcad::core)

# Generates the command registration (register_all.cpp) from libs/commands/*/module.cmake.
include(command_modules)
tbcad_discover_command_modules()

tbcad_add_tests(cmd
  SOURCES
    tests/main.cpp
    tests/alias_tests.cpp
    tests/runner_tests.cpp
    tests/undo_tests.cpp)
