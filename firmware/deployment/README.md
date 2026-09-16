# TankerVision PDB deployment firmware

Standalone Arduino ESP32-S3 firmware for `ESP32-S3-WROOM-1-N8R8`, with a USB
protocol for the Jetson. This folder is independent of the historical
`board_control_test` sketch. See [API.md](API.md) for the complete wire protocol,
[jetson/README.md](jetson/README.md) for the reference client and shutdown hook,
and [USB-TRANSPORT.md](jetson/USB-TRANSPORT.md) for USB configuration and upload recovery.

## Operating sequence

1. **Settle for 10 seconds from application uptime zero.** D7 is solid green.
   Initialize the ESP32's monitoring interfaces and USB, hold direct backup GPIO42
   at its default low level, and read the board. Send telemetry every second.
   Leave both expanders' registers untouched until settling finishes. Reads send
   I2C register pointers but do not program their configuration or output data.
2. **Prepare charging.** Read and reconcile retained expander outputs, lowering
   CE before boost and other enables. Select real temperature sensing and the low
   nominal **1.03 A** charge profile. Validate a **20 V / 3 A** USB-PD request,
   then enable boost, wait for PG and qualify the real sensor before enabling CE.
   This preparation also runs with an already charged bank.
3. **Qualify the bank.** Proceed when VCAP is strictly **greater than 20 V**, or
   report a charge issue and proceed after **10 seconds without a meaningful
   rise**. A meaningful rise is at least **0.10 V above the last progress mark**;
   small ADC noise cannot continually restart the timer. A healthy >20 V result
   leaves boost and charger enabled to maintain the bank while the Jetson runs.
   A no-rise result, invalid sensing, temperature/PG/control fault, 21 V ADC trip,
   or two-hour initial-charge limit requests charging shutdown and continues
   startup with the issue retained in telemetry. There is no automatic retry of
   a failed charger in that boot. Maintenance has no charge-progress/time limit;
   temperature, supply, control readback and voltage protection remain active.
4. **Arm backup and start the Jetson.** When valid VCAP is at least 10 V, enable
   GPIO42 and require backup PG. A charging or backup-arm failure does not skip
   the Jetson startup attempt. Enable **J9 / V_BUS_SS / `vbus_ss`**, then ping
   the Jetson every two seconds. A matching reply confirms communication;
   `JET_ON_FB` is recorded independently. Power monitoring does not wait for the
   operating system or USB service to finish booting.
5. **Run.** D7 is a 1 Hz heartbeat (500 ms on/off) after settling. Continue 1 Hz
   telemetry, charger maintenance/health monitoring, pings, and API service.
   Charger faults in this state disable charging and retain Jetson/backup power.
6. **Latch shutdown on main loss or an API reboot request.** Immediately notify
   the Jetson, disable charger CE, then boost after its decay interval. Keep
   backed ports and GPIO42 available during the shutdown allowance. Record the
   Jetson's acknowledgment and final ready message. Neither message changes the
   deadline: it is **60 seconds from the ESP32's original shutdown request**.
   Actual main loss also latches the unbacked J7/J8 outputs off after charger
   cleanup, including J8's converter, so they stay off if main returns. An API
   reboot with uninterrupted main leaves those outputs on until final cutoff.
   Main-power return is reported but ignored for this policy. No API command can
   cancel shutdown or extend the timer, and port changes are rejected during it.
7. **Cut power and restart if still powered.** At the deadline, begin restoring
   both expanders to their power-on defaults, then deassert GPIO42. This final
   cleanup has a 750 ms allowance; an I2C fault cannot hold backup indefinitely.
   If main remains absent, the board loses its supply. If it remains alive for
   250 ms after GPIO42 release, call `esp_restart()` and repeat the 10-second
   startup sequence. This also implements a requested full-system reboot.

The main-loss detector also operates after settling during charger/Jetson startup.
A low `PMUX_ST` directly identifies backup selection; high requires input PG to
establish main power because the mux can also report high with its output Hi-Z.
Invalid expander data is reported instead of being silently treated as fresh.

The heartbeat follows application-loop execution; it is not autonomous PWM that
could keep blinking while the power manager is stuck. USB availability is never
required for startup or timeout processing. USB messages are generated every
second throughout settling, charging, operation and the shutdown allowance;
physical delivery requires a connected host that reads them. No firmware can
deliver USB telemetry while its host is unpowered or its port is closed.

## Board and power assumptions

The schematic/PCB hashes still match the earlier audited version. A fresh KiCad
netlist on **2026-09-16** rechecked all 23 expander connections, all nine ADC
signals, the 82 kΩ/10 kΩ VCAP divider, and seven 820 Ω IMON resistors. J9 is the
backed VBUS Jetson port. J8 is the unbacked 5 V port and its API operation enables
the upstream converter before the port, then disables the port before that buck.

Use the intended **21.6 V rated bank** and real temperature resistor. The charger
hardware target remains approximately **20.629 V**. The requested >20 V / progress
rule is startup qualification; it does not establish full stored capacity. VCAP
is the ADC millivolts scaled by **9.2**, without applying a single-point meter
correction. All seven current channels are now sampled for the requested complete
telemetry; their nominal estimates use **148.42 mV/A**, and remain uncalibrated.

USB input J1 requires a source advertising at least **20 V / 3 A**. Preparation
requests and verifies that allowance; the source advertisement alone is not
accepted. The previous bench procedure used 2 A with unloaded outputs; this build
requests 3 A because maintenance charging and the Jetson can operate together.
The charger can consume roughly 23 W including losses at its low profile. Size
the Jetson power mode and all external loads within the remaining source and
board limits. Firmware does not claim calibrated current-based load protection.

For DC-only input, JP1 must be open and USB-PD power absent, as required by the
board design. The configured DC budget is **3 A** at the permitted 18.5–20 V input;
this is an installation assumption because a DC adapter cannot negotiate or
report its rating. Change that constant to match the actual supply.

Charger failures continue to the J9 startup attempt, but a failed input-budget
verification or failed hardware reconciliation cannot authorize an unchecked
large load: J9 remains off and reports `JETSON_INPUT_BUDGET_UNVERIFIED` or the
hardware error. A degraded bank can also leave insufficient energy for the full
60 seconds under real Jetson load. The deadline is fixed; this version reports
low bank voltage but does not replace the user's deadline with an earlier 7 V
cutoff. Loaded endurance and rail droop require bench measurements.

TCA9535 power-on defaults are input directions `FFFF`, output latches `FFFF`, and
polarity `0000`. The external pulldowns keep controlled enables low when their
pins become inputs. `startRestorePor()` first lowers and verifies driven enables,
then verifies input directions before writing the high output-latch defaults.
GPIO42 is released last. There are no further GPIO/ADC/I2C/PD operations after
release; only bounded USB draining and the possible software restart remain.

An unexpected ESP32-only reset releases GPIO42 but does not reset the expanders.
The requested 10-second settling period leaves any retained expander outputs
untouched until reconciliation. Intended full-system reboot explicitly defaults
the expanders before restarting the ESP32; it is the supported controlled path.

## Build and upload

Validated toolchain: Arduino CLI 0.35.3 with Arduino ESP32 core **3.3.3**. No
third-party Arduino libraries are required.

```sh
sh firmware/deployment/build.sh
```

The build selects USB-OTG/TinyUSB and disables CDC-on-boot. The application creates
its own CDC instance and calls `enableReboot(false)` **before USB starts**. This
disables the Arduino DTR/RTS and 1200-baud reboot hooks that are inappropriate for
the power manager. It avoids the hardware Serial/JTAG mode used by the bench
firmware, where a host reopen was observed to reset the MCU. It does not modify
eFuses. Ordinary CDC open/close and reset behavior must still be confirmed on the
actual Jetson during integration.

Upload with main power present. To enter the ROM downloader from deployment
firmware, hold SW1/BOOT, press and release SW2/RESET, then release BOOT. Find the
ROM serial port and write the generated images, for example:

```sh
python /path/to/esptool.py --chip esp32s3 --port /dev/cu.usbmodem2101 \
  --before no_reset --baud 460800 write_flash --flash_size 8MB \
  0x0000 firmware/deployment/build/deployment.ino.bootloader.bin \
  0x8000 firmware/deployment/build/deployment.ino.partitions.bin \
  0xe000 /path/to/esp32/core/tools/partitions/boot_app0.bin \
  0x10000 firmware/deployment/build/deployment.ino.bin
```

When replacing the older hardware-CDC bench firmware, its existing `--before
usb_reset` upload path is also available. After deployment starts, use the manual
ROM sequence above for subsequent flashing. The USB port identity/path can change
between ROM and application modes. Never initiate flashing/reset while the board
is relying on backup power.

This firmware **starts charging and enables J9 automatically after settling**.
It does not wait for a USB command. The reference Jetson client expects one owner
of J2; its cleanup and OS power-off commands are explicit opt-ins.

## Code organization

| File | Responsibility |
|---|---|
| `PowerManager.h/.cpp` | Pure, allocation-free policy and hardcoded timing/voltage constants; no hardware waits. |
| `BoardHardware.h/.cpp` | Reusable asynchronous converter, charger, backup, port and expander-default operations; ADC/GPIO/register snapshots. |
| `PowerDelivery.h/.cpp` | Read-only CYPD3177 monitoring and staged, verified volatile 20 V/3 A sink request. |
| `Application.cpp` | Integrates policy, hardware operations, LED, complete telemetry, Jetson handshake and final reset. |
| `Protocol.h/.cpp` | Bounded request framing, strict JSON request schema and recent mutation replay cache. |
| `JsonOutput.h` | Bounded JSON construction and USB queue with partial-write continuation and reply capacity. |
| `jetson/pdb_client.py` | Reusable host API, monitor/control CLI, ping replies, acknowledgment and optional cleanup/shutdown workflow. |

All I2C and policy operations run from the main loop. GPIO interrupt handlers only
count edges. I2C transactions have 25 ms timeouts; hardware ramp/settling and PD
stability waits are state transitions rather than `delay()` loops. USB writes
use a bounded 2 ms timeout and continue partial frames without interleaving.
There is no indefinite USB wait, blocking command read, or shutdown-ACK wait.
Per-message sequence, boot ID, uptime, sample age, validity and queue/drop metrics
make gaps and restarts visible. Internal hardware operations and polling can add
bounded scheduling jitter; this is not a hard real-time 1000.000 ms wire clock.

## Validation

Run the host suites without accessing the board:

```sh
sh firmware/deployment/tests/run-power-manager.sh
sh firmware/deployment/tests/run-hardware.sh
sh firmware/deployment/tests/run-protocol.sh
sh firmware/deployment/tests/run-pd.sh
sh firmware/deployment/tests/run-application.sh
python3 -m unittest discover -s firmware/deployment/jetson -p 'test_*.py' -v
```

These cover policy timing and rollover, charger maintenance/failure, real hardware
driver sequencing against register models, strict framing/parser behavior,
negotiated-budget checks, application integration, and the Jetson client. Build
logs and the [final test summary](results/README.md) are under `results/`. These are development
verification results; they do not substitute for a loaded Jetson power test.

For hardware acceptance, first verify 1 Hz telemetry and solid-then-blinking D7,
temperature qualification, charger maintenance, all PG/enables and a matching
Jetson pong. Open/close the application serial port and require the **same boot ID
with increasing uptime**. Then exercise the three user-controlled ports, including
J8 converter sequencing. With adequate charged-bank energy, test main loss and
both Jetson acknowledgment messages, including main return during the grace
period: the shutdown must still complete. Finally issue `reboot` with main present
and verify all ports are off, both expanders reach defaults, the ESP32 gets a new
boot ID, and startup repeats after settling. Record any missing ACK, lost data,
unexpected reset, rail droop or inadequate backup duration as a test failure or
limitation rather than inferring success from D7 alone.
