# Shared blocking TCP sockets (Winsock / POSIX) for the MCP server and collaboration.
if(WIN32)
  set(_net_sources src/socket_win32.cpp src/argon2.cpp src/channel.cpp)
else()
  set(_net_sources src/socket_posix.cpp src/argon2.cpp src/channel.cpp)
endif()

tbcad_add_library(net SOURCES ${_net_sources})
if(WIN32)
  target_link_libraries(tbcad_net PUBLIC ws2_32)
endif()
find_package(Threads REQUIRED)
target_link_libraries(tbcad_net PUBLIC Threads::Threads)

tbcad_add_tests(net SOURCES tests/main.cpp tests/socket_tests.cpp tests/argon2_tests.cpp tests/frame_tests.cpp DEPENDS tbcad::net)
