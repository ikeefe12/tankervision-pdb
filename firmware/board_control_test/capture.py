#!/usr/bin/env python3
"""Capture serial evidence. USB serial open/close may reset ESP32; requires pyserial."""
import argparse
from datetime import datetime, timezone
from pathlib import Path
import time
import serial

parser = argparse.ArgumentParser()
parser.add_argument("--port", required=True)
parser.add_argument("--command", default="status")
parser.add_argument("--seconds", type=float, default=10)
parser.add_argument("--log", type=Path, required=True)
args = parser.parse_args()
args.log.parent.mkdir(parents=True, exist_ok=True)
with args.log.open("w") as log:
    def emit(text):
        print(text, end="", flush=True)
        log.write(text)
        log.flush()
    emit(f"UTC {datetime.now(timezone.utc).isoformat()} port={args.port} command={args.command}\n")
    device = serial.Serial(port=None, baudrate=115200, timeout=0.2)
    device.dtr = False
    device.rts = False
    device.port = args.port
    with device:
        time.sleep(1)
        device.write((args.command + "\n").encode("ascii"))
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            data = device.read(device.in_waiting or 1)
            if data:
                emit(data.decode("utf-8", errors="replace"))
