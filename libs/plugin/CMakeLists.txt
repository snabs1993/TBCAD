# Plugin loader: shared libraries with a small C interface (include/tbcad/plugin/tbcad_plugin.h)
# that add commands and menu entries. The header is plain C and MIT licensed.
find_package(Threads REQUIRED)
tbcad_add_library(plugin
  SOURCES
    src/dynlib.cpp
    src/plugin_host.cpp
  DEPENDS tbcad::cmd tbcad::core tbcad::geom tbcad::ui Threads::Threads ${CMAKE_DL_LIBS})

# A sample plugin, written in plain C, used by the tests and as a template (docs/plugin-api.md).
if(TBCAD_BUILD_TESTS)
  add_library(tbcad_hello_plugin MODULE examples/hello_plugin.c)
  target_include_directories(tbcad_hello_plugin PRIVATE include)
  set_target_properties(tbcad_hello_plugin PROPERTIES
    PREFIX "" OUTPUT_NAME "hello_plugin"
    C_VISIBILITY_PRESET hidden
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins")
  if(MSVC)
    target_compile_options(tbcad_hello_plugin PRIVATE /W4)
  else()
    target_compile_options(tbcad_hello_plugin PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()

tbcad_add_tests(plugin
  SOURCES
    tests/main.cpp
    tests/plugin_tests.cpp
  DEPENDS tbcad_command_registry)
if(TBCAD_BUILD_TESTS)
  add_dependencies(tbcad_plugin_tests tbcad_hello_plugin)
  target_compile_definitions(tbcad_plugin_tests PRIVATE
    TBCAD_TEST_PLUGIN_DIR="${CMAKE_CURRENT_BINARY_DIR}/plugins"
    TBCAD_TEST_LIB_EXT="$<IF:$<BOOL:${WIN32}>,.dll,$<IF:$<BOOL:${APPLE}>,.dylib,.so>>")
endif()
