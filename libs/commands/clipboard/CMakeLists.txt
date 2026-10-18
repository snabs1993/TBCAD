# Clipboard commands: COPYCLIP, COPYBASE, CUTCLIP, PASTECLIP, PASTEBLOCK, PASTEORIG.
# The clip lives in the application process (see docs/clipboard.md).
tbcad_add_library(clipboard
  SOURCES
    src/clipboard.cpp
    src/commands.cpp
  DEPENDS tbcad::cmd tbcad::ui tbcad::geom tbcad::core tbcad::io)
tbcad_add_tests(clipboard
  SOURCES
    tests/main.cpp
    tests/clipboard_tests.cpp
  DEPENDS tbcad_clipboard tbcad_command_registry)
