# Live collaboration on the local network (free, MIT): encrypted links between programs, Share /
# Join, presence, team controls (per-person layers, chat), host hand-over and offline work, on top
# of the session engine in libs/collab. Dear ImGui is only used by the panel, in the app build.
tbcad_add_library(collabapp
  SOURCES
    src/link.cpp
    src/discovery.cpp
    src/team.cpp
    src/collab_controller.cpp
    src/attach.cpp
  DEPENDS tbcad::collab tbcad::net tbcad::nettls tbcad::appcore tbcad::ui tbcad::cmd
          tbcad::core tbcad::platform)
target_include_directories(tbcad_collabapp PRIVATE src)

if(TBCAD_BUILD_APP)
  include(deps/imgui)
  target_sources(tbcad_collabapp PRIVATE src/collab_panel.cpp)
  target_link_libraries(tbcad_collabapp PUBLIC tbcad::imgui)
  target_compile_definitions(tbcad_collabapp PRIVATE TBCAD_COLLABAPP_IMGUI=1)
endif()

tbcad_add_tests(collabapp
  SOURCES
    tests/main.cpp
    tests/link_tests.cpp
    tests/discovery_tests.cpp
    tests/team_tests.cpp
    tests/live_session_tests.cpp
    tests/controller_tests.cpp
  DEPENDS tbcad_command_registry)
if(TARGET tbcad_collabapp_tests)
  target_include_directories(tbcad_collabapp_tests PRIVATE src)
endif()
