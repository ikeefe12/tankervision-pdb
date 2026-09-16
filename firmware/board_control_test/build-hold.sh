#!/bin/sh
# Explicit bench build: automatically holds unloaded VCAP high after every boot.
set -eu
exec sh "$(dirname "$0")/build.sh" \
  --build-property 'compiler.cpp.extra_flags=-DPDB_CHARGER_HOLD_ON_BOOT=1' "$@"
