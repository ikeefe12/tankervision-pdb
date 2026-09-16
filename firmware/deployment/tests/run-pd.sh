#!/bin/sh
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_bin=$(mktemp "${TMPDIR:-/tmp}/pdb-deployment-pd-test.XXXXXX")
trap 'rm -f "$test_bin"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -fsanitize="${SANITIZERS:-undefined}" -fno-omit-frame-pointer \
  -I"$test_dir/pd_mocks" -I"$test_dir/.." \
  "$test_dir/../PowerDelivery.cpp" "$test_dir/test_power_delivery.cpp" \
  -o "$test_bin"
"$test_bin"
