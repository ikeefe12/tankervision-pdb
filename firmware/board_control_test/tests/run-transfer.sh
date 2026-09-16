#!/bin/sh
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_bin=$(mktemp "${TMPDIR:-/tmp}/pdb-transfer-test.XXXXXX")
trap 'rm -f "$test_bin"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -I"$test_dir/mocks" -I"$test_dir/.." \
  "$test_dir/../SupercapTransfer.cpp" "$test_dir/test_supercap_transfer.cpp" \
  -o "$test_bin"
"$test_bin"
