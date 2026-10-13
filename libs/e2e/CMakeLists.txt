# End-to-end smoke test across the workstreams: open a DXF, draw, edit, hatch, save .tbcad and
# reload. Links every command module through tbcad::command_registry. Commands that are not on
# main yet (dimensions, plot) are exercised only when they are registered.
tbcad_add_library(e2e INTERFACE DEPENDS tbcad::core)

tbcad_add_tests(e2e
  SOURCES
    tests/main.cpp
    tests/smoke_tests.cpp
  DEPENDS tbcad::dxf tbcad::io tbcad::cmd tbcad::command_registry)
if(TARGET tbcad_e2e_tests)
  target_compile_definitions(tbcad_e2e_tests PRIVATE
    TBCAD_TEST_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus")
endif()
