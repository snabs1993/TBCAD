# earcut.hpp 2.2.4 (ISC). Header-only polygon triangulation, fetched as a single file pinned by
# SHA-256. License text: third_party/notices/earcut.txt
include(FetchContent)

if(NOT TARGET tbcad_earcut)
  FetchContent_Declare(earcut_header
    URL "https://raw.githubusercontent.com/mapbox/earcut.hpp/v2.2.4/include/mapbox/earcut.hpp"
    URL_HASH SHA256=3b838bc3f643d34ced850b6d53b19e2d9abdf6c2c067425b02868d131379b3cc
    DOWNLOAD_NO_EXTRACT TRUE)
  FetchContent_MakeAvailable(earcut_header)

  # earcut.hpp must be reachable as <mapbox/earcut.hpp>.
  set(_ec_inc "${CMAKE_BINARY_DIR}/_deps/earcut_include/mapbox")
  file(MAKE_DIRECTORY "${_ec_inc}")
  file(COPY "${earcut_header_SOURCE_DIR}/earcut.hpp" DESTINATION "${_ec_inc}")

  add_library(tbcad_earcut INTERFACE)
  target_include_directories(tbcad_earcut SYSTEM INTERFACE "${CMAKE_BINARY_DIR}/_deps/earcut_include")
  add_library(tbcad::earcut ALIAS tbcad_earcut)
endif()
