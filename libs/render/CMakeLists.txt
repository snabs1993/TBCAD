option(TBCAD_BUILD_BENCH "Build the render_bench benchmark" ON)

include(deps/glad)
include(deps/earcut)

tbcad_add_library(render
  SOURCES
    src/camera.cpp
    src/colors.cpp
    src/fill.cpp
    src/layer_sync.cpp
    src/overlay.cpp
    src/pattern.cpp
    src/scene.cpp
    src/scene_instance.cpp
    src/gl/renderer.cpp
    src/gl/shaders.cpp
  DEPENDS tbcad::core)
target_include_directories(tbcad_render PRIVATE src)
# GL calls stay inside src/gl/ (a Direct3D 11 backend would sit beside it); glad is private.
target_link_libraries(tbcad_render PRIVATE tbcad::glad tbcad::earcut)

# Offscreen GL context for the smoke tests and the benchmark (not part of the product).
add_library(tbcad_render_testsupport STATIC testsupport/gl_context.cpp)
target_include_directories(tbcad_render_testsupport PUBLIC testsupport)
target_compile_features(tbcad_render_testsupport PUBLIC cxx_std_20)
tbcad_set_warnings(tbcad_render_testsupport)
target_link_libraries(tbcad_render_testsupport PUBLIC ${CMAKE_DL_LIBS}
                      $<$<PLATFORM_ID:Windows>:opengl32>)

tbcad_add_tests(render
  SOURCES
    tests/main.cpp
    tests/camera_tests.cpp
    tests/pattern_tests.cpp
    tests/precision_tests.cpp
    tests/fill_tests.cpp
    tests/scene_tests.cpp
    tests/overlay_tests.cpp
    tests/layer_sync_tests.cpp
    tests/gl_smoke_tests.cpp
  DEPENDS tbcad::glad tbcad_render_testsupport)

if(TBCAD_BUILD_BENCH)
  add_executable(render_bench bench/render_bench.cpp)
  target_compile_features(render_bench PRIVATE cxx_std_20)
  tbcad_set_warnings(render_bench)
  target_link_libraries(render_bench PRIVATE tbcad::render tbcad::glad tbcad_render_testsupport)
endif()
