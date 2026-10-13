# Live collaboration, core part (Q61): entity/row versions, id ranges, the wire form of an edit,
# host-side validation and the replica a guest keeps. No sockets here (see docs/collaboration.md).
tbcad_add_library(collab
  SOURCES
    src/bytes.h
    src/versions.cpp
    src/ops.cpp
    src/ids.cpp
    src/host.cpp
    src/replica.cpp
    src/snapshot.cpp
    src/undo_guard.cpp
    src/session.cpp
  DEPENDS tbcad::core tbcad::io)

tbcad_add_tests(collab
  SOURCES
    tests/main.cpp
    tests/helpers.h
    tests/versions_tests.cpp
    tests/ids_tests.cpp
    tests/ops_tests.cpp
    tests/host_tests.cpp
    tests/convergence_tests.cpp
    tests/undo_guard_tests.cpp
    tests/session_tests.cpp)
