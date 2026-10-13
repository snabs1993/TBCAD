include(deps/glfw)
include(deps/imgui)

set(_tbcad_app_sources
  src/main.cpp
  src/app.cpp
  src/gui.cpp
  src/gui_style.cpp
  src/key_map.cpp
  src/file_browser.cpp
  src/viewport_target.cpp
  src/gate.cpp)
if(WIN32)
  list(APPEND _tbcad_app_sources src/ime_win32.cpp tbcad.manifest)
else()
  list(APPEND _tbcad_app_sources src/ime_stub.cpp)
endif()

add_executable(tbcad ${_tbcad_app_sources})
target_compile_features(tbcad PRIVATE cxx_std_20)
tbcad_set_warnings(tbcad)
target_include_directories(tbcad PRIVATE src)
target_compile_definitions(tbcad PRIVATE
  GLFW_INCLUDE_NONE
  TBCAD_VERSION="${PROJECT_VERSION}"
  TBCAD_SOURCE_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
target_link_libraries(tbcad PRIVATE
  tbcad_appcore tbcad_ui tbcad_platform tbcad_render tbcad_command_registry
  tbcad::glad tbcad::imgui)
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
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/THIRD_PARTY_NOTICES.txt"
          "$<TARGET_FILE_DIR:tbcad>/THIRD_PARTY_NOTICES.txt"
  VERBATIM)
add_dependencies(tbcad notices)

# Headless smoke test: opens a (hidden) window, runs commands, draws a few frames. Needs a display
# and OpenGL 3.3 (CI uses xvfb and Mesa); skipped when TBCAD_RUN_GUI_TESTS is not set.
if(TBCAD_BUILD_TESTS)
  add_test(NAME tbcad_gui_smoke
           COMMAND tbcad --self-test --frames 5 --script "DEVGEN LINES 20000")
  set_tests_properties(tbcad_gui_smoke PROPERTIES
    SKIP_RETURN_CODE 77
    ENVIRONMENT "TBCAD_CONFIG_DIR=${CMAKE_CURRENT_BINARY_DIR}/smoke-config;TBCAD_DATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/smoke-data")
endif()
