tbcad_add_library(modify_commands
  SOURCES
    src/break_join.cpp
    src/edit_tools.cpp
    src/fillet.cpp
    src/group_order.cpp
    src/misc.cpp
    src/misc2.cpp
    src/offset.cpp
    src/register.cpp
    src/trim.cpp
    src/util.cpp
    src/ui.cpp
  DEPENDS tbcad::ui tbcad::cmd tbcad::modify tbcad::geom tbcad::core tbcad::text tbcad::blocks tbcad::transform)
tbcad_add_tests(modify_commands
  SOURCES
    tests/main.cpp
    tests/command_tests.cpp
    tests/robustness_tests.cpp
    tests/cancel_sweep_tests.cpp
  DEPENDS tbcad_command_registry)
