# Embeds assets/patterns/tbcad.patterns into the library so the built-in patterns need no file at
# run time. The file is the single source of truth: edit it and re-run CMake.
set(_pattern_file "${CMAKE_SOURCE_DIR}/assets/patterns/tbcad.patterns")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_pattern_file}")
file(READ "${_pattern_file}" _pattern_text)
set(_pattern_cpp "${CMAKE_CURRENT_BINARY_DIR}/generated/pattern_data.cpp")
file(WRITE "${_pattern_cpp}.tmp"
"// Generated from assets/patterns/tbcad.patterns by libs/hatch/CMakeLists.txt. Do not edit.
#include \"tbcad/hatch/pattern.h\"

namespace tbcad::hatch {

std::string_view builtinPatternText() {
    return R\"TBCADPAT(${_pattern_text})TBCADPAT\";
}

}  // namespace tbcad::hatch
")
file(COPY_FILE "${_pattern_cpp}.tmp" "${_pattern_cpp}" ONLY_IF_DIFFERENT)
file(REMOVE "${_pattern_cpp}.tmp")

tbcad_add_library(hatch
  SOURCES
    src/associate.cpp
    src/boundary.cpp
    src/display.cpp
    src/fill.cpp
    src/gradient.cpp
    src/pattern.cpp
    src/trim.cpp
    "${_pattern_cpp}"
  DEPENDS tbcad::core tbcad::geom)

tbcad_add_tests(hatch
  SOURCES
    tests/main.cpp
    tests/associate_tests.cpp
    tests/gap_tests.cpp
    tests/boundary_tests.cpp
    tests/fill_tests.cpp
    tests/pattern_tests.cpp
    tests/perf_tests.cpp
    tests/fuzz_tests.cpp
    tests/messy_tests.cpp)

# Optional libFuzzer build (clang only): cmake -DTBCAD_FORMATS_FUZZ=ON
if(TBCAD_FORMATS_FUZZ)
  set(_fuzz_flags -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined)
  target_compile_options(tbcad_hatch PRIVATE ${_fuzz_flags})
  add_executable(tbcad_hatch_fuzz_patterns tests/fuzz_patterns.cpp)
  target_link_libraries(tbcad_hatch_fuzz_patterns PRIVATE tbcad::hatch)
  target_compile_options(tbcad_hatch_fuzz_patterns PRIVATE ${_fuzz_flags})
  target_link_options(tbcad_hatch_fuzz_patterns PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
