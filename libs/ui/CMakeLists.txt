# UiRegistry: the small API command modules use to add panels, menu items, status toggles, overlay
# drawers and property sections. No Dear ImGui dependency: panels are plain callbacks.
tbcad_add_library(ui
  SOURCES src/ui_registry.cpp
  DEPENDS tbcad::core tbcad::cmd tbcad::render)

# AppCore: the application model without the window (document, selection, viewport, files,
# settings, command line). Kept apart from `ui` so command modules can link `ui` without a
# dependency cycle through tbcad::command_registry.
tbcad_add_library(appcore
  SOURCES
    src/app_commands.cpp
    src/app_core.cpp
    src/scene_sync.cpp
    src/selection.cpp
    src/settings_model.cpp
  DEPENDS tbcad::ui tbcad::blocks tbcad::xref tbcad::platform tbcad::geom tbcad::index tbcad::io tbcad::render tbcad::cmd)
target_include_directories(tbcad_appcore PRIVATE src)

tbcad_add_tests(ui
  SOURCES
    tests/main.cpp
    tests/ui_registry_tests.cpp)

tbcad_add_tests(appcore
  SOURCES
    tests/main.cpp
    tests/selection_tests.cpp
    tests/settings_model_tests.cpp
    tests/scene_sync_tests.cpp
    tests/block_scene_tests.cpp
    tests/app_core_tests.cpp
    tests/headless_file_tests.cpp)
