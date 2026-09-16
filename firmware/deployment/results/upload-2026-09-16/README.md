# First deployment upload and USB telemetry — 2026-09-16

The user authorized uploading the deployment build and confirming USB telemetry.
Before uploading, the existing bench capture reported main USB power present,
GPIO42 low, all controlled outputs low, and raw VCAP about 2.86 V. The existing
capture accepted `/quit` with its verified safe-close condition; see
[pre-upload.log](pre-upload.log).

All 45 entries in the development SHA-256 manifest matched before flashing.
Esptool 4.8.6 uploaded the bootloader, partition table, `boot_app0.bin`, and
application to ESP32-S3 MAC **68:ee:8f:58:b0:10** with hash verification after every
write. See [upload.log](upload.log). The application image is 438,672 bytes with
SHA-256 `a96676a473132a92399874deadfcba5cea796a18174841d5495c6d48aeedd09f`.

The new software CDC endpoint enumerated as **TankerVision PDB**, VID:PID
`303A:1001`, serial `68:EE:8F:58:B0:10`, at `/dev/cu.usbmodem2101` on this Mac.
The reference client ran in default `monitor` mode: status and pong only, without
port/reboot commands, shutdown acknowledgments, or any host poweroff action.
Capture began at device uptime 30.695 s, so it does not directly establish USB
delivery or LED behavior during the initial settling period.

Captured messages are in [telemetry.jsonl](telemetry.jsonl), with host connection
diagnostics in [monitor.stderr.log](monitor.stderr.log). A Mac client pong proves
the USB protocol round trip; it does not prove that a Jetson service is connected.

## Results

**Passed:** 240 valid JSON messages, including 158 telemetry snapshots, across
the initial capture and one deliberate close/reopen. Both captures used boot ID
`0E44CE37`. The 156 periodic snapshots were spaced exactly 1,000 ms apart within
each connected interval; the two additional snapshots answered `status` requests.
No new drops or late telemetry periods occurred while either capture was open.
The maximum reported application-loop gap was 26 ms. See [analysis.json](analysis.json)
and the [final telemetry snapshot](final-telemetry.json).

| Observation | Result |
|---|---|
| Charging | Captured raw VCAP rose from 6.7988 V to **20.47 V**; firmware entered `RUNNING` and retained maintenance charging. |
| Real temperature path | Override and high-current selections stayed low; final TS was **1.816 V**. |
| USB-PD | Verified **20 V / 3 A** operating request; source advertised 5 A at 20 V. |
| Charger controls/PG | Boost and CE asserted, boost and charger PG asserted. |
| Backup | GPIO42 armed and backup PG asserted. |
| Jetson supply | J9 enable and PG asserted; the other three external outputs remained disabled. |
| Complete telemetry | All nine ADC channels, 30 named digital signals, both valid/fresh expander snapshots and valid/fresh PD data received. |
| Faults | No issue bits or adapter errors in any captured snapshot. |
| USB round trip | Status and matching pong requests returned `OK`; Jetson feedback itself remained low. |
| Reopen | Last initial-capture uptime **140.045 s**; first reopened snapshot **151.516 s**, with the same boot ID. Uptime continued through **197.045 s** without a restart. |

The reopened capture is [reopen-telemetry.jsonl](reopen-telemetry.jsonl), with
[connection diagnostics](reopen.stderr.log). Reported reset reason 11 belongs to
the upload's initial USB reset; it did not change or recur at serial reopen.
Both host monitors were closed after verification, freeing J2 for the user's
client. Firmware remains running with charger maintenance, backup armed and J9
enabled. `jetson.responsive` will expire when this Mac stops answering pings until
another host client responds; that is not a power-state change.

## Scope

No main-power removal, load test, API reboot, OS shutdown, temperature override,
or optional-port command was performed. Current estimates remain uncalibrated.
The initial offline drop count records telemetry generated before the host opened
USB; it is separate from losses while the capture is connected. At 1 Hz, D7 samples
can coincide with the same phase of its 1 Hz heartbeat, so a constant sampled LED
level does not establish whether the physical LED is blinking.
