#!/usr/bin/env python3
"""Host-only protocol tests: no USB, pyserial dependency, or shutdown commands."""

import json
import io
import threading
import time
import unittest
from unittest.mock import patch

from pdb_client import JsonLines, MAX_RX_LINE, PdbClient, ShutdownHooks, UINT32_MAX


def envelope(kind="event", boot_id="AABB0011", **fields):
    return {"v": 1, "type": kind, "boot_id": boot_id, "seq": 1,
            "uptime_ms": 1000, **fields}


def shutdown(**fields):
    return envelope(event="shutdown_requested", shutdown_id=17, reason="MAIN_LOSS",
                    deadline_ms=61000, remaining_ms=60000, **fields)


class Clock:
    def __init__(self):
        self.now = 100.0

    def __call__(self):
        return self.now


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.sent, self.logs = [], []
        self.clock = Clock()
        self.client = self.make_client()

    def make_client(self, **options):
        return PdbClient(lambda data: self.sent.append(json.loads(data)),
                         report=self.logs.append, clock=self.clock,
                         first_request_id=100, **options)

    def commands(self):
        return [message["cmd"] for message in self.sent]

    def wait_for_worker(self, client):
        limit = time.monotonic() + 1
        while client.results.empty() and time.monotonic() < limit:
            time.sleep(0.001)
        self.assertFalse(client.results.empty(), "Worker did not finish")

    def test_fragmented_and_multiple_json_lines(self):
        framer = JsonLines(self.logs.append)
        self.assertEqual(framer.feed(b'{"a":'), [])
        self.assertEqual(framer.feed(b'1}\r\n{"b":2}\n'), [{"a": 1}, {"b": 2}])
        self.assertEqual(framer.feed(b'\n'), [])

    def test_oversize_frame_discards_through_newline_and_recovers(self):
        framer = JsonLines(self.logs.append)
        self.assertEqual(framer.feed(b"x" * MAX_RX_LINE), [])
        self.assertEqual(len(framer.buffer), MAX_RX_LINE)
        self.assertEqual(framer.feed(b'x{"looks":"valid"}'), [])
        self.assertEqual(len(framer.buffer), 0)
        self.assertEqual(framer.feed(b'\n{"ok":true}\n'), [{"ok": True}])
        self.assertEqual(len(self.logs), 1)

    def test_invalid_utf8_json_constants_duplicate_keys_and_arrays_ignored(self):
        framer = JsonLines(self.logs.append)
        self.assertEqual(framer.feed(b'\xff\n{"a":NaN}\n{"a":1e999}\n{"a":1,"a":2}\n[]\n{"ok":1}\n'), [{"ok": 1}])
        self.assertEqual(len(self.logs), 5)

    def test_boot_required_for_mutations_but_status_available(self):
        self.client.request("status")
        self.assertNotIn("boot_id", self.sent[0])
        with self.assertRaises(RuntimeError):
            self.client.request("reboot")
        self.client.receive(envelope("telemetry", state="RUNNING"))
        self.client.request("reboot")
        self.assertEqual(self.sent[-1]["boot_id"], "AABB0011")

    def test_pong_matches_current_boot_and_ping_without_manual_action(self):
        self.client.receive(envelope(event="ping", ping_id=55))
        self.assertEqual(self.sent, [{"v": 1, "id": 100, "cmd": "pong", "boot_id": "AABB0011", "ping_id": 55}])
        self.client.receive(envelope(event="ping", ping_id=True))
        self.assertEqual(len(self.sent), 1)

    def test_invalid_envelope_cannot_change_boot_or_trigger_command(self):
        for fields in ({"v": True}, {"boot_id": "not-hex"}, {"boot_id": "1234"},
                       {"boot_id": "aabb0011"}, {"seq": True}, {"uptime_ms": -1}):
            message = envelope(event="ping", ping_id=1)
            message.update(fields)
            self.assertFalse(self.client.receive(message))
        self.assertIsNone(self.client.boot_id)
        self.assertEqual(self.sent, [])

    def test_reserved_port_and_ambiguous_types_rejected_locally(self):
        self.client.receive(envelope("telemetry"))
        for fields in ({"port": "vbus_ss", "enabled": False},
                       {"port": "5v_vbus", "enabled": 1}):
            with self.assertRaises(ValueError):
                self.client.request("port_set", **fields)
        with self.assertRaises(ValueError):
            self.client.request("reboot", boot_id="1234")
        with self.assertRaises(ValueError):
            self.client.request("shutdown_ready", shutdown_id=True)
        self.assertEqual(self.sent, [])

    def test_async_response_and_result_retained_by_request_id(self):
        self.client.receive(envelope("telemetry"))
        request_id = self.client.request("port_set", port="5v_vbus", enabled=True)
        self.client.receive(envelope("response", id=request_id, ok=True, code="ACCEPTED"))
        self.client.receive(envelope(event="port_result", request_id=request_id,
                                     port="5v_vbus", enabled=True, ok=False, code="HARDWARE_ERROR"))
        self.assertEqual(self.client.responses[request_id]["code"], "ACCEPTED")
        self.assertFalse(self.client.port_results[request_id]["ok"])

    def test_history_bounded_and_request_id_wraps_without_zero(self):
        for request_id in range(200):
            self.client.receive(envelope("response", id=request_id, ok=True, code="OK"))
        self.assertEqual(len(self.client.responses), 128)
        self.client.next_id = UINT32_MAX
        self.assertEqual(self.client.request("status"), UINT32_MAX)
        self.assertEqual(self.client.request("status"), 1)

    def test_monitor_never_claims_ack_ready_or_powers_off(self):
        self.client.receive(shutdown())
        self.client.poll()
        self.assertEqual(self.sent, [])
        self.assertIsNone(self.client.hooks)

    def test_ack_only_mode_and_event_telemetry_deduplicate(self):
        client = self.make_client(ack_shutdown=True)
        client.receive(shutdown())
        client.receive(shutdown())
        fields = {key: value for key, value in shutdown().items() if key in
                  ("shutdown_id", "reason", "deadline_ms", "remaining_ms")}
        self.clock.now += 1
        fields["remaining_ms"] = 59000
        client.receive(envelope("telemetry", shutdown=fields))
        self.assertEqual(self.commands(), ["shutdown_ack"])
        self.assertEqual(client.attempt.deadline, 160.0)

    def test_reconnect_telemetry_alone_starts_shutdown_handler(self):
        client = self.make_client(ack_shutdown=True)
        client.receive(envelope("telemetry", shutdown={"shutdown_id": 17, "reason": "MAIN_LOSS",
                                                       "deadline_ms": 61000, "remaining_ms": 25000}))
        self.assertEqual(self.commands(), ["shutdown_ack"])
        self.assertEqual(client.attempt.deadline, 125.0)

    def test_cleanup_keeps_ping_service_active_and_poweroff_requires_ready_ok(self):
        release, entered, powered_off = threading.Event(), threading.Event(), threading.Event()

        def prepare(_request, _timeout):
            entered.set()
            release.wait(1)
            return True

        client = self.make_client(shutdown_hooks=ShutdownHooks(prepare, lambda: powered_off.set() or True))
        client.receive(shutdown())
        self.assertTrue(entered.wait(1))
        client.receive(envelope(event="ping", ping_id=18))
        self.assertEqual(self.commands(), ["shutdown_ack", "pong"])
        self.assertFalse(powered_off.is_set())
        release.set()
        self.wait_for_worker(client)
        client.poll()
        self.assertEqual(self.commands(), ["shutdown_ack", "pong", "shutdown_ready"])
        self.assertFalse(powered_off.is_set())
        client.receive(envelope("response", id=client.attempt.ready_id, ok=True, code="OK"))
        self.assertTrue(powered_off.wait(1))
        client.receive(shutdown())
        self.assertEqual(self.commands().count("shutdown_ready"), 1)

    def test_cleanup_failure_never_sends_ready(self):
        for outcome in (False, RuntimeError("cleanup failed")):
            with self.subTest(outcome=outcome):
                self.sent.clear()

                def prepare(_request, _timeout):
                    if isinstance(outcome, Exception):
                        raise outcome
                    return outcome

                client = self.make_client(shutdown_hooks=ShutdownHooks(prepare, lambda: self.fail("Poweroff was called")))
                client.receive(shutdown())
                self.wait_for_worker(client)
                client.poll()
                self.assertEqual(self.commands(), ["shutdown_ack"])
                self.assertTrue(client.attempt.failed)

    def test_timeout_discards_late_cleanup_result_and_does_not_extend_deadline(self):
        release = threading.Event()
        client = self.make_client(shutdown_hooks=ShutdownHooks(lambda *_: release.wait(1), lambda: True, 10))
        client.receive(shutdown())
        self.clock.now += 10
        client.poll()
        self.assertTrue(client.attempt.failed)
        release.set()
        self.wait_for_worker(client)
        client.poll()
        self.clock.now += 1
        client.receive(shutdown())  # Even a repeated 60s report cannot extend it.
        self.assertEqual(client.attempt.deadline, 160)
        self.assertEqual(self.commands(), ["shutdown_ack"])

    def test_short_remaining_time_bounds_cleanup_even_across_device_millis_wrap(self):
        seen = []
        client = self.make_client(shutdown_hooks=ShutdownHooks(lambda request, timeout: seen.append((request, timeout)) or False, lambda: True))
        message = shutdown()
        message.update(remaining_ms=5000, deadline_ms=100)
        client.receive(message)
        self.wait_for_worker(client)
        client.poll()
        self.assertEqual(seen[0][1], 4.0)
        self.assertEqual(client.attempt.deadline, 105.0)

    def test_boot_change_discards_old_cleanup_and_allows_new_shutdown_id(self):
        release = threading.Event()
        client = self.make_client(shutdown_hooks=ShutdownHooks(lambda *_: release.wait(1), lambda: self.fail("Old boot powered off host")))
        client.receive(shutdown())
        old_attempt = client.attempt
        client.receive(envelope("telemetry", boot_id="BBBB0022"))
        release.set()
        self.wait_for_worker(client)
        client.poll()
        self.assertIsNone(client.attempt)
        self.assertEqual(self.commands(), ["shutdown_ack"])
        self.assertEqual(old_attempt.request.boot_id, "AABB0011")

    def test_missing_or_rejected_ready_response_does_not_poweroff(self):
        for reply in (None, {"ok": False, "code": "STALE_SHUTDOWN"}):
            with self.subTest(reply=reply):
                self.sent.clear()
                client = self.make_client(shutdown_hooks=ShutdownHooks(lambda *_: True, lambda: self.fail("Poweroff was called")))
                client.receive(shutdown())
                self.wait_for_worker(client)
                client.poll()
                if reply:
                    client.receive(envelope("response", id=client.attempt.ready_id, **reply))
                else:
                    self.clock.now += 1.0
                    client.poll()
                self.assertTrue(client.attempt.failed)

    def test_expired_shutdown_never_runs_cleanup_or_ack(self):
        client = self.make_client(shutdown_hooks=ShutdownHooks(lambda *_: self.fail("Cleanup was called"), lambda: True))
        message = shutdown()
        message["remaining_ms"] = 0
        client.receive(message)
        self.assertEqual(self.sent, [])
        self.assertTrue(client.attempt.failed)

    def test_hook_timeout_validation(self):
        for timeout in (0, -1, 46, float("nan"), float("inf")):
            with self.assertRaises(ValueError):
                self.make_client(shutdown_hooks=ShutdownHooks(lambda *_: True, lambda: True, timeout))

    def test_usb_open_asserts_stable_lines_and_linux_exclusive_without_reset(self):
        from pdb_client import open_serial

        class FakeSerial:
            def __init__(self, **options):
                self.options = options
                self.events = []

            def open(self):
                self.events.append((self.port, self.dtr, self.rts))

        class FakeModule:
            Serial = FakeSerial

        with patch.dict("sys.modules", {"serial": FakeModule}), patch("sys.platform", "linux"):
            connection = open_serial("/dev/serial/by-id/test")
        self.assertEqual(connection.events, [("/dev/serial/by-id/test", True, True)])
        self.assertTrue(connection.options["exclusive"])
        self.assertEqual(connection.options["baudrate"], 115200)

    def test_cli_requires_explicit_complete_poweroff_opt_in_before_usb_open(self):
        from pdb_client import main
        with patch("pdb_client.open_serial") as opened, patch("sys.stderr", io.StringIO()):
            with self.assertRaises(SystemExit):
                main(["--device", "fake", "--poweroff-command", "poweroff"])
            opened.assert_not_called()

    def test_transport_read_error_exits_without_reopen_or_mutation(self):
        from pdb_client import main
        sent = []

        class FakeConnection:
            closed = False

            def write(self, data):
                sent.append(json.loads(data))
                return len(data)

            def read(self, _size):
                raise OSError("Device disconnected")

            def close(self):
                self.closed = True

        connection = FakeConnection()
        with patch("pdb_client.open_serial", return_value=connection) as opened, patch("sys.stderr", io.StringIO()):
            self.assertEqual(main(["--device", "fake", "monitor"]), 2)
            opened.assert_called_once_with("fake")
        self.assertTrue(connection.closed)
        self.assertEqual([message["cmd"] for message in sent], ["status"])

    def test_partial_request_write_is_fatal_and_never_retried(self):
        from pdb_client import main
        writes = []

        class FakeConnection:
            def write(self, data):
                writes.append(data)
                return len(data) - 1

            def close(self):
                pass

        with patch("pdb_client.open_serial", return_value=FakeConnection()), patch("sys.stderr", io.StringIO()):
            self.assertEqual(main(["--device", "fake", "reboot"]), 2)
        self.assertEqual(len(writes), 1)
        self.assertEqual(json.loads(writes[0])["cmd"], "status")


if __name__ == "__main__":
    unittest.main()
