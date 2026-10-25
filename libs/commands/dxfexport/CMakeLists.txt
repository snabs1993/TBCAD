tbcad_add_library(dxfexport
  SOURCES
    src/dxfexport.cpp
    src/merge.cpp
  DEPENDS tbcad::cmd tbcad::dxf tbcad::io)

tbcad_add_tests(dxfexport
  SOURCES
    tests/main.cpp
    tests/dxfexport_tests.cpp
  DEPENDS tbcad_dxfexport tbcad_command_registry)
