set(_textcmd_sources
  src/annotative_command.cpp
  src/field_command.cpp
  src/editable.cpp
  src/style_command.cpp
  src/style_manager.cpp
  src/text_commands.cpp
  src/txt2mtxt.cpp
  src/txtexp.cpp
  src/arctext.cpp
  src/textalign.cpp
  src/textfit.cpp
  src/tcount.cpp
  src/spell.cpp
  src/mtext_format.cpp
  src/mtext_explode.cpp
  src/format_edits.cpp
  src/register.cpp)

# The editor panel needs Dear ImGui, which is only fetched when the application is built.
if(TBCAD_BUILD_APP)
  include(deps/imgui)
  list(APPEND _textcmd_sources ui/text_editor_panel.cpp ui/text_style_panel.cpp ui/annotation_section.cpp
       ui/format_bar.cpp ui/text_context_tab.cpp)
endif()

tbcad_add_library(textcmd
  SOURCES ${_textcmd_sources}
  DEPENDS tbcad::cmd tbcad::text tbcad::geom tbcad::ui tbcad::blocks)
target_include_directories(tbcad_textcmd PRIVATE src)
if(TBCAD_BUILD_APP)
  target_link_libraries(tbcad_textcmd PRIVATE tbcad::imgui)
  target_compile_definitions(tbcad_textcmd PRIVATE TBCAD_TEXTCMD_IMGUI=1)
endif()

# Headless command tests link the generated registry so the module's registration is covered too.
tbcad_add_tests(textcmd
  SOURCES
    tests/main.cpp
    tests/editable_tests.cpp
    tests/format_edits_tests.cpp
    tests/text_command_tests.cpp
    tests/style_command_tests.cpp
    tests/annotative_tests.cpp
    tests/field_tests.cpp
    tests/style_manager_tests.cpp
  DEPENDS tbcad_command_registry)
