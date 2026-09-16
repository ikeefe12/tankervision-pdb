#!/usr/bin/env python3
"""Capture J1 loss/20-second cutoff/return while leaving the J2 cable untouched.

Usage: capture_usb_recovery.py --port /dev/cu.usbmodem2101 --log results/log.txt
Requires pyserial. Type firmware commands on stdin; charging never starts here
automatically. The initial serial open is immediate and assumes main is on.

An I/O error or silence never triggers close/reopen. Only an observed /dev node
disappearance permits closing the stale handle. After reappearance, wait 25 s
(--settle-seconds) for firmware to record its early boot diagnostic, then attempt
one open with DTR/RTS false. Even this open may reset ESP32; logs mark that fact.
Read-only status and usb-status probes run 3 s and 15 s after each successful
open. Allow 60 s after restoring J1, keeping J2 untouched, to capture recovery.

/reopen retries an unsuccessful recovery open only after a confirmed disappearance
and the settle interval. It never closes a live handle. /quit (also Ctrl-C) needs
a fresh main-powered/idle heartbeat, or a currently absent device after confirmed
disappearance. EOF leaves capture running. No reset, flashing or USB reset tools
are invoked by this program.
"""

import argparse
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import select
import signal
import sys
import time


@dataclass(frozen=True)
class NodeIdentity:
    device: int
    inode: int
    rdevice: int


def node_identity(port):
    """Only ENOENT means disappearance; permission/I/O errors remain unknown."""
    try:
        info = os.stat(port)
    except FileNotFoundError:
        return None
    return NodeIdentity(info.st_dev, info.st_ino, info.st_rdev)


class RecoveryCapture:
    """Small event loop with injected I/O/time for tests that never touch USB."""

    def __init__(self, port, emit, serial_factory, metadata, clock=time.monotonic,
                 stat=node_identity, settle_seconds=25.0, io_errors=(OSError,)):
        self.port, self.emit, self.serial_factory = port, emit, serial_factory
        self.metadata, self.clock, self.stat = metadata, clock, stat
        self.settle_seconds, self.io_errors = settle_seconds, io_errors
        self.device = None
        self.node = None
        self.arrived_at = self.absent_at = None
        self.absence_confirmed = False
        self.disappearances = 0
        self.opened_generation = -1
        self.attempted_generation = -1
        self.expected_usb = None
        self.opened_at = self.last_rx_at = None
        self.next_read_at = self.last_read_error_at = 0.0
        self.probes_sent = set()
        self.buffer = b""
        self.rx_bytes = self.tx_bytes = self.read_errors = self.open_count = 0
        self.main_seen = self.idle_seen = None
        self.firmware_session = None
        self.last_poll_log_at = None
        self.last_stat_error = None
        self.shutdown_marker = False

    def _usb(self):
        try:
            return self.metadata(self.port)
        except self.io_errors as exc:
            self.emit(f"HOST USB_METADATA_ERROR {exc}")
            return None

    def _open(self, reason):
        if self.device is not None:
            self.emit("HOST OPEN_REFUSED an existing handle is still retained")
            return False
        self.attempted_generation = self.disappearances
        usb = self._usb()
        if self.expected_usb and usb:
            for key in ("vid", "pid", "serial_number"):
                expected = self.expected_usb.get(key)
                if expected is not None and usb.get(key) != expected:
                    self.emit(f"HOST OPEN_REFUSED USB identity mismatch field={key} "
                              f"expected={expected!r} observed={usb.get(key)!r}")
                    return False
        if self.expected_usb is None and usb:
            self.expected_usb = usb
        self.emit(f"HOST OPEN_ATTEMPT reason={reason} generation={self.disappearances} "
                  f"DTR=0 RTS=0 usb={json.dumps(usb, sort_keys=True)} "
                  "possible_reset_due_to_open=1")
        candidate = self.serial_factory()
        candidate.dtr = False
        candidate.rts = False
        candidate.port = self.port
        try:
            candidate.open()
        except self.io_errors as exc:
            # pyserial handles failed-open cleanup. Do not retry automatically.
            self.emit(f"HOST OPEN_FAILED {exc}; no automatic retry until another disappearance")
            return False
        self.device = candidate
        self.opened_generation = self.disappearances
        self.open_count += 1
        self.opened_at = self.clock()
        self.last_rx_at = None
        self.next_read_at = self.opened_at
        self.probes_sent.clear()
        self.main_seen = self.idle_seen = None
        self.emit(f"HOST OPEN_OK open={self.open_count} generation={self.disappearances}; "
                  "no continuity claim across this host open")
        return True

    def start(self):
        now = self.clock()
        self.node = self.stat(self.port)
        if self.node is None:
            raise RuntimeError("Initial serial node is absent; start with main power present")
        self.arrived_at = now
        self.emit(f"HOST INITIAL_NODE port={self.port} identity={json.dumps(asdict(self.node))}")
        self.emit(f"HOST POLICY recovery_settle_s={self.settle_seconds:g} "
                  "probes_after_open_s=3,15 auto_charge=0")
        self._open("initial main-powered attachment")

    def _close_absent(self):
        if self.buffer:
            self.emit("HOST PARTIAL_BEFORE_DISAPPEARANCE " +
                      self.buffer.decode("utf-8", errors="replace"))
            self.buffer = b""
        if self.device is not None:
            self.emit("HOST CLOSE_STALE_HANDLE only after confirmed node disappearance")
            try:
                self.device.close()
            except self.io_errors as exc:
                self.emit(f"HOST CLOSE_ERROR {exc}")
            self.device = None
        self.main_seen = self.idle_seen = None

    def poll_node(self):
        now = self.clock()
        try:
            observed = self.stat(self.port)
        except OSError as exc:
            error = str(exc)
            if error != self.last_stat_error:
                self.emit(f"HOST NODE_STAT_ERROR {error}; absence is not established")
                self.last_stat_error = error
            return
        self.last_stat_error = None
        if observed is None:
            if self.absent_at is None:
                self.absent_at = now
                self.arrived_at = None
                self.emit("HOST DEVICE_ABSENT_OBSERVED waiting for a second absent poll")
            if not self.absence_confirmed and now - self.absent_at >= 0.2:
                self.absence_confirmed = True
                self.disappearances += 1
                self.emit(f"HOST DEVICE_DISAPPEARED generation={self.disappearances} "
                          f"after_shutdown_marker={int(self.shutdown_marker)} "
                          f"last_identity={json.dumps(asdict(self.node)) if self.node else 'null'}")
                self.node = None
                self._close_absent()
            return
        if self.absent_at is not None:
            confirmed = self.absence_confirmed
            self.absent_at = None
            self.absence_confirmed = False
            self.arrived_at = now
            self.emit(f"HOST DEVICE_ARRIVED confirmed_prior_disappearance={int(confirmed)} "
                      f"identity={json.dumps(asdict(observed))} "
                      f"usb={json.dumps(self._usb(), sort_keys=True)}")
            if not confirmed:
                self.emit("HOST KEEP_HANDLE disappearance was too brief to confirm; no reconnect")
        elif self.node is not None and observed != self.node:
            self.emit(f"HOST NODE_IDENTITY_CHANGED old={json.dumps(asdict(self.node))} "
                      f"new={json.dumps(asdict(observed))}; no absent node observed, no reconnect")
        self.node = observed
        if (self.device is None and self.disappearances > 0
                and self.attempted_generation != self.disappearances
                and self.arrived_at is not None and now - self.arrived_at >= self.settle_seconds):
            self._open("settled reappearance after confirmed disappearance")

    def receive(self, data):
        now = self.clock()
        self.rx_bytes += len(data)
        self.last_rx_at = now
        self.emit(f"HOST RX_BYTES open={self.open_count} count={len(data)} total={self.rx_bytes}")
        self.buffer += data
        while b"\n" in self.buffer:
            raw, self.buffer = self.buffer.split(b"\n", 1)
            line = raw.decode("utf-8", errors="replace").rstrip("\r")
            self.emit(line)
            if line.startswith("INTENTIONAL_BACKUP_SHUTDOWN "):
                self.shutdown_marker = True
            elif line.startswith("SHUTDOWN_RELEASE_FAILED "):
                self.shutdown_marker = False
            if line.startswith("HEARTBEAT "):
                fields = dict(re.findall(r"(\w+)=([^ ]+)", line))
                main = (fields.get("MAIN") == "1" and fields.get("SS") == "1"
                        and (fields.get("USB") == "1" or fields.get("DC") == "1"))
                idle = (fields.get("state") == "STOPPED" and fields.get("EN42") == "0"
                        and fields.get("OUT") == "0x00" and fields.get("EOUT") == "0x00")
                self.main_seen = now if main else None
                self.idle_seen = now if idle else None
                session = fields.get("session")
                if session and session != self.firmware_session:
                    self.emit(f"HOST FIRMWARE_SESSION old={self.firmware_session} new={session} "
                              f"open={self.open_count}")
                    self.firmware_session = session
        if len(self.buffer) > 65536:
            self.emit("HOST PARTIAL_OVERSIZE " + self.buffer.decode("utf-8", errors="replace"))
            self.buffer = b""

    def send(self, command, origin="operator"):
        if self.device is None or self.absent_at is not None:
            self.emit(f"HOST SEND_REFUSED no present open handle command={command!r}")
            return False
        if not command or len(command) > 128 or any(ord(c) < 32 or ord(c) > 126 for c in command):
            self.emit("HOST SEND_REFUSED expected 1..128 printable ASCII characters")
            return False
        payload = (command + "\n").encode("ascii")
        self.emit(f"HOST SEND origin={origin} open={self.open_count} command={command!r} bytes={len(payload)}")
        try:
            written = self.device.write(payload)
        except self.io_errors as exc:
            self.emit(f"HOST WRITE_ERROR {exc}; handle retained, no reopen")
            return False
        self.tx_bytes += written
        self.emit(f"HOST TX_BYTES count={written} total={self.tx_bytes}")
        if command == "transfer-start":
            self.idle_seen = None
            self.shutdown_marker = False
        return written == len(payload)

    def can_quit(self):
        now = self.clock()
        fresh = any(seen is not None and now - seen <= 3
                    for seen in (self.main_seen, self.idle_seen))
        return fresh or (self.absence_confirmed and self.node is None and self.device is None)

    def command(self, command):
        """Return True only for an accepted /quit; ordinary commands keep running."""
        command = command.strip()
        if command == "/quit":
            if not self.can_quit():
                self.emit("HOST QUIT_REFUSED need a fresh main/idle heartbeat or confirmed current disappearance")
                return False
            self.emit("HOST QUIT accepted with verified safe close condition")
            if self.device is not None:
                self.device.close()
                self.device = None
            return True
        if command == "/reopen":
            if (self.device is not None or self.disappearances == 0 or self.node is None
                    or self.opened_generation == self.disappearances or self.arrived_at is None
                    or self.clock() - self.arrived_at < self.settle_seconds):
                self.emit("HOST REOPEN_REFUSED requires an unused confirmed disappearance, "
                          "a settled present node, and no retained handle")
            else:
                self._open("explicit /reopen after unsuccessful recovery open")
            return False
        if command:
            self.send(command)
        return False

    def tick(self):
        self.poll_node()
        now = self.clock()
        if self.device is not None and self.absent_at is None and now >= self.next_read_at:
            try:
                data = self.device.read(min(self.device.in_waiting or 1, 4096))
                if data:
                    self.receive(data)
            except self.io_errors as exc:
                self.read_errors += 1
                if self.read_errors == 1 or now - self.last_read_error_at >= 5:
                    self.emit(f"HOST READ_ERROR count={self.read_errors} error={exc}; "
                              "handle retained until confirmed node disappearance")
                    self.last_read_error_at = now
                self.next_read_at = now + 1
        if self.device is not None and self.opened_at is not None:
            for at in (3, 15):
                if at not in self.probes_sent and now - self.opened_at >= at:
                    self.probes_sent.add(at)
                    for command in ("status", "usb-status"):
                        self.send(command, f"bounded probe at {at}s after open")
        if self.last_poll_log_at is None or now - self.last_poll_log_at >= 5:
            self.last_poll_log_at = now
            silent = None if self.last_rx_at is None else round(now - self.last_rx_at, 3)
            self.emit(f"HOST POLL present={int(self.node is not None and self.absent_at is None)} "
                      f"handle={int(self.device is not None)} generation={self.disappearances} "
                      f"open={self.open_count} rx_bytes={self.rx_bytes} tx_bytes={self.tx_bytes} "
                      f"silent_s={silent} firmware_session={self.firmware_session}")


def main():
    import serial
    from serial.tools import list_ports

    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", required=True)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--settle-seconds", type=float, default=25)
    args = parser.parse_args()
    if not 0 <= args.settle_seconds <= 300:
        parser.error("--settle-seconds must be between 0 and 300")
    args.log.parent.mkdir(parents=True, exist_ok=True)
    quit_signal = False
    stdout_open = True

    def interrupted(_signum, _frame):
        nonlocal quit_signal
        quit_signal = True

    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    with args.log.open("a", buffering=1) as log:
        def emit(message):
            nonlocal stdout_open
            line = f"{datetime.now(timezone.utc).isoformat()} mono={time.monotonic():.6f} {message}"
            log.write(line + "\n")
            if stdout_open:
                try:
                    print(line, flush=True)
                except BrokenPipeError:
                    stdout_open = False
                    log.write("HOST stdout closed; capture remains active\n")

        def metadata(port):
            for entry in list_ports.comports():
                if entry.device == port:
                    return {key: getattr(entry, key, None) for key in
                            ("vid", "pid", "serial_number", "location", "manufacturer", "product")}
            return None

        capture = RecoveryCapture(args.port, emit,
                                  lambda: serial.Serial(port=None, baudrate=115200,
                                                        timeout=0.05, write_timeout=0.25),
                                  metadata, settle_seconds=args.settle_seconds,
                                  io_errors=(OSError, serial.SerialException))
        capture.start()
        stdin_open = True
        while True:
            capture.tick()
            if quit_signal:
                quit_signal = False
                if capture.command("/quit"):
                    break
            if stdin_open and select.select([sys.stdin], [], [], 0)[0]:
                command = sys.stdin.readline()
                if not command:
                    stdin_open = False
                    emit("HOST stdin closed; capture continues")
                elif capture.command(command):
                    break
            time.sleep(0.05)


if __name__ == "__main__":
    main()
