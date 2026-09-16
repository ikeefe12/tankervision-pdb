#!/usr/bin/env python3
"""Optional Jetson cleanup hook: stop explicitly named services, then sync.

This program does not power off. It is not invoked by the client unless supplied
as --cleanup-command. Customize it when application cleanup needs more than a
normal systemd service stop.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import time


def prepare_services(services: list[str], timeout: float = 40, *,
                     runner=subprocess.run, clock=time.monotonic) -> bool:
    if not services or any(not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.@:-]*\.service", name)
                           for name in services):
        raise ValueError("Explicit valid systemd .service unit names are required")
    if not 0 < timeout <= 45:
        raise ValueError("Cleanup timeout must be greater than 0 and at most 45 seconds")
    deadline = clock() + timeout
    # subprocess receives argv directly. No shell, sudo, reboot, or poweroff.
    for command in (["systemctl", "stop", "--", *services], ["sync"]):
        remaining = deadline - clock()
        if remaining <= 0:
            return False
        try:
            if runner(command, check=False, timeout=remaining).returncode != 0:
                return False
        except (OSError, subprocess.TimeoutExpired):
            return False
    return clock() < deadline


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--service", required=True, action="append", help="Application .service unit; repeat for multiple units")
    parser.add_argument("--timeout", type=float, default=40, help="Combined stop+sync budget, maximum 45 seconds")
    args = parser.parse_args(argv)
    try:
        return 0 if prepare_services(args.service, args.timeout) else 1
    except ValueError as error:
        parser.error(str(error))
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
