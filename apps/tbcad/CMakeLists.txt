include(deps/glfw)
include(deps/imgui)

set(_tbcad_app_sources
  src/main.cpp
  src/app.cpp
  src/viewport_target.cpp
  src/gate.cpp)
# Everything drawn with Dear ImGui lives in a library so the window-less GUI tests can use it too.
set(_tbcad_gui_sources
  src/gui.cpp
  src/gui_style.cpp
  src/wheel_menu.cpp
  src/icons.cpp
  src/key_map.cpp
  src/settings_dialog.cpp
  src/file_browser.cpp)
if(WIN32)
  # Version info and icon (packaging/windows/tbcad.rc.in).
  enable_language(RC)
  string(REPLACE "." "," _v "${PROJECT_VERSION}")
  set(TBCAD_VERSION_COMMAS "${_v},0")
  set(TBCAD_VERSION_DOTS "${PROJECT_VERSION}.0")
  set(TBCAD_ICON_PATH "${CMAKE_SOURCE_DIR}/packaging/windows/tbcad.ico")
  configure_file("${CMAKE_SOURCE_DIR}/packaging/windows/tbcad.rc.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/tbcad.rc" @ONLY)
  list(APPEND _tbcad_app_sources tbcad.manifest "${CMAKE_CURRENT_BINARY_DIR}/tbcad.rc")
  list(APPEND _tbcad_gui_sources src/ime_win32.cpp)
else()
  list(APPEND _tbcad_gui_sources src/ime_stub.cpp)
endif()

# The commit the program was built from, shown in Help > About.
set(TBCAD_GIT_HASH "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=10 HEAD
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE _hash OUTPUT_STRIP_TRAILING_WHITESPACE
                  RESULT_VARIABLE _hashResult ERROR_QUIET)
  if(_hashResult EQUAL 0 AND NOT _hash STREQUAL "")
    set(TBCAD_GIT_HASH "${_hash}")
  endif()
endif()

set(_tbcad_app_libs
  tbcad_appcore tbcad_mcpapp tbcad_plugin tbcad_dwgopen tbcad_drafting tbcad_ui tbcad_platform tbcad_render tbcad_text tbcad_command_registry
  tbcad::glad tbcad::imgui)
set(_tbcad_app_defs
  GLFW_INCLUDE_NONE
  TBCAD_VERSION="${PROJECT_VERSION}"
  TBCAD_GIT_HASH="${TBCAD_GIT_HASH}"
  TBCAD_SOURCE_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")

add_library(tbcad_gui STATIC ${_tbcad_gui_sources})
target_compile_features(tbcad_gui PUBLIC cxx_std_20)
tbcad_set_warnings(tbcad_gui)
target_include_directories(tbcad_gui PUBLIC src)
target_compile_definitions(tbcad_gui PUBLIC ${_tbcad_app_defs})
target_link_libraries(tbcad_gui PUBLIC ${_tbcad_app_libs})
if(WIN32)
  target_link_libraries(tbcad_gui PUBLIC imm32)
  target_compile_definitions(tbcad_gui PUBLIC NOMINMAX)
endif()

add_executable(tbcad ${_tbcad_app_sources})
target_compile_features(tbcad PRIVATE cxx_std_20)
tbcad_set_warnings(tbcad)
target_include_directories(tbcad PRIVATE src)
target_compile_definitions(tbcad PRIVATE ${_tbcad_app_defs})
target_link_libraries(tbcad PRIVATE tbcad_gui ${_tbcad_app_libs})
if(WIN32)
  target_link_libraries(tbcad PRIVATE imm32)
  # A GUI program that keeps `main` as its entry point.
  target_link_options(tbcad PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
  target_compile_definitions(tbcad PRIVATE NOMINMAX)
endif()

# Fonts and the notices file next to the executable, so it runs from the build folder.
add_custom_command(TARGET tbcad POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets"
          "$<TARGET_FILE_DIR:tbcad>/assets"
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:tbcad>/assets/help"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/docs/quickstart.md"
          "$<TARGET_FILE_DIR:tbcad>/assets/help/quickstart.md"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/THIRD_PARTY_NOTICES.txt"
          "$<TARGET_FILE_DIR:tbcad>/THIRD_PARTY_NOTICES.txt"
  VERBATIM)
add_dependencies(tbcad notices)

# Headless smoke test: opens a (hidden) window, runs commands, draws a few frames. Needs a display
# and OpenGL 3.3 (CI uses xvfb and Mesa); skipped when TBCAD_RUN_GUI_TESTS is not set.
if(TBCAD_BUILD_TESTS)
  # The command list for the user guide: needs no window, so it runs everywhere.
  add_test(NAME tbcad_list_commands
           COMMAND ${CMAKE_COMMAND}
             -DEXE=$<TARGET_FILE:tbcad>
             -DOUT=${CMAKE_CURRENT_BINARY_DIR}/commands.md
             -DCONFIG=${CMAKE_CURRENT_BINARY_DIR}/list-config
             -P ${CMAKE_CURRENT_SOURCE_DIR}/check_list_commands.cmake)
  # Every menu entry's command run headless (no window): must not crash or hang.
  add_executable(tbcad_menu_smoke_tests tests/menu_smoke.cpp)
  target_compile_features(tbcad_menu_smoke_tests PRIVATE cxx_std_20)
  tbcad_set_warnings(tbcad_menu_smoke_tests)
  target_link_libraries(tbcad_menu_smoke_tests PRIVATE
    tbcad_appcore tbcad_ui tbcad_platform tbcad_command_registry doctest::doctest)
  include(doctest)
  doctest_discover_tests(tbcad_menu_smoke_tests PROPERTIES TIMEOUT 600)
  # Every panel drawn for a few frames in a window-less Dear ImGui context; any recoverable ImGui
  # error (wrong table column count, unbalanced Begin / End ...) fails the test.
  add_executable(tbcad_panel_frame_tests tests/panel_frames.cpp)
  target_compile_features(tbcad_panel_frame_tests PRIVATE cxx_std_20)
  tbcad_set_warnings(tbcad_panel_frame_tests)
  target_link_libraries(tbcad_panel_frame_tests PRIVATE
    tbcad_appcore tbcad_ui tbcad_platform tbcad_command_registry tbcad::imgui doctest::doctest)
  doctest_discover_tests(tbcad_panel_frame_tests PROPERTIES TIMEOUT 300)
  # The real Gui driven with queued mouse / keyboard events and compared with golden pictures; no
  # window, no GPU (GLFW's null platform and a software rasteriser).
  add_executable(tbcad_gui_tests tests/gui_tests.cpp tests/gui_harness.cpp)
  target_compile_features(tbcad_gui_tests PRIVATE cxx_std_20)
  tbcad_set_warnings(tbcad_gui_tests)
  target_include_directories(tbcad_gui_tests PRIVATE tests)
  target_compile_definitions(tbcad_gui_tests PRIVATE
    TBCAD_GUI_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden"
    TBCAD_GUI_FONT_DIR="${CMAKE_SOURCE_DIR}/assets/fonts"
    TBCAD_GUI_FAILURE_DIR="${CMAKE_CURRENT_BINARY_DIR}/gui-test-failures")
  target_link_libraries(tbcad_gui_tests PRIVATE tbcad_gui tbcad_mcp tbcad_underlay doctest::doctest)
  doctest_discover_tests(tbcad_gui_tests PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)
  add_test(NAME tbcad_gui_smoke
           COMMAND tbcad --self-test --frames 5 --script "DEVGEN LINES 20000")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/capture-smoke.txt"
       "LINE\n0,0\n100,50\n\n@shot ${CMAKE_CURRENT_BINARY_DIR}/capture-smoke.png\n")
  add_test(NAME tbcad_capture_smoke
           COMMAND tbcad --self-test --capture-script "${CMAKE_CURRENT_BINARY_DIR}/capture-smoke.txt")
  set_tests_properties(tbcad_capture_smoke PROPERTIES
    SKIP_RETURN_CODE 77
    ENVIRONMENT "TBCAD_CONFIG_DIR=${CMAKE_CURRENT_BINARY_DIR}/smoke-config;TBCAD_DATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/smoke-data")
  # Whole workflows driven like a user (mouse at world points, keys, typed lines) with checks:
  # apps/tbcad/tests/scenarios/*.txt, run in a hidden window. Skipped (77) without a display.
  file(GLOB TBCAD_SCENARIOS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/scenarios/*.txt")
  foreach(scenario ${TBCAD_SCENARIOS})
    get_filename_component(scenarioName "${scenario}" NAME_WE)
    add_test(NAME tbcad_scenario_${scenarioName}
             COMMAND tbcad --self-test --capture-script "${scenario}"
                     --capture-out "${CMAKE_CURRENT_BINARY_DIR}/scenarios/${scenarioName}")
    set_tests_properties(tbcad_scenario_${scenarioName} PROPERTIES
      SKIP_RETURN_CODE 77
      RUN_SERIAL TRUE
      ENVIRONMENT "TBCAD_CONFIG_DIR=${CMAKE_CURRENT_BINARY_DIR}/scenario-config/${scenarioName};TBCAD_DATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/scenario-data/${scenarioName}")
  endforeach()
  set_tests_properties(tbcad_gui_smoke PROPERTIES
    SKIP_RETURN_CODE 77
    ENVIRONMENT "TBCAD_CONFIG_DIR=${CMAKE_CURRENT_BINARY_DIR}/smoke-config;TBCAD_DATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/smoke-data")
endif()
