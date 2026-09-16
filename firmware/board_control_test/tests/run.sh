#!/bin/sh
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_bin=$(mktemp "${TMPDIR:-/tmp}/pdb-driver-test.XXXXXX")
trap 'rm -f "$test_bin"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -I"$test_dir/mocks" -I"$test_dir/.." \
  "$test_dir/../BoardControl.cpp" "$test_dir/../ChargerHold.cpp" \
  "$test_dir/test_board_control.cpp" \
  -o "$test_bin"
"$test_bin"
