# tbcad-convert: command line converter (DXF -> .tbcad now, .tbcad -> DXF from W4-6).
# src/export/ belongs to W4-6 and is picked up by the glob below.
file(GLOB_RECURSE _convert_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(FILTER _convert_sources EXCLUDE REGEX "/src/main\\.cpp$")

tbcad_add_library(convert
  SOURCES ${_convert_sources}
  DEPENDS tbcad::core tbcad::dxf tbcad::dwg tbcad::io tbcad::platform tbcad::xref)

# File names with Thai characters and paths over 260 characters need the manifest on Windows.
if(WIN32)
  add_executable(tbcad-convert src/main.cpp tbcad-convert.manifest)
else()
  add_executable(tbcad-convert src/main.cpp)
endif()
target_link_libraries(tbcad-convert PRIVATE tbcad::convert)
tbcad_set_warnings(tbcad-convert)

file(GLOB_RECURSE _convert_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
tbcad_add_tests(convert SOURCES ${_convert_tests})
if(TBCAD_BUILD_TESTS)
  target_compile_definitions(tbcad_convert_tests PRIVATE
    TBCAD_TEST_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus")
endif()
