tbcad_add_library(sysvars
  SOURCES
    src/sysvars.cpp
    src/rename.cpp
    src/setvar.cpp
    src/view.cpp
    src/vports.cpp
    src/addselected.cpp
    src/hyperlink.cpp
    src/script.cpp
    src/ui.cpp
  DEPENDS tbcad::cmd tbcad::core tbcad::layers tbcad::layout tbcad::ui)

if(TBCAD_BUILD_APP)
  include(deps/imgui)
  target_sources(tbcad_sysvars PRIVATE ui/views_panel.cpp ui/vports_panel.cpp)
  target_compile_definitions(tbcad_sysvars PRIVATE TBCAD_SYSVARS_WITH_IMGUI)
  target_link_libraries(tbcad_sysvars PRIVATE tbcad::imgui)
endif()

tbcad_add_tests(sysvars
  SOURCES
    tests/main.cpp
    tests/sysvars_tests.cpp
  DEPENDS tbcad_command_registry)
