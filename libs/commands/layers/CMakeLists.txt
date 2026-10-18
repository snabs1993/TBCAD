# Layers with sub-layers, linetypes, LAYER / -LAYER / -PROP / -LINETYPE and the layer and
# properties panels (W4-3).

# Our own linetype file is compiled into the library so loading works without the assets folder.
set(_lt_file "${CMAKE_SOURCE_DIR}/assets/linetypes/tbcad.lintypes")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_lt_file}")
file(READ "${_lt_file}" TBCAD_LINETYPES_TEXT)
configure_file(src/builtin_linetypes.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/builtin_linetypes.cpp" @ONLY)

set(_layers_sources
  src/layer_ops.cpp
  src/layer_states.cpp
  src/state_commands.cpp
  src/linetype_file.cpp
  src/properties.cpp
  src/layer_commands.cpp
  src/layer_tools.cpp
  src/laywalk.cpp
  src/property_commands.cpp
  src/register.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/builtin_linetypes.cpp")
set(_layers_deps tbcad::cmd tbcad::ui tbcad::core tbcad::geom tbcad::blocks tbcad::layout)

# The panels draw with Dear ImGui, which only exists when the application is built. Without it
# the panels are registered as empty stubs, so headless builds and tests still link.
if(TBCAD_BUILD_APP)
  include(deps/imgui)
  list(APPEND _layers_sources ui/color_picker.cpp ui/layers_panel.cpp ui/laywalk_panel.cpp ui/properties_panel.cpp)
  list(APPEND _layers_deps tbcad::imgui)
else()
  list(APPEND _layers_sources src/panels_stub.cpp)
endif()

tbcad_add_library(layers SOURCES ${_layers_sources} DEPENDS ${_layers_deps})
target_include_directories(tbcad_layers PRIVATE src)
if(TBCAD_BUILD_APP)
  target_include_directories(tbcad_layers PRIVATE ui)
endif()

tbcad_add_tests(layers
  SOURCES
    tests/main.cpp
    tests/layer_ops_tests.cpp
    tests/layer_command_tests.cpp
    tests/layer_tools_tests.cpp
    tests/layer_state_tests.cpp
    tests/linetype_tests.cpp
    tests/properties_tests.cpp
  DEPENDS tbcad_layers tbcad_command_registry tbcad::io)
if(TARGET tbcad_layers_tests)
  target_compile_definitions(tbcad_layers_tests PRIVATE TBCAD_SOURCE_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
endif()
