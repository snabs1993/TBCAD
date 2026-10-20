set(_inquiry_ui_sources)
# The dialogs draw with Dear ImGui, which only exists when the application is built.
if(TBCAD_BUILD_APP)
  include(deps/imgui)
  set(_inquiry_ui_sources src/props_panel.cpp)
endif()

tbcad_add_library(inquiry
  SOURCES
    ${_inquiry_ui_sources}
    src/inquiry.cpp
    src/areatable.cpp
    src/audit.cpp
    src/measure.cpp
    src/massprop.cpp
    src/dwgprops.cpp
    src/findstray.cpp
    src/picktrace.cpp
    src/quick.cpp
    src/quickcmd.cpp
    src/calc.cpp
    src/ui.cpp
  DEPENDS tbcad::cmd tbcad::expr tbcad::geom tbcad::layout tbcad::plotstyle tbcad::ui tbcad::platform)
if(TBCAD_BUILD_APP)
  target_link_libraries(tbcad_inquiry PRIVATE tbcad::imgui)
  target_compile_definitions(tbcad_inquiry PRIVATE TBCAD_INQUIRY_WITH_IMGUI=1)
endif()

tbcad_add_tests(inquiry
  SOURCES
    tests/main.cpp
    tests/inquiry_tests.cpp
    tests/quick_tests.cpp
  DEPENDS tbcad_inquiry tbcad_command_registry)
