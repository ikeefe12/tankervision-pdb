#!/bin/sh
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_bin=$(mktemp "${TMPDIR:-/tmp}/pdb-protocol-test.XXXXXX")
trap 'rm -f "$test_bin"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -fsanitize="${SANITIZERS:-undefined}" -fno-omit-frame-pointer \
  -I"$test_dir/.." \
  "$test_dir/../Protocol.cpp" "$test_dir/test_protocol.cpp" \
  -o "$test_bin"
"$test_bin"
