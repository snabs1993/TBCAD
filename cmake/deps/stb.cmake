# stb_image 2.30 (public domain / MIT, either). Single-header PNG, JPEG and BMP decoder used by
# libs/underlay for image underlays. Fetched as one file pinned by SHA-256 (commit 31c1ad3).
# License text: third_party/notices/stb.txt
include_guard(GLOBAL)
include(FetchContent)

FetchContent_Declare(stb_image_header
  URL "https://raw.githubusercontent.com/nothings/stb/31c1ad37456438565541f4919958214b6e762fb4/stb_image.h"
  URL_HASH SHA256=594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3
  DOWNLOAD_NO_EXTRACT TRUE)
FetchContent_MakeAvailable(stb_image_header)

if(NOT TARGET stb_image)
  set(_stb_impl "${CMAKE_BINARY_DIR}/_deps/stb_image_impl/stb_image_impl.c")
  file(WRITE "${_stb_impl}" "
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_MAX_DIMENSIONS 16384
#include \"stb_image.h\"
")
  add_library(stb_image STATIC "${_stb_impl}")
  target_include_directories(stb_image SYSTEM PUBLIC "${stb_image_header_SOURCE_DIR}")
  set_target_properties(stb_image PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER third_party)
endif()
