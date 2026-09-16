#!/bin/sh
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_build=$(mktemp -d /tmp/pdb-irq-tests.XXXXXX)
trap 'rm -rf "$test_build"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror \
  -I"$test_dir/irq_mocks" -I"$test_dir/mocks" -I"$test_dir/.." \
  "$test_dir/../ExpanderInterrupts.cpp" "$test_dir/test_expander_interrupts.cpp" \
  -o "$test_build/test_expander_interrupts"
"$test_build/test_expander_interrupts"
