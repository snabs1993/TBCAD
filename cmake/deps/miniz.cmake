# miniz 3.0.2 (MIT). Single-file deflate, used by libs/pdf for /FlateDecode.
# Fetched as the amalgamated release zip, pinned by SHA-256.
# License text: third_party/notices/miniz.txt
include_guard(GLOBAL)
include(FetchContent)

FetchContent_Declare(miniz_src
  URL "https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip"
  URL_HASH SHA256=ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(miniz_src)

if(NOT TARGET miniz)
  add_library(miniz STATIC "${miniz_src_SOURCE_DIR}/miniz.c")
  target_include_directories(miniz SYSTEM PUBLIC "${miniz_src_SOURCE_DIR}")
  # Third-party code: no project warning flags. Keep it free of stdio/time/archive
  # features we do not use.
  target_compile_definitions(miniz PRIVATE MINIZ_NO_STDIO MINIZ_NO_TIME
                                           MINIZ_NO_ARCHIVE_APIS MINIZ_NO_ARCHIVE_WRITING_APIS)
  set_target_properties(miniz PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER third_party)
endif()
