tbcad_add_library(example
  SOURCES src/example.cpp
  DEPENDS tbcad::cmd)

# Proves the generated registration picks this module up.
if(TBCAD_REGISTER_EXAMPLE_COMMANDS)
  tbcad_add_tests(example
    SOURCES
      tests/main.cpp
      tests/example_tests.cpp
    DEPENDS tbcad_command_registry)
endif()
