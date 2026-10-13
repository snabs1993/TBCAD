# DXF reader building blocks. Later workstreams (W3-2 reader, W4-6 writer) add their own
# source folders under src/ and tests/; the globs below pick them up, so they do not edit
# this file.
file(GLOB_RECURSE _dxf_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

tbcad_add_library(dxf SOURCES ${_dxf_sources} DEPENDS tbcad::core tbcad::geom tbcad::hatch tbcad::blocks)
target_include_directories(tbcad_dxf PRIVATE src/encoding src/reader)

# Optional libFuzzer build (clang only): cmake -DTBCAD_DXF_FUZZ=ON
option(TBCAD_DXF_FUZZ "Build the DXF fuzz targets with libFuzzer + ASan/UBSan" OFF)
if(TBCAD_DXF_FUZZ)
  set(_fuzz_flags -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined)
  target_compile_options(tbcad_dxf PRIVATE ${_fuzz_flags})
  file(GLOB_RECURSE _dxf_fuzz CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*/fuzz_*.cpp")
  foreach(_f IN LISTS _dxf_fuzz)
    get_filename_component(_n "${_f}" NAME_WE)
    add_executable(tbcad_dxf_${_n} "${_f}")
    target_link_libraries(tbcad_dxf_${_n} PRIVATE tbcad::dxf)
    target_compile_options(tbcad_dxf_${_n} PRIVATE ${_fuzz_flags})
    target_link_options(tbcad_dxf_${_n} PRIVATE -fsanitize=fuzzer,address,undefined)
  endforeach()
endif()

file(GLOB_RECURSE _dxf_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
list(FILTER _dxf_tests EXCLUDE REGEX "/fuzz_[^/]*\\.cpp$")
tbcad_add_tests(dxf SOURCES ${_dxf_tests})
if(TBCAD_BUILD_TESTS)
  # tests/corpus lives at the repository root so every module can share it.
  target_compile_definitions(tbcad_dxf_tests PRIVATE
    TBCAD_TEST_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus")
endif()
