#!/bin/sh
set -eu
cd "$(dirname "$0")"
if [ -n "${ARDUINO_CLI:-}" ]; then
  cli="$ARDUINO_CLI"
elif command -v arduino-cli >/dev/null 2>&1; then
  cli=arduino-cli
else
  cli='/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli'
fi
# Explicit software CDC: its reset hooks are disabled before USB.begin().
fqbn='esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,FlashSize=8M,PartitionScheme=default_8MB,PSRAM=opi,FlashMode=qio'
exec "$cli" compile --fqbn "$fqbn" --build-path "$PWD/build" --warnings all "$@" "$PWD"
