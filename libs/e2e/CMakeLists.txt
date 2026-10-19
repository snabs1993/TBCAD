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
    tests/dim_assoc_tests.cpp
    tests/dim_hard_geometry_tests.cpp
    tests/dim_perf_tests.cpp
    tests/ucs_world_tests.cpp
    tests/stretch_assoc_tests.cpp
    tests/attrib_robust_tests.cpp
    tests/mirrtext_tests.cpp
    tests/edit_family_perf_tests.cpp
    tests/transform_property_tests.cpp
    tests/bulk_edit_perf_tests.cpp
    tests/plot_perf_tests.cpp
    tests/audit_purge_large_tests.cpp
    tests/block_scale_tests.cpp
    tests/linetype_scale_tests.cpp
    tests/hatch_scale_tests.cpp
    tests/text_fonts_tests.cpp
    tests/cmdline_ux_tests.cpp
    tests/drawing_objects_tests.cpp
  DEPENDS tbcad::dxf tbcad::io tbcad::cmd tbcad::command_registry tbcad::appcore)
# The target exists only when tests are built (the ezdxf and fuzz jobs configure without them).
if(TARGET tbcad_e2e_tests)
  target_compile_definitions(tbcad_e2e_tests PRIVATE
    TBCAD_TEST_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus"
    TBCAD_FONTS_DIR="${CMAKE_SOURCE_DIR}/assets/fonts"
    TBCAD_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/samples")
endif()
