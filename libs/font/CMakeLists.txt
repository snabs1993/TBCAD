include(deps/freetype)
include(deps/harfbuzz)

tbcad_add_library(font SOURCES src/font_library.cpp src/stroke_font.cpp src/shx_font.cpp)
target_link_libraries(tbcad_font PRIVATE freetype harfbuzz)
target_compile_definitions(tbcad_font PRIVATE
  TBCAD_BUNDLED_FONT_DIR="${CMAKE_SOURCE_DIR}/assets/fonts")

tbcad_add_tests(font SOURCES tests/font_tests.cpp)
if(TARGET tbcad_font_tests)
  target_compile_definitions(tbcad_font_tests PRIVATE
    TBCAD_FONT_TEST_ASSETS="${CMAKE_SOURCE_DIR}/assets/fonts")
endif()

# Optional libFuzzer build (clang only): cmake -DTBCAD_FORMATS_FUZZ=ON
if(TBCAD_FORMATS_FUZZ)
  set(_fuzz_flags -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined)
  target_compile_options(tbcad_font PRIVATE ${_fuzz_flags})
  add_executable(tbcad_font_fuzz_stroke tests/fuzz_stroke.cpp)
  target_link_libraries(tbcad_font_fuzz_stroke PRIVATE tbcad::font)
  target_include_directories(tbcad_font_fuzz_stroke PRIVATE src)
  target_compile_options(tbcad_font_fuzz_stroke PRIVATE ${_fuzz_flags})
  target_link_options(tbcad_font_fuzz_stroke PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
