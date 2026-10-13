#!/usr/bin/env bash
# Fails if a cmake/deps/<name>.cmake has no third_party/notices/<name>.txt.
set -u
cd "$(dirname "$0")/.."
status=0
shopt -s nullglob
for f in cmake/deps/*.cmake; do
  name=$(basename "$f" .cmake)
  if [ ! -s "third_party/notices/$name.txt" ]; then
    echo "ERROR: $f has no third_party/notices/$name.txt" >&2
    status=1
  fi
done
[ $status -eq 0 ] && echo "License notices OK"
exit $status
