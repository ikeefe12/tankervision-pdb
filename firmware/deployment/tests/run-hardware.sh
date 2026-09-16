#!/bin/sh
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_bin=$(mktemp "${TMPDIR:-/tmp}/pdb-deployment-hardware.XXXXXX")
trap 'rm -f "$test_bin"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -I"$test_dir/hardware_mocks" -I"$test_dir/.." \
  "$test_dir/../BoardHardware.cpp" "$test_dir/test_board_hardware.cpp" -o "$test_bin"
"$test_bin"
