# glad 2.0.8 (generated loader for OpenGL 3.3 core + GL_KHR_debug). The generated sources are
# committed in third_party/glad (no download, no generator needed at build time).
# glad itself: WTFPL OR CC0-1.0; the bundled Khronos header: MIT-style.
# License text: third_party/notices/glad.txt
#
# Regenerate with:
#   pip install glad2==2.0.8
#   glad --api "gl:core=3.3" --extensions GL_KHR_debug --reproducible --out-path out c
if(NOT TARGET tbcad_glad)
  add_library(tbcad_glad STATIC "${CMAKE_SOURCE_DIR}/third_party/glad/src/gl.c")
  target_include_directories(tbcad_glad SYSTEM PUBLIC "${CMAKE_SOURCE_DIR}/third_party/glad/include")
  set_target_properties(tbcad_glad PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99)
  add_library(tbcad::glad ALIAS tbcad_glad)
endif()
