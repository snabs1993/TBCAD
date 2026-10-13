set(_plot_sources src/commands.cpp src/export_file.cpp src/print_cmd.cpp src/settings.cpp src/stamp.cpp src/ui.cpp)

# The dialog panel draws with Dear ImGui, which only exists when the application is built.
if(TBCAD_BUILD_APP)
  include(deps/imgui)
  list(APPEND _plot_sources src/plot_panel.cpp src/style_panel.cpp)
endif()

tbcad_add_library(plot
  SOURCES ${_plot_sources}
  DEPENDS tbcad::cmd tbcad::core tbcad::geom tbcad::export tbcad::ui tbcad::layout tbcad::plotstyle
          tbcad::sheets tbcad::print)
if(TBCAD_BUILD_APP)
  target_link_libraries(tbcad_plot PRIVATE tbcad::imgui)
  target_compile_definitions(tbcad_plot PRIVATE TBCAD_PLOT_WITH_IMGUI=1)
endif()

tbcad_add_tests(plot
  SOURCES
    tests/main.cpp
    tests/plot_command_tests.cpp
    tests/plot_content_tests.cpp
    tests/print_command_tests.cpp
  DEPENDS tbcad_plot tbcad::text tbcad::hatch)
