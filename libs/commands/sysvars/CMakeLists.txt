tbcad_add_library(sysvars
  SOURCES
    src/sysvars.cpp
    src/rename.cpp
    src/setvar.cpp
    src/view.cpp
    src/addselected.cpp
  DEPENDS tbcad::cmd tbcad::core tbcad::layers)

tbcad_add_tests(sysvars
  SOURCES
    tests/main.cpp
    tests/sysvars_tests.cpp
  DEPENDS tbcad_command_registry)
