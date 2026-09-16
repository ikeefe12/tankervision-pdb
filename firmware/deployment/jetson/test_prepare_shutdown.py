import subprocess
import unittest
from types import SimpleNamespace

from prepare_shutdown import prepare_services


class PrepareShutdownTests(unittest.TestCase):
    def test_stops_explicit_services_before_sync_without_poweroff(self):
        calls = []

        def run(command, **options):
            calls.append((command, options))
            return SimpleNamespace(returncode=0)

        self.assertTrue(prepare_services(["tankervision.service", "recorder@0.service"], runner=run, clock=lambda: 100))
        self.assertEqual([call[0] for call in calls],
                         [["systemctl", "stop", "--", "tankervision.service", "recorder@0.service"], ["sync"]])
        self.assertTrue(all(call[1]["timeout"] == 40 for call in calls))

    def test_failed_stop_or_timeout_does_not_claim_cleanup_success(self):
        for outcome in (SimpleNamespace(returncode=1), OSError("missing systemctl"),
                        subprocess.TimeoutExpired("systemctl", 40)):
            calls = []

            def run(command, **_options):
                calls.append(command)
                if isinstance(outcome, Exception):
                    raise outcome
                return outcome

            self.assertFalse(prepare_services(["app.service"], runner=run))
            self.assertEqual(len(calls), 1)

    def test_empty_injected_or_nonservice_units_never_run_commands(self):
        for services in ([], ["--all"], ["app.service;poweroff"], ["app.target"], ["/tmp/app.service"]):
            with self.assertRaises(ValueError):
                prepare_services(services, runner=lambda *_a, **_k: self.fail("Unexpected command"))

    def test_one_deadline_covers_stop_and_sync(self):
        now, timeouts = [100], []

        def run(_command, **options):
            timeouts.append(options["timeout"])
            now[0] += 15
            return SimpleNamespace(returncode=0)

        self.assertTrue(prepare_services(["app.service"], timeout=40, runner=run, clock=lambda: now[0]))
        self.assertEqual(timeouts, [40, 25])


if __name__ == "__main__":
    unittest.main()
