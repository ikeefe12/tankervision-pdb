# Jetson integration example

[`pdb_client.py`](pdb_client.py) implements the [deployment USB protocol](../API.md). It can be imported into the Jetson application or run as a single-owner serial client. Its default action is to print telemetry and answer pings. It never executes host poweroff unless all explicit shutdown options are supplied.

The ESP32 keeps a healthy charger enabled after reaching the initial charge target, maintaining the supercap while main power is present. Charger faults and the fixed shutdown sequence stop charging. The default USB input budget is 20 V/3 A; the installed Jetson, charging demand, optional outputs, and losses must fit the combined budget. The client does not override temperature protection or change charger settings.

Use deployment firmware with TinyUSB CDC reboot hooks disabled before USB startup. The client asserts stable DTR and RTS for that transport; read [USB-TRANSPORT.md](USB-TRANSPORT.md) before using it with the board. Older HWCDC board-test firmware has different reset behavior.

## Install and monitor

Install pyserial in the Python environment that will run the Jetson application:

```sh
python3 -m pip install -r firmware/deployment/jetson/requirements.txt
python3 firmware/deployment/jetson/pdb_client.py \
  --device /dev/serial/by-id/REPLACE_WITH_PDB_DEVICE \
  --log pdb-telemetry.jsonl monitor
```

Use the actual persistent device path from the Jetson. Linux opens are exclusive; stop another serial terminal before starting the client. Stdout and `--log` contain received JSON, while client diagnostics and configured child-command output go to stderr. No module import or test opens a serial device.

Read a snapshot or control an optional output with a standalone client:

```sh
python3 firmware/deployment/jetson/pdb_client.py --device /dev/serial/by-id/REPLACE_WITH_PDB_DEVICE status
python3 firmware/deployment/jetson/pdb_client.py --device /dev/serial/by-id/REPLACE_WITH_PDB_DEVICE port 5v_vbus on
python3 firmware/deployment/jetson/pdb_client.py --device /dev/serial/by-id/REPLACE_WITH_PDB_DEVICE port 5v_vbus off
```

The `port` command waits for the response and asynchronous `port_result`. It exits nonzero on rejection, failed hardware action, or a ten-second result timeout. It does not retry a mutation. `vbus_ss` is the reserved Jetson supply and is deliberately absent from the optional-port commands.

The `reboot` command requests the ESP32's complete fixed 60-second shutdown/restart sequence. It exits after acceptance; it does not claim the Jetson has shut down. A persistent application should issue controls through its existing `PdbClient` instance so it can keep receiving shutdown requests instead of opening a second serial owner.

## Enable a real Jetson shutdown handler

Only run the following configuration on the Jetson that should shut down with this PDB. Provide a cleanup program that stops the application's work, flushes its data, and exits zero on success. It must not itself power off the host; that happens after final readiness is sent.

```sh
python3 firmware/deployment/jetson/pdb_client.py \
  --device /dev/serial/by-id/REPLACE_WITH_PDB_DEVICE \
  --cleanup-command '/usr/local/sbin/tankervision-prepare-shutdown' \
  --cleanup-timeout 45 \
  --poweroff-command '/usr/bin/systemctl poweroff' \
  --allow-host-poweroff monitor
```

The commands are parsed into argument lists and executed without a shell, implicit `sudo`, or privilege escalation. Configure their required permissions on the Jetson. The host cleanup budget is at most 45 seconds and is shortened when reconnecting late in the board's fixed 60-second window.

If the application is already managed by systemd, [`prepare_shutdown.py`](prepare_shutdown.py) is an optional reference cleanup hook. Supply every application service explicitly, for example:

```text
--cleanup-command 'python3 /opt/tankervision/pdb/prepare_shutdown.py --service tankervision.service'
```

The reference hook waits for the named service stop to finish, then runs `sync`, within one 40-second default budget. It does not power off. Missing service arguments, command errors, or a timeout fail cleanup. Adapt the hook if the application needs additional coordination, and keep the USB client in a separate service so stopping the application does not stop its shutdown handshake.

The client immediately ACKs a newly observed shutdown, runs cleanup in a worker so USB reception and pongs continue, then sends final-ready on success. It waits at most one second for the ready response before starting the explicitly configured poweroff command. Cleanup failure, timeout, rejection, a changed boot ID, or a missing ready response prevents this example from starting its poweroff action. The board's cutoff still occurs on schedule.

For protocol testing without shutting down the host, use `--ack-shutdown monitor`. That mode sends ACK once per shutdown but never final-ready or an operating-system poweroff command. Do not treat this mode as a working production shutdown handler.

The example ends on a transport error or device disappearance and does not automatically reopen or reset the ESP32. A service may restart it against the same persistent device identity. Each new client obtains the current boot ID and checks telemetry for a shutdown that began before connection. A retained shutdown is deduplicated within one client process; make the cleanup program idempotent across process restarts.

## Import into an application

The reusable pieces have no implicit serial or operating-system actions:

- `JsonLines.feed(bytes)` provides bounded parsing and recovery after oversized or malformed input.
- `PdbClient(send_bytes, ...)` handles protocol objects and uses a caller-supplied function that writes all bytes or raises.
- `client.receive(message)` processes a complete JSON object, automatically answers pings, and delivers it to `on_message`.
- `client.poll()` handles asynchronous cleanup results and deadlines; call it regularly from the same thread as `receive()` and `request()`.
- `client.request("port_set", port="5v_vbus", enabled=True)` and `client.request("reboot")` return IDs for observing responses. Read at least one valid device message before a mutation so the current boot ID is known.
- `ShutdownHooks(prepare, poweroff, timeout=45)` supplies explicit application callbacks. `prepare(request, timeout)` runs in a daemon worker and returns exactly `True` on success; it must honor its timeout and arrange cancellation of its own subordinate work. `poweroff()` runs only after the matching final-ready response.

The bundled CLI uses `subprocess.run(..., timeout=...)` for cleanup. A timed-out process is killed and waited for by Python; a cleanup script is responsible for any separately detached subprocesses it starts. No client can turn a failed or timed-out cleanup into extra time from the power supervisor.

## Host tests

```sh
python3 -m unittest discover -s firmware/deployment/jetson -p 'test_*.py' -v
```

Tests use in-memory messages, a fake clock, and mocked serial construction. They do not open USB, invoke shutdown commands, or require pyserial. They cover framing recovery, mutation guards, asynchronous port results, continued ping service during cleanup, duplicate shutdown observations, timeout/failure behavior, boot changes, and poweroff opt-in. Hardware transport and loaded Jetson shutdown still require the acceptance checks in [USB-TRANSPORT.md](USB-TRANSPORT.md).
