#!/usr/bin/env python3
"""Keep one serial connection open through charge/transfer; relay stdin commands.

Opening or closing this host's USB serial can reset ESP32 and drop backup. This
logger has no timed exit or automatic reconnect. /quit requires a fresh heartbeat
confirming main power restored. A lost USB connection is logged, not hidden.
"""
import argparse
from datetime import datetime, timezone
from pathlib import Path
import re
import select
import sys
import time
import serial

parser = argparse.ArgumentParser()
parser.add_argument("--port", required=True)
parser.add_argument("--log", type=Path, required=True)
args = parser.parse_args()
args.log.parent.mkdir(parents=True, exist_ok=True)
device = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=1)
device.dtr = False
device.rts = False
device.port = args.port

with args.log.open("a", buffering=1) as log:
    def emit(message):
        line = f"{datetime.now(timezone.utc).isoformat()} {message}"
        print(line, flush=True)
        log.write(line + "\n")

    device.open()
    emit(f"HOST OPEN port={args.port}; keep this process and J2 connected")
    buffer = b""
    main_seen = 0.0
    stdin_open = True
    shutdown_seen_at = 0.0
    try:
        # Board initialization may reset on serial open. Status is read-only.
        time.sleep(1)
        device.write(b"status\n")
        while True:
            data = device.read(device.in_waiting or 1)
            if data:
                buffer += data
                while b"\n" in buffer:
                    raw, buffer = buffer.split(b"\n", 1)
                    line = raw.decode("utf-8", errors="replace").rstrip("\r")
                    emit(line)
                    if line.startswith("INTENTIONAL_BACKUP_SHUTDOWN "):
                        shutdown_seen_at = time.monotonic()
                    elif line.startswith("SHUTDOWN_RELEASE_FAILED "):
                        shutdown_seen_at = 0.0
                    if line.startswith("HEARTBEAT "):
                        fields = dict(re.findall(r"(\w+)=([^ ]+)", line))
                        main_seen = time.monotonic() if (
                            fields.get("MAIN") == "1" and fields.get("SS") == "1"
                            and (fields.get("USB") == "1" or fields.get("DC") == "1")
                        ) else 0.0
            if stdin_open and select.select([sys.stdin], [], [], 0)[0]:
                command = sys.stdin.readline()
                if not command:
                    stdin_open = False
                    emit("HOST stdin closed; serial capture continues")
                    continue
                command = command.strip()
                if command == "/quit":
                    if not main_seen or time.monotonic() - main_seen > 3:
                        emit("HOST quit refused: fresh main-power heartbeat required")
                        continue
                    emit("HOST CLOSE requested with main power present")
                    break
                if command:
                    emit(f"HOST SEND {command}")
                    device.write((command + "\n").encode("ascii"))
    except (serial.SerialException, OSError) as exc:
        if shutdown_seen_at and time.monotonic() - shutdown_seen_at < 3:
            emit(f"HOST EXPECTED_POWER_OFF after firmware shutdown marker: {exc}")
        else:
            emit(f"HOST SERIAL_LOST {exc}; no automatic reconnect or continuity claim")
            raise
    finally:
        if buffer:
            emit("HOST partial=" + buffer.decode("utf-8", errors="replace"))
        device.close()
