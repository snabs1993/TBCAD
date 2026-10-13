# GLFW 3.4 (zlib/libpng license). The official release archive, pinned by SHA-256.
# License text: third_party/notices/glfw.txt
include(FetchContent)

if(NOT TARGET tbcad_glfw_target)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)  # X11 only on Linux; Windows is the product
  set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw
    URL "https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip"
    URL_HASH SHA256=b5ec004b2712fd08e8861dc271428f048775200a2df719ccf575143ba749a3e9
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(glfw)
  add_library(tbcad_glfw_target INTERFACE)
  target_link_libraries(tbcad_glfw_target INTERFACE glfw)
  add_library(tbcad::glfw ALIAS tbcad_glfw_target)
endif()
