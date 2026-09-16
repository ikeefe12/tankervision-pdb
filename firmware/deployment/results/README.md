# Deployment development verification — 2026-09-16

This records the compiled and host-tested development build. The subsequent
[2026-09-16 upload and telemetry check](upload-2026-09-16/README.md) records its
first physical ESP32 deployment. Loaded Jetson shutdown still requires integration
testing. Historical bench results apply to the separate `board_control_test`
firmware.

## Build

`sh firmware/deployment/build.sh` passed using Arduino CLI 0.35.3 and ESP32 core
3.3.3. See [build.log](build.log). The final incremental build reported no compiler
warnings, 438,531 bytes of flash and 105,624 bytes of global RAM (222,056 bytes
remaining in the reported static allocation budget). These figures are not a
measurement of runtime stack/heap margins.

All production `.cpp` and `.h` files match the compiler's sketch copies. The
SHA-256 manifest records production source, host client/test files and generated
application/bootloader/partition images for this verification snapshot. Generated
images are in the ignored `../build/` directory and can be reproduced with the
documented toolchain.

## Host checks

| Check | Result |
|---|---|
| `tests/run-power-manager.sh` | 28 policy scenarios passed: startup order, charge progress/failure/maintenance, backup arming, main loss, reboot, immutable deadlines, stale completions and timer rollover. |
| `tests/run-hardware.sh` | Passed actual hardware-driver tests against an I2C register model: read-only settling, real-temperature qualification, CE-before-boost shutdown, PG timeouts, preemption, J8 converter sequencing, rollback, expander defaults and complete sampling. |
| `tests/run-protocol.sh` | 12 scenarios passed, including 20,000 randomized input streams under undefined-behavior sanitization. |
| `tests/run-pd.sh` | 35 scenarios passed under undefined-behavior sanitization: request contents, verified contract response, freshness, insufficient budget, I2C failures and timeout/rollover. |
| `tests/run-application.sh` | Three actual application/policy/parser/USB queue integration scenarios passed with simulated board/PD/USB: 356 JSON frames and 172 telemetry snapshots independently validated; see [application-tests.log](application-tests.log). |
| Python `unittest` discovery in `jetson/` | 28 tests passed: 24 client tests and four cleanup-hook tests. No serial device was opened and no host shutdown action was executed. |

Application tests cover continuous settling telemetry, matching pong, asynchronous
port controls, main loss with immediate return, staged unbacked-port shedding,
ACK/ready without deadline changes, full reboot with main present, defaults before
backup release, no hardware work after release, USB reconnect framing and runtime
charger fault handling. JSON output is parsed independently and checked for all
nine ADC channels, 30 named digital signals and expander/PD telemetry.

## Schematic evidence and remaining hardware acceptance

The schematic and PCB SHA-256 values match the prior audited
[`schematic-map.json`](../../board_control_test/schematic-map.json):

```text
schematic fe40a02cf6cebcd3f4909c79bb2411796b1a13390b0c5edc303da37153d90239
pcb       88b42dc8079ee75d50be56a2138377d0cd0d8bb0ad311b51be9ec2db7a48d808
```

A fresh KiCad XML export on 2026-09-16 rechecked the expander signal map, ADC
connections and divider/current-sense resistor values. GPIO0 requires its internal
pull-up during operation because the schematic has no external pull-up. This
review did not rerun ERC or DRC.

The [first upload report](upload-2026-09-16/README.md) now establishes the 3 A
USB-PD request, charger maintenance, automatic J9/backup controls with PG, complete
1 Hz telemetry and same-boot serial reopen on the Mac. Hardware acceptance still
includes the remaining procedure in the [firmware README](../README.md): optional
port commands, USB behavior on the Jetson, actual cleanup and Linux poweroff,
backup endurance under the installed load, and main-return/full-reboot behavior.
ADC current estimates are nominal and require calibration before being used as
quantitative load evidence.
