# DWG reader (clean-room, see docs/dwg-cleanroom-log.md). Decodes a .dwg file to DXF-equivalent
# records and hands them to the DXF importer.
file(GLOB_RECURSE _dwg_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

tbcad_add_library(dwg SOURCES ${_dwg_sources} DEPENDS tbcad::core tbcad::dxf)
target_include_directories(tbcad_dwg PRIVATE src)

# Optional libFuzzer build (clang only): cmake -DTBCAD_DWG_FUZZ=ON
option(TBCAD_DWG_FUZZ "Build the DWG fuzz target with libFuzzer + ASan/UBSan" OFF)
if(TBCAD_DWG_FUZZ)
  set(_fuzz_flags -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined)
  target_compile_options(tbcad_dwg PRIVATE ${_fuzz_flags})
  add_executable(tbcad_dwg_fuzz_dwg tests/fuzz_dwg.cpp)
  target_link_libraries(tbcad_dwg_fuzz_dwg PRIVATE tbcad::dwg)
  target_compile_options(tbcad_dwg_fuzz_dwg PRIVATE ${_fuzz_flags})
  target_link_options(tbcad_dwg_fuzz_dwg PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

file(GLOB_RECURSE _dwg_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
list(FILTER _dwg_tests EXCLUDE REGEX "/fuzz_[^/]*\\.cpp$")
tbcad_add_tests(dwg SOURCES ${_dwg_tests})
if(TBCAD_BUILD_TESTS)
  target_compile_definitions(tbcad_dwg_tests PRIVATE
    TBCAD_TEST_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus")
  target_include_directories(tbcad_dwg_tests PRIVATE src)
  target_link_libraries(tbcad_dwg_tests PRIVATE tbcad::layout)
endif()
