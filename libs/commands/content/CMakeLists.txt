# The content browser: browse folders of .tbcad/.dxf/.tbt files and copy blocks, layers,
# linetypes, text styles and dimension styles from them into the current drawing.
tbcad_add_library(content_cmds
  SOURCES
    src/content_command.cpp
    src/register.cpp
  DEPENDS tbcad::cmd tbcad::xref tbcad::block_cmds tbcad::ui)

if(TBCAD_BUILD_APP)
  include(deps/imgui)
  target_sources(tbcad_content_cmds PRIVATE ui/content_panel.cpp)
  target_link_libraries(tbcad_content_cmds PUBLIC tbcad::imgui)
  target_compile_definitions(tbcad_content_cmds PRIVATE TBCAD_CONTENT_IMGUI=1)
endif()

tbcad_add_tests(content_cmds
  SOURCES
    tests/main.cpp
    tests/content_command_tests.cpp
  DEPENDS tbcad_command_registry)
