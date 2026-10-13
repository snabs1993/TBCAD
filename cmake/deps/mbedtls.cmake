# Mbed TLS 3.6.2 (Apache-2.0 OR GPL-2.0-or-later; TBCAD uses it under Apache-2.0). TLS 1.3 with
# pre-shared keys for collaboration (libs/nettls). The official release archive, pinned by SHA-256.
# License text: third_party/notices/mbedtls.txt
include_guard(GLOBAL)
include(FetchContent)

set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE)  # the release archive ships the generated sources
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
set(INSTALL_MBEDTLS_HEADERS OFF CACHE BOOL "" FORCE)
set(MBEDTLS_AS_SUBPROJECT ON)

FetchContent_Declare(mbedtls_src
  URL "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2"
  URL_HASH SHA256=8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(mbedtls_src)
foreach(_t mbedtls mbedx509 mbedcrypto everest p256m)
  if(TARGET ${_t})
    set_target_properties(${_t} PROPERTIES FOLDER third_party POSITION_INDEPENDENT_CODE ON)
  endif()
endforeach()
