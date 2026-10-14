# Collaboration over TCP + TLS-PSK (Q62): moves the frames of libs/collab sessions over libs/net
# channels. The sessions themselves stay single threaded; only TLS handshakes run on helper threads.
tbcad_add_library(collabnet SOURCES src/tcp.cpp DEPENDS tbcad::collab tbcad::net tbcad::nettls)

tbcad_add_tests(collabnet
  SOURCES
    tests/main.cpp
    tests/tcp_tests.cpp
  DEPENDS tbcad::collabnet)
