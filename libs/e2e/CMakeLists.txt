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
    tests/odd_geometry_tests.cpp
    tests/table_churn_tests.cpp
    tests/hatch_follow_tests.cpp
    tests/script_gate_tests.cpp
    tests/stretch_parity_tests.cpp
    tests/hatch_pattern_follow_tests.cpp
    tests/undo_stress_tests.cpp
    tests/undo_options_tests.cpp
    tests/table_ops_tests.cpp
    tests/dimstyle_tests.cpp
    tests/dimstyle_overrides_tests.cpp
    tests/dim_chain_scale_tests.cpp
    tests/dim_assoc_tests.cpp
    tests/dim_assoc_more_tests.cpp
    tests/dim_transspatial_tests.cpp
    tests/dim_hard_geometry_tests.cpp
    tests/dim_perf_tests.cpp
    tests/ucs_world_tests.cpp
    tests/views_tests.cpp
    tests/command_help_tests.cpp
    tests/align_snap_tests.cpp
    tests/stretch_assoc_tests.cpp
    tests/attrib_robust_tests.cpp
    tests/dirty_flag_tests.cpp
    tests/mirrtext_tests.cpp
    tests/edit_family_perf_tests.cpp
    tests/scan_commands_perf_tests.cpp
    tests/transform_property_tests.cpp
    tests/bulk_edit_perf_tests.cpp
    tests/plot_perf_tests.cpp
    tests/audit_purge_large_tests.cpp
    tests/block_scale_tests.cpp
    tests/linetype_scale_tests.cpp
    tests/hatch_scale_tests.cpp
    tests/open_file_bench_tests.cpp
    tests/text_fonts_tests.cpp
    tests/cmdline_ux_tests.cpp
    tests/drawing_objects_tests.cpp
    tests/appcore_undo_tests.cpp
  DEPENDS tbcad::dxf tbcad::io tbcad::cmd tbcad::command_registry tbcad::appcore tbcad::dwgopen)
# The target exists only when tests are built (the ezdxf and fuzz jobs configure without them).
if(TARGET tbcad_e2e_tests)
  target_compile_definitions(tbcad_e2e_tests PRIVATE
    TBCAD_TEST_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus"
    TBCAD_FONTS_DIR="${CMAKE_SOURCE_DIR}/assets/fonts"
    TBCAD_COMMAND_HELP_FILE="${CMAKE_SOURCE_DIR}/docs/command-help.md"
    TBCAD_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/samples")
endif()

# Optional libFuzzer build (clang only): cmake -DTBCAD_CMD_FUZZ=ON. Commands with input made from
# the fuzzer's bytes, on the Thai house sample.
option(TBCAD_CMD_FUZZ "Build the command fuzz target with libFuzzer + ASan/UBSan" OFF)
if(TBCAD_CMD_FUZZ)
  add_executable(tbcad_cmd_fuzz tests/fuzz_commands.cpp)
  target_link_libraries(tbcad_cmd_fuzz PRIVATE tbcad::io tbcad::dxf tbcad::cmd tbcad::command_registry)
  target_include_directories(tbcad_cmd_fuzz PRIVATE ${CMAKE_SOURCE_DIR}/libs/dxf/tests)
  target_compile_definitions(tbcad_cmd_fuzz PRIVATE TBCAD_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/samples"
    TBCAD_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus")
  target_compile_options(tbcad_cmd_fuzz PRIVATE
    -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined)
  target_link_options(tbcad_cmd_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
