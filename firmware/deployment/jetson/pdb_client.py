#!/usr/bin/env python3
"""Tankervision PDB protocol v1 client. Importing this module never opens USB.

The default CLI monitors and answers pings. Host shutdown requires explicit
--allow-host-poweroff, --cleanup-command, and --poweroff-command options.
"""

from __future__ import annotations

import argparse
from collections import OrderedDict
from dataclasses import dataclass
import json
import math
import queue
import secrets
import shlex
import subprocess
import sys
import threading
import time
from typing import Callable

MAX_RX_LINE = 8192
MAX_TX_LINE = 512
UINT32_MAX = 0xFFFFFFFF
PORTS = ("vbus", "5v_vbus", "5v_ss")


def uint32(value: object, *, positive: bool = False) -> bool:
    return type(value) is int and (1 if positive else 0) <= value <= UINT32_MAX


def reject_constant(value: str) -> None:
    raise ValueError(f"Non-JSON constant {value}")


def finite_float(value: str) -> float:
    number = float(value)
    if not math.isfinite(number):
        raise ValueError("JSON number exceeds finite float range")
    return number


def unique_object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate JSON key {key}")
        result[key] = value
    return result


class JsonLines:
    """Bound memory use; discard an oversized line through its next LF."""

    def __init__(self, report: Callable[[str], None] = lambda _: None):
        self.buffer = bytearray()
        self.discarding = False
        self.report = report

    def feed(self, data: bytes) -> list[dict]:
        messages = []
        # Serial reads are bounded by the caller; a single oversized frame never
        # accumulates in self.buffer, including across multiple reads.
        for chunk_index, chunk in enumerate(data.split(b"\n")):
            if chunk_index:
                if not self.discarding and self.buffer:
                    try:
                        message = json.loads(
                            self.buffer.decode("utf-8"),
                            parse_constant=reject_constant,
                            parse_float=finite_float,
                            object_pairs_hook=unique_object,
                        )
                        if not isinstance(message, dict):
                            raise ValueError("Expected a JSON object")
                        messages.append(message)
                    except (UnicodeError, ValueError, RecursionError) as error:
                        self.report(f"Ignored invalid JSON line: {error}")
                self.buffer.clear()
                self.discarding = False
            if not self.discarding:
                if len(self.buffer) + len(chunk) > MAX_RX_LINE:
                    self.buffer.clear()
                    self.discarding = True
                    self.report("Discarded oversized USB line")
                else:
                    self.buffer.extend(chunk)
        return messages


@dataclass(frozen=True)
class ShutdownRequest:
    boot_id: str
    shutdown_id: int
    reason: str
    deadline_ms: int
    remaining_ms: int


@dataclass(frozen=True)
class ShutdownHooks:
    # prepare runs in a daemon worker, leaving USB reception/pongs active.
    # It must honor timeout itself if it starts any work that must be cancelled.
    prepare: Callable[[ShutdownRequest, float], bool]
    poweroff: Callable[[], bool]
    timeout: float = 45.0


@dataclass
class _ShutdownAttempt:
    request: ShutdownRequest
    deadline: float
    started: float
    timeout: float
    failed: bool = False
    cleanup_done: bool = False
    ready_id: int | None = None
    ready_sent: float = 0.0
    poweroff_started: bool = False


class PdbClient:
    """Transport-independent client. Call receive() and poll() from one thread.

    send_bytes must write the entire buffer or raise. A partial failed send is
    fatal to that transport: never retry the remaining line on another session.
    No serial handle or poweroff command is created by this class.
    """

    def __init__(self, send_bytes: Callable[[bytes], None], *,
                 report: Callable[[str], None] = lambda _: None,
                 on_message: Callable[[dict], None] = lambda _: None,
                 clock: Callable[[], float] = time.monotonic,
                 shutdown_hooks: ShutdownHooks | None = None,
                 ack_shutdown: bool = False, first_request_id: int | None = None):
        if shutdown_hooks and (not math.isfinite(shutdown_hooks.timeout)
                               or not 0 < shutdown_hooks.timeout <= 45):
            raise ValueError("Cleanup timeout must be greater than 0 and at most 45 seconds")
        self.send_bytes = send_bytes
        self.report = report
        self.on_message = on_message
        self.clock = clock
        self.hooks = shutdown_hooks
        self.ack_shutdown = ack_shutdown or shutdown_hooks is not None
        self.boot_id: str | None = None
        self.telemetry: dict | None = None
        self.next_id = first_request_id if first_request_id is not None else secrets.randbelow(UINT32_MAX) + 1
        if not uint32(self.next_id, positive=True):
            raise ValueError("Invalid initial request id")
        self.responses: OrderedDict[int, dict] = OrderedDict()
        self.port_results: OrderedDict[int, dict] = OrderedDict()
        self.attempt: _ShutdownAttempt | None = None
        self.results: queue.Queue = queue.Queue()

    def request(self, command: str, **fields: object) -> int:
        allowed = {
            "status": set(), "pong": {"ping_id"},
            "shutdown_ack": {"shutdown_id"}, "shutdown_ready": {"shutdown_id"},
            "port_set": {"port", "enabled"}, "reboot": set(),
        }
        if command not in allowed or set(fields) != allowed[command]:
            raise ValueError("Unknown command or incorrect command fields")
        if command != "status" and self.boot_id is None:
            raise RuntimeError("Read the device boot_id before sending a mutation")
        if command == "port_set":
            if fields["port"] not in PORTS or type(fields["enabled"]) is not bool:
                raise ValueError("Invalid controllable port or non-boolean enabled value")
        for name in ("ping_id", "shutdown_id"):
            if name in fields and not uint32(fields[name]):
                raise ValueError(f"Invalid {name}")
        request_id = self.next_id
        self.next_id = 1 if request_id == UINT32_MAX else request_id + 1
        message = {"v": 1, "id": request_id, "cmd": command}
        if command != "status":
            message["boot_id"] = self.boot_id
        message.update(fields)
        encoded = json.dumps(message, separators=(",", ":"), allow_nan=False).encode("utf-8")
        if len(encoded) > MAX_TX_LINE:
            raise ValueError("Request exceeds firmware line limit")
        self.send_bytes(encoded + b"\n")
        return request_id

    @staticmethod
    def _remember(history: OrderedDict, key: int, message: dict) -> None:
        history[key] = message
        history.move_to_end(key)
        while len(history) > 128:
            history.popitem(last=False)

    def receive(self, message: dict) -> bool:
        """Validate a framed device object, process it, then notify the observer."""
        boot = message.get("boot_id")
        if (type(message.get("v")) is not int or message["v"] != 1
                or message.get("type") not in ("telemetry", "event", "response")
                or not isinstance(boot, str) or len(boot) != 8
                or any(c not in "0123456789ABCDEF" for c in boot)
                or not uint32(message.get("seq")) or not uint32(message.get("uptime_ms"))):
            self.report("Ignored message with invalid protocol envelope")
            return False
        kind = message["type"]
        if kind == "response" and (not uint32(message.get("id"))
                                   or type(message.get("ok")) is not bool
                                   or not isinstance(message.get("code"), str)):
            self.report("Ignored invalid response")
            return False
        if self.boot_id != boot:
            if self.boot_id is not None:
                self.report(f"Device restarted: boot_id {self.boot_id} -> {boot}")
            self.boot_id = boot
            self.telemetry = None
            self.responses.clear()
            self.port_results.clear()
            self.attempt = None  # Old cleanup completion must never power off a new boot.
        if kind == "response":
            self._remember(self.responses, message["id"], message)
            attempt = self.attempt
            if attempt and message["id"] == attempt.ready_id:
                if message["ok"] and message["code"] == "OK":
                    self._start_poweroff(attempt)
                else:
                    attempt.failed = True
                    self.report(f"Final-ready was rejected: {message['code']}; host poweroff not started")
        elif kind == "telemetry":
            self.telemetry = message
            if isinstance(message.get("shutdown"), dict):
                self._shutdown(message["shutdown"])
        elif message.get("event") == "ping":
            if uint32(message.get("ping_id")):
                self.request("pong", ping_id=message["ping_id"])
            else:
                self.report("Ignored ping with invalid ping_id")
        elif message.get("event") == "shutdown_requested":
            self._shutdown(message)
        elif message.get("event") == "port_result":
            if (uint32(message.get("request_id"), positive=True)
                    and message.get("port") in PORTS
                    and type(message.get("enabled")) is bool
                    and type(message.get("ok")) is bool
                    and isinstance(message.get("code"), str)):
                self._remember(self.port_results, message["request_id"], message)
            else:
                self.report("Ignored invalid port_result")
        self.on_message(message)
        return True

    def _shutdown(self, fields: dict) -> None:
        remaining = fields.get("remaining_ms")
        if (not uint32(fields.get("shutdown_id")) or not uint32(fields.get("deadline_ms"))
                or not uint32(remaining) or remaining > 60000
                or fields.get("reason") not in ("MAIN_LOSS", "FULL_REBOOT")):
            self.report("Ignored invalid shutdown request")
            return
        now = self.clock()
        request = ShutdownRequest(self.boot_id, fields["shutdown_id"], fields["reason"],
                                  fields["deadline_ms"], remaining)
        if self.attempt:
            if (self.attempt.request.boot_id, self.attempt.request.shutdown_id) != (request.boot_id, request.shutdown_id):
                self.report("Ignored conflicting shutdown id within one boot")
                return
            # Repeated telemetry cannot extend the host's estimate of the deadline.
            self.attempt.deadline = min(self.attempt.deadline, now + remaining / 1000)
            return
        timeout = min(self.hooks.timeout if self.hooks else 45.0, max(0.0, remaining / 1000 - 1.0))
        attempt = self.attempt = _ShutdownAttempt(request, now + remaining / 1000, now, timeout)
        self.report(f"Shutdown {request.shutdown_id}: {request.reason}, {remaining} ms remaining")
        if self.ack_shutdown and remaining:
            self.request("shutdown_ack", shutdown_id=request.shutdown_id)
        if not self.hooks:
            self.report("Host shutdown is not enabled; final-ready will not be sent")
            return
        if timeout <= 0:
            attempt.failed = True
            self.report("Too little time remains to prepare host shutdown")
            return

        def prepare() -> None:
            try:
                result = self.hooks.prepare(request, timeout)
                self.results.put((attempt, "prepare", result is True, ""))
            except Exception as error:
                self.results.put((attempt, "prepare", False, str(error)))

        threading.Thread(target=prepare, name="pdb-shutdown-prepare", daemon=True).start()

    def _start_poweroff(self, attempt: _ShutdownAttempt) -> None:
        if (attempt is not self.attempt or attempt.failed or attempt.poweroff_started
                or self.clock() >= attempt.deadline
                or self.clock() - attempt.ready_sent >= 1.0 or not self.hooks):
            return
        attempt.poweroff_started = True
        self.report("Final-ready accepted; starting the explicitly configured host poweroff action")

        def poweroff() -> None:
            try:
                self.results.put((attempt, "poweroff", self.hooks.poweroff() is True, ""))
            except Exception as error:
                self.results.put((attempt, "poweroff", False, str(error)))

        threading.Thread(target=poweroff, name="pdb-host-poweroff", daemon=True).start()

    def poll(self) -> None:
        """Service worker completions/timeouts; never waits for a cleanup worker."""
        now = self.clock()
        attempt = self.attempt
        if attempt and self.hooks and not attempt.failed and not attempt.poweroff_started:
            if now >= attempt.deadline:
                attempt.failed = True
                self.report("Shutdown deadline reached; no further host action will be started")
            elif not attempt.cleanup_done and now - attempt.started >= attempt.timeout:
                attempt.failed = True
                self.report("Cleanup timed out; final-ready will not be sent")
            elif attempt.ready_id is not None and now - attempt.ready_sent >= 1.0:
                attempt.failed = True
                self.report("Final-ready response timed out; host poweroff not started")
        while True:
            try:
                finished, stage, ok, detail = self.results.get_nowait()
            except queue.Empty:
                break
            if finished is not self.attempt or finished.failed:
                continue
            if not ok:
                finished.failed = True
                self.report(f"Host {stage} action failed: {detail or 'non-success return'}")
            elif stage == "prepare":
                finished.cleanup_done = True
                # Readiness is sent once, after cleanup, immediately before the
                # explicit poweroff action (which waits at most 1 s for its OK).
                finished.ready_id = self.request("shutdown_ready", shutdown_id=finished.request.shutdown_id)
                finished.ready_sent = self.clock()
            else:
                self.report("Host poweroff action accepted by the operating system")


def run_command(argv: list[str], timeout: float) -> bool:
    """Run an explicit argument list, without a shell or implicit privileges."""
    result = subprocess.run(argv, timeout=timeout, check=False,
                            stdout=sys.stderr, stderr=sys.stderr)
    return result.returncode == 0


def open_serial(device: str):
    # Deferred import allows pure protocol tests without pyserial installed.
    import serial

    options = {"baudrate": 115200, "timeout": 0.1, "write_timeout": 0.2,
               "xonxoff": False, "rtscts": False, "dsrdtr": False}
    if sys.platform.startswith("linux"):
        options["exclusive"] = True
    connection = serial.Serial(port=None, **options)
    # Arduino TinyUSB CDC requires asserted control lines. Reboot hooks must
    # already be disabled in the deployment firmware before USB.begin().
    connection.dtr = True
    connection.rts = True
    connection.port = device
    connection.open()
    return connection


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True, help="Deployment CDC device, preferably /dev/serial/by-id/...")
    parser.add_argument("--log", help="Append received protocol JSON lines to this file")
    parser.add_argument("--ack-shutdown", action="store_true", help="ACK requests without claiming final-ready or powering off")
    parser.add_argument("--allow-host-poweroff", action="store_true", help="Explicitly enable the configured host poweroff action")
    parser.add_argument("--cleanup-command", help="Explicit cleanup argv, shell quoting accepted; no shell is run")
    parser.add_argument("--poweroff-command", help="Explicit poweroff argv, e.g. '/usr/bin/systemctl poweroff'")
    parser.add_argument("--cleanup-timeout", type=float, default=45, help="Cleanup timeout in seconds, maximum 45")
    commands = parser.add_subparsers(dest="command")
    commands.add_parser("monitor", help="Stream telemetry and answer pings (default)")
    commands.add_parser("status", help="Read one current telemetry snapshot")
    port_parser = commands.add_parser("port", help="Set an optional external output")
    port_parser.add_argument("port_name", choices=PORTS)
    port_parser.add_argument("enabled", choices=("on", "off"))
    commands.add_parser("reboot", help="Request the board's fixed 60-second full-reboot sequence")
    args = parser.parse_args(argv)
    configured = (bool(args.cleanup_command), bool(args.poweroff_command), args.allow_host_poweroff)
    if any(configured) and not all(configured):
        parser.error("Host shutdown requires --allow-host-poweroff, --cleanup-command, and --poweroff-command together")
    if not math.isfinite(args.cleanup_timeout) or not 0 < args.cleanup_timeout <= 45:
        parser.error("--cleanup-timeout must be greater than 0 and at most 45 seconds")
    hooks = None
    if all(configured):
        cleanup_argv, poweroff_argv = shlex.split(args.cleanup_command), shlex.split(args.poweroff_command)
        if not cleanup_argv or not poweroff_argv:
            parser.error("Explicit shutdown commands must not be empty")
        hooks = ShutdownHooks(lambda _request, timeout: run_command(cleanup_argv, timeout),
                              lambda: run_command(poweroff_argv, 5.0), args.cleanup_timeout)
    log_file = open(args.log, "a", encoding="utf-8", buffering=1) if args.log else None

    def report(text: str) -> None:
        print(f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} {text}", file=sys.stderr, flush=True)

    def display(message: dict) -> None:
        line = json.dumps(message, separators=(",", ":"), allow_nan=False)
        print(line, flush=True)
        if log_file:
            log_file.write(line + "\n")

    connection = None
    try:
        connection = open_serial(args.device)

        def send(data: bytes) -> None:
            if connection.write(data) != len(data):
                raise OSError("Partial USB write; stopping without retrying a mutation")

        client = PdbClient(send, report=report, on_message=display,
                           shutdown_hooks=hooks, ack_shutdown=args.ack_shutdown)
        framer = JsonLines(report)
        report("Opened deployment TinyUSB CDC; control lines remain asserted, no reset requested")
        client.request("status")
        command = args.command or "monitor"
        request_id = None
        started = time.monotonic()
        while True:
            data = connection.read(4096)
            for message in framer.feed(data):
                client.receive(message)
            client.poll()
            if command == "status" and client.telemetry is not None:
                return 0
            if client.telemetry is not None and request_id is None:
                if command == "port":
                    request_id = client.request("port_set", port=args.port_name, enabled=args.enabled == "on")
                elif command == "reboot":
                    request_id = client.request("reboot")
            if request_id in client.responses:
                response = client.responses[request_id]
                if not response["ok"]:
                    return 1
                if command == "reboot":
                    return 0
                if request_id in client.port_results:
                    return 0 if client.port_results[request_id]["ok"] else 1
            if command != "monitor" and time.monotonic() - started > 10:
                report("Command result timed out; no automatic resend or reset was attempted")
                return 2
    except KeyboardInterrupt:
        return 0
    except Exception as error:
        report(f"Client stopped: {error}; no automatic reset or reopen was attempted")
        return 2
    finally:
        if connection is not None:
            connection.close()
        if log_file:
            log_file.close()


if __name__ == "__main__":
    raise SystemExit(main())
