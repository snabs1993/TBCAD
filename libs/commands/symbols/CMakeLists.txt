tbcad_add_library(symbols_commands
  SOURCES
    src/symbols.cpp
    src/symbol_commands.cpp
    src/tolerance.cpp
  DEPENDS tbcad::cmd tbcad::blocks tbcad::gdt tbcad::geom tbcad::text tbcad::ui)

tbcad_add_tests(symbols_commands
  SOURCES
    tests/main.cpp
    tests/symbol_tests.cpp
  DEPENDS tbcad_command_registry)
