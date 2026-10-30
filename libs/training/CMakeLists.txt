# Practice mode: lessons (assets/training/lessons/*.json), the checks that tick their steps off,
# stars and the saved progress. No window code; the application draws the panel.
tbcad_add_library(training
  SOURCES
    src/badges.cpp
    src/check.cpp
    src/lesson.cpp
    src/run.cpp
  DEPENDS tbcad::core tbcad::geom tbcad::blocks tbcad::platform)

tbcad_add_tests(training
  SOURCES
    tests/main.cpp
    tests/check_tests.cpp
    tests/lesson_tests.cpp
  DEPENDS tbcad::training tbcad::appcore tbcad::command_registry tbcad::cmd)
if(TBCAD_BUILD_TESTS)
  target_compile_definitions(tbcad_training_tests
    PRIVATE TBCAD_LESSON_DIR="${CMAKE_SOURCE_DIR}/assets/training/lessons")
endif()
