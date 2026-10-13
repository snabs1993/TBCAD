include(deps/freetype)
include(deps/harfbuzz)

tbcad_add_library(font SOURCES src/font_library.cpp src/stroke_font.cpp)
target_link_libraries(tbcad_font PRIVATE freetype harfbuzz)
target_compile_definitions(tbcad_font PRIVATE
  TBCAD_BUNDLED_FONT_DIR="${CMAKE_SOURCE_DIR}/assets/fonts")

tbcad_add_tests(font SOURCES tests/font_tests.cpp)
if(TARGET tbcad_font_tests)
  target_compile_definitions(tbcad_font_tests PRIVATE
    TBCAD_FONT_TEST_ASSETS="${CMAKE_SOURCE_DIR}/assets/fonts")
endif()
