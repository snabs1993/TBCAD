set(_platform_sources src/json.cpp src/paths.cpp src/settings.cpp src/keybindings.cpp)
if(WIN32)
  list(APPEND _platform_sources src/file_dialog_win32.cpp)
else()
  list(APPEND _platform_sources src/file_dialog_stub.cpp)
endif()

tbcad_add_library(platform SOURCES ${_platform_sources} DEPENDS tbcad::core)
if(WIN32)
  target_link_libraries(tbcad_platform PRIVATE ole32 shell32 uuid)
  target_compile_definitions(tbcad_platform PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endif()

tbcad_add_tests(platform
  SOURCES
    tests/main.cpp
    tests/json_tests.cpp
    tests/paths_tests.cpp
    tests/settings_tests.cpp
    tests/keybindings_tests.cpp)
