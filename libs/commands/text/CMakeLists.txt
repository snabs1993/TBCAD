set(_textcmd_sources
  src/editable.cpp
  src/style_command.cpp
  src/text_commands.cpp
  src/register.cpp)

# The editor panel needs Dear ImGui, which is only fetched when the application is built.
if(TBCAD_BUILD_APP)
  include(deps/imgui)
  list(APPEND _textcmd_sources ui/text_editor_panel.cpp)
endif()

tbcad_add_library(textcmd
  SOURCES ${_textcmd_sources}
  DEPENDS tbcad::cmd tbcad::text tbcad::geom tbcad::ui)
if(TBCAD_BUILD_APP)
  target_link_libraries(tbcad_textcmd PRIVATE tbcad::imgui)
  target_compile_definitions(tbcad_textcmd PRIVATE TBCAD_TEXTCMD_IMGUI=1)
endif()

# Headless command tests link the generated registry so the module's registration is covered too.
tbcad_add_tests(textcmd
  SOURCES
    tests/main.cpp
    tests/editable_tests.cpp
    tests/text_command_tests.cpp
    tests/style_command_tests.cpp
  DEPENDS tbcad_command_registry)
