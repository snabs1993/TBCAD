# TLS 1.3 + pre-shared key channel on top of libs/net, using Mbed TLS.
include(deps/mbedtls)

tbcad_add_library(nettls SOURCES src/tls.cpp DEPENDS tbcad::net)
target_link_libraries(tbcad_nettls PRIVATE mbedtls mbedx509 mbedcrypto)

tbcad_add_tests(nettls SOURCES tests/main.cpp tests/tls_tests.cpp DEPENDS tbcad::nettls)
