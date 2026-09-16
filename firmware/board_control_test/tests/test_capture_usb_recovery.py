"""Host logger regression tests; no serial ports, sleeps, or hardware access."""
import importlib.util
from pathlib import Path
import sys
import unittest

path = Path(__file__).resolve().parents[1] / "capture_usb_recovery.py"
spec = importlib.util.spec_from_file_location("capture_usb_recovery", path)
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)


class FakeSerial:
    def __init__(self, fixture):
        self.fixture = fixture
        self.dtr = self.rts = True
        self.port = None
        self.read_error = False
        self.write_error = False
        self.rx = b""
        self.writes = []
        self.closes = 0

    def open(self):
        assert self.dtr is False and self.rts is False
        assert self.port == "/dev/fake"
        self.fixture.open_attempts += 1
        if self.fixture.open_error:
            raise OSError("injected open failure")

    def close(self):
        self.closes += 1

    @property
    def in_waiting(self):
        return len(self.rx)

    def read(self, count):
        if self.read_error:
            raise OSError("injected read failure")
        data, self.rx = self.rx[:count], self.rx[count:]
        return data

    def write(self, payload):
        if self.write_error:
            raise OSError("injected write failure")
        self.writes.append(payload)
        return len(payload)


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.now = 1000.0
        self.node = module.NodeIdentity(1, 2, 3)
        self.stat_error = None
        self.usb = {"vid": 0x303A, "pid": 0x1001, "serial_number": "test-chip"}
        self.messages = []
        self.devices = []
        self.open_error = False
        self.open_attempts = 0
        self.capture = module.RecoveryCapture(
            "/dev/fake", self.messages.append, self.factory, lambda _: self.usb,
            clock=lambda: self.now, stat=self.stat, settle_seconds=25)
        self.capture.start()

    def factory(self):
        device = FakeSerial(self)
        self.devices.append(device)
        return device

    def stat(self, _port):
        if self.stat_error:
            raise self.stat_error
        return self.node

    def tick(self, seconds=0):
        self.now += seconds
        self.capture.tick()

    def disappear(self):
        self.node = None
        self.tick()
        self.tick(0.25)

    def reappear(self):
        self.node = module.NodeIdentity(1, 5, 6)
        self.tick()

    def heartbeat(self, *, main, state="BACKUP", enabled=True, session="ABC"):
        line = (f"HEARTBEAT session={session} state={state} MAIN={int(main)} SS=1 "
                f"USB={int(main)} DC=0 EN42={int(enabled)} OUT=0x00 EOUT=0x00\n")
        self.capture.receive(line.encode())

    def test_initial_open_and_bounded_read_only_probes(self):
        self.assertEqual(self.open_attempts, 1)
        self.tick(2.99)
        self.assertEqual(self.devices[0].writes, [])
        self.tick(0.02)
        self.assertEqual(self.devices[0].writes, [b"status\n", b"usb-status\n"])
        self.tick(12)
        self.tick(120)
        self.assertEqual(self.devices[0].writes,
                         [b"status\n", b"usb-status\n"] * 2)
        self.assertEqual(self.open_attempts, 1)
        self.assertFalse(any(b"transfer-start" in data for data in self.devices[0].writes))

    def test_read_errors_and_silence_never_close_or_reopen_live_handle(self):
        self.devices[0].read_error = True
        self.heartbeat(main=False)
        for _ in range(20):
            self.tick(5)
        self.capture.command("/reopen")
        self.assertFalse(self.capture.command("/quit"))
        self.assertEqual(self.open_attempts, 1)
        self.assertEqual(self.devices[0].closes, 0)
        self.assertIs(self.capture.device, self.devices[0])
        self.assertGreater(self.capture.read_errors, 0)
        self.assertTrue(any("HOST POLL present=1 handle=1" in s for s in self.messages))

    def test_confirmed_disappearance_then_one_settled_reopen(self):
        self.heartbeat(main=False)
        self.node = None
        self.tick()
        self.assertEqual(self.devices[0].closes, 0)
        self.tick(0.25)
        self.assertEqual(self.devices[0].closes, 1)
        self.assertEqual(self.capture.disappearances, 1)
        self.assertIsNone(self.capture.device)
        self.tick(30)
        self.reappear()
        self.tick(24.99)
        self.assertEqual(self.open_attempts, 1)
        self.capture.command("/reopen")
        self.assertEqual(self.open_attempts, 1)
        self.tick(0.02)
        self.assertEqual(self.open_attempts, 2)
        self.tick(3)
        self.tick(12)
        self.tick(60)
        self.capture.command("/reopen")
        self.assertEqual(self.open_attempts, 2)
        self.assertEqual(self.devices[1].closes, 0)
        self.assertEqual(self.devices[1].writes, [b"status\n", b"usb-status\n"] * 2)
        self.assertTrue(any("possible_reset_due_to_open=1" in s for s in self.messages))

    def test_transient_missing_node_does_not_consume_reopen_permission(self):
        original = self.node
        self.node = None
        self.tick()
        self.node = original
        self.tick(0.05)
        self.tick(100)
        self.assertEqual(self.capture.disappearances, 0)
        self.assertEqual(self.open_attempts, 1)
        self.assertEqual(self.devices[0].closes, 0)

    def test_stat_errors_are_not_disappearance(self):
        self.stat_error = PermissionError("injected stat permission failure")
        self.tick(1)
        self.tick(100)
        self.assertEqual(self.capture.disappearances, 0)
        self.assertEqual(self.devices[0].closes, 0)
        self.assertEqual(self.open_attempts, 1)

    def test_node_replacement_without_observed_absence_retains_handle(self):
        self.node = module.NodeIdentity(1, 200, 300)
        self.tick(1)
        self.tick(100)
        self.assertEqual(self.capture.disappearances, 0)
        self.assertEqual(self.devices[0].closes, 0)
        self.assertEqual(self.open_attempts, 1)

    def test_failed_recovery_open_does_not_retry_until_explicit_command(self):
        self.disappear()
        self.reappear()
        self.open_error = True
        self.tick(25)
        self.assertEqual(self.open_attempts, 2)
        self.tick(100)
        self.assertEqual(self.open_attempts, 2)
        self.open_error = False
        self.capture.command("/reopen")
        self.assertEqual(self.open_attempts, 3)
        self.capture.command("/reopen")
        self.assertEqual(self.open_attempts, 3)

    def test_reappearance_rejects_a_different_usb_device(self):
        self.disappear()
        self.reappear()
        self.usb = dict(self.usb, serial_number="another-chip")
        self.tick(25)
        self.assertEqual(self.open_attempts, 1)
        self.assertIsNone(self.capture.device)
        self.assertTrue(any("USB identity mismatch" in s for s in self.messages))

    def test_quit_requires_fresh_safe_status(self):
        self.heartbeat(main=True)
        self.tick(3.01)
        self.assertFalse(self.capture.command("/quit"))
        self.heartbeat(main=False)
        self.assertFalse(self.capture.command("/quit"))
        self.capture.receive(b"INTENTIONAL_BACKUP_SHUTDOWN reason=MAIN_TIMEOUT\n")
        self.assertFalse(self.capture.command("/quit"))
        self.heartbeat(main=True)
        self.assertTrue(self.capture.command("/quit"))
        self.assertEqual(self.devices[0].closes, 1)

    def test_idle_default_or_confirmed_absence_allow_quit(self):
        self.heartbeat(main=False, state="STOPPED", enabled=False)
        self.assertTrue(self.capture.can_quit())
        self.capture.command("transfer-start")
        self.assertFalse(self.capture.can_quit())
        self.disappear()
        self.assertTrue(self.capture.command("/quit"))
        self.assertEqual(self.devices[0].closes, 1)

    def test_write_failure_keeps_handle_and_commands_are_logged(self):
        self.devices[0].write_error = True
        self.assertFalse(self.capture.send("usb-status"))
        self.assertEqual(self.devices[0].closes, 0)
        self.assertEqual(self.open_attempts, 1)
        self.assertFalse(self.capture.send("bad\x00command"))
        self.assertTrue(any("HOST SEND origin=operator" in s for s in self.messages))

    def test_partial_lines_and_firmware_sessions_are_preserved(self):
        self.capture.receive(b"HEARTBEAT session=OLD state=BACKUP ")
        self.capture.receive(b"MAIN=0 SS=1 USB=0 DC=0 EN42=1 OUT=0x00 EOUT=0x00\n")
        self.heartbeat(main=True, state="STOPPED", enabled=False, session="NEW")
        self.assertEqual(self.capture.firmware_session, "NEW")
        self.assertTrue(any("HOST FIRMWARE_SESSION old=OLD new=NEW" in s for s in self.messages))
        self.assertGreater(self.capture.rx_bytes, 0)


if __name__ == "__main__":
    unittest.main()
