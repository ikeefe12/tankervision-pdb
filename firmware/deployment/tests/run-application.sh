#!/bin/sh
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_work=$(mktemp -d "${TMPDIR:-/tmp}/pdb-application-test.XXXXXX")
trap 'rm -rf "$test_work"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -fsanitize=undefined -fno-omit-frame-pointer \
  -I"$test_dir/app_mocks" -I"$test_dir/.." \
  "$test_dir/../Application.cpp" "$test_dir/../PowerManager.cpp" \
  "$test_dir/../Protocol.cpp" "$test_dir/test_application.cpp" \
  -o "$test_work/application-test"
for scenario in loss reboot reconnect; do
  "$test_work/application-test" "$scenario" "$test_work/$scenario.jsonl"
done
python3 "$test_dir/validate_application_json.py" "$test_work"/*.jsonl
