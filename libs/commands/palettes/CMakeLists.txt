tbcad_add_library(palettes
  SOURCES
    src/commands.cpp
    src/palettes.cpp
    src/register.cpp
  DEPENDS tbcad::cmd tbcad::core tbcad::platform tbcad::ui)

if(TBCAD_BUILD_APP)
  include(deps/imgui)
  target_sources(tbcad_palettes PRIVATE ui/palette_panel.cpp)
  target_link_libraries(tbcad_palettes PUBLIC tbcad::imgui)
  target_compile_definitions(tbcad_palettes PRIVATE TBCAD_PALETTES_IMGUI=1)
endif()

tbcad_add_tests(palettes
  SOURCES
    tests/main.cpp
    tests/palettes_tests.cpp
  DEPENDS tbcad_command_registry)
