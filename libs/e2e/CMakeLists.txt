# End-to-end smoke test across the workstreams: open a DXF, draw, edit, hatch, save .tbcad and
# reload. Links every command module through tbcad::command_registry. Commands that are not on
# main yet (dimensions, plot) are exercised only when they are registered.
tbcad_add_library(e2e INTERFACE DEPENDS tbcad::core)

tbcad_add_tests(e2e
  SOURCES
    tests/main.cpp
    tests/smoke_tests.cpp
    tests/plot_colour_tests.cpp
    tests/command_fuzz_tests.cpp
    tests/hatch_follow_tests.cpp
    tests/undo_stress_tests.cpp
    tests/table_ops_tests.cpp
    tests/dimstyle_tests.cpp
  DEPENDS tbcad::dxf tbcad::io tbcad::cmd tbcad::command_registry)
# The target exists only when tests are built (the ezdxf and fuzz jobs configure without them).
if(TARGET tbcad_e2e_tests)
  target_compile_definitions(tbcad_e2e_tests PRIVATE
    TBCAD_TEST_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus"
    TBCAD_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/samples")
endif()
