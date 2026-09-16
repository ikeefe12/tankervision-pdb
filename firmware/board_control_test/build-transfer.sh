#!/bin/sh
# Real bank: explicit serial start, real thermistor, continuous transfer monitor.
set -eu
exec sh "$(dirname "$0")/build.sh" \
  --build-property 'compiler.cpp.extra_flags=-DPDB_SUPERCAP_TRANSFER_TEST=1' "$@"
