tbcad_add_library(cmd
  SOURCES
    src/builtin.cpp
    src/context.cpp
    src/describe.cpp
    src/input.cpp
    src/registry.cpp
    src/pgp.cpp
    src/runner.cpp
    src/script_host.cpp
    src/undo.cpp
    src/view_history.cpp
  DEPENDS tbcad::core)

# Generates the command registration (register_all.cpp) from libs/commands/*/module.cmake.
include(command_modules)
tbcad_discover_command_modules()

tbcad_add_tests(cmd
  SOURCES
    tests/main.cpp
    tests/alias_tests.cpp
    tests/hook_tests.cpp
    tests/describe_tests.cpp
    tests/runner_tests.cpp
    tests/undo_tests.cpp)
