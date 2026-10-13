# Networking: `libs/net` and `libs/nettls`

Shared by the MCP server and by real-time collaboration (see `build/collab-brief.md`).

## libs/net (`tbcad::net`)

- `socket.h`: blocking TCP on Winsock or POSIX. `listenLoopback` / `connectLoopback` for the MCP
  server (loopback only); `listenTcp(bindAddress, ...)` / `connectTcp(host, ...)` for LAN and
  VPN use. IPv4 only for now. All timeouts are in milliseconds; errors are return codes.
- `channel.h`: `Channel`, a connected byte stream (plain or TLS), and length-prefixed frames on
  top: a 4-byte little-endian length, then the payload, at most 16 MiB. `FrameReader::poll`
  keeps a half-received frame across timeouts.
- `argon2.h`: Argon2id (RFC 9106) and BLAKE2b (RFC 7693), written from the RFCs and tested
  against the reference implementation's output. No third-party code.

## libs/nettls (`tbcad::nettls`)

`connectTls` / `acceptTls` run a TLS 1.3 handshake with a pre-shared key (psk_dhe_ke, so past
traffic stays secret if the password leaks later) and return a `Channel`. `derivePsk` turns a
session password into the 32-byte key with Argon2id (default 19 MiB, 2 passes), salted with the
session name. A wrong password fails the handshake on both sides. Mbed TLS 3.6 (Apache-2.0) is
fetched by `cmake/deps/mbedtls.cmake`, pinned by SHA-256; its licence is in
`third_party/notices/mbedtls.txt`. Nothing here needs certificates.

One thread may read while others write; reads wait on the socket outside the TLS lock.
