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
# Validated with Arduino ESP32 core 3.3.3; module is ESP32-S3-WROOM-1-N8R8.
fqbn='esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB,PSRAM=opi,FlashMode=qio'
exec "$cli" compile --fqbn "$fqbn" --build-path "$PWD/build" --warnings all "$@" "$PWD"
