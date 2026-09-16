#!/bin/sh
# Instrument the real-bank 20-second shutdown test without automatic USB repair.
set -eu
exec sh "$(dirname "$0")/build.sh" \
  --build-property 'compiler.cpp.extra_flags=-DPDB_SUPERCAP_TRANSFER_TEST=1 -DPDB_USB_RECOVERY_TEST=1' "$@"
