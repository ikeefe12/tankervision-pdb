# USB serial recovery after intentional power-off

This diagnostic build keeps the real-bank charger, D7 heartbeat and 20-second
main-loss shutdown policy from [the transfer test](TRANSFER-TEST.md). It records
USB state without automatically resetting or repairing the USB controller. The
test distinguishes application boot, USB bus activity and host-visible serial
recovery. It does not assume the previous failure's cause is known.

## Setup

Keep J1 main power and J2 data connected, with the 21.6 V bank and real temperature
resistor connected and exterior outputs unloaded. Build and upload using the
usual README esptool command:

```sh
sh firmware/board_control_test/build-usb-recovery.sh
python firmware/board_control_test/capture_usb_recovery.py \
  --port /dev/cu.usbmodem2101 \
  --log firmware/board_control_test/results/usb-recovery-2026-09-14/live.log
```

Keep this single capture running. Check `status` and `usb-status` with controlled
outputs off. Send `transfer-start` and require verified 20 V/2 A PD, real sensor
qualification, backup armed with PG, and `READY_FOR_METER_CHECK` before proceeding.
The existing meter-confirmed charging endpoint is 20.67 V. VCAP telemetry is still
an uncalibrated ADC estimate; no current ADC channels are sampled.

## Physical sequence

1. Remove **J1 main power only**, leaving **J2 data connected**.
2. D7 should keep blinking for approximately 20 seconds, then stop when GPIO42
   releases backup and the board powers down. Wait another **5 seconds**.
3. Restore **J1**, keeping J2 untouched. Note whether D7 resumes blinking.
4. Leave both cables connected for **60 seconds**. Report D7's behavior and any
   unexpected restarts. Do not press reset or open another serial monitor.
5. Keep J2 connected until the captured evidence has been checked. If application
   serial is silent, a later controlled J2 replug with J1 still connected retrieves
   the saved diagnostics. That is a separate intervention, not part of the first
   recovery observation.

The host records port disappearance and return. After a recorded disappearance
and reappearance it waits 25 seconds before one reopen, allowing the rebooted
application to save its first snapshot. Opening this native serial port can reset
the ESP32; every open is therefore an explicit host-log event, and session/reset
identifiers distinguish an original boot from a host-induced restart. A silent
stream does not trigger repeated reopen attempts. No reopen occurs merely
because serial reports a read error while its device node is still present.

## Recorded evidence and limits

`UsbRecoveryDiagnostics` provides reusable `begin`, `service`,
`observeConnection`, `snapshot`, `exportBlob` and reporting functions. It records
task-delivered HWCDC bus-reset/RX/TX counts, byte counts, recent reset/RX/connection
events, SOF presence, receive-queue availability, and raw/enabled/status USB
interrupt registers. It observes the existing `bool(Serial)` evaluations:
Arduino core 3.3.3's connection check actively enables a TX interrupt and flushes
the hardware TX FIFO when disconnected, so extra connection probes are avoided.
The command reader also rejects negative availability/read results, which the
core can return when its receive queue is unavailable.

TX callbacks mean bytes were moved into the device TX FIFO, not that macOS received
them. Host byte receipts are separate evidence. The older `dropped_bytes` field
counts only some application write shortfalls; it does not count every gated or
buffered message and is not a delivery guarantee. The declared HWCDC connected
event is not relied upon: this installed core does not post it.

A checksummed RTC journal can retain the previous boot across some warm resets;
records are rejected after power-on, brownout or power-glitch reset. RTC retention
through USB resets is a test observation, not assumed. After boot, snapshots at
10 and 45 seconds also use a separate NVS key, only if main power is freshly
validated, the transfer is inactive and all controlled outputs including GPIO42
are off. There are at most two snapshot attempts per boot. The preceding NVS blob
is loaded at boot for later reporting, separately from RTC evidence. The first
snapshot also archives the preceding saved boot under a second NVS key, and that
older record is loaded and reported separately. Session and
uptime identify which boot each record describes; it may predate this test.

`usb-status` reports current and available prior evidence only with main power and
controlled outputs off. The report services the supervisor and heartbeat between
rows. Event callbacks perform no serial or flash operations. Recording begins in
application setup, so it cannot capture ROM events or earlier enumeration.

Successful recovery requires a fresh application session and responses to host
commands after J1 returns with J2 unchanged. D7 alone establishes application-loop
liveness, not serial recovery. A second session following the host reopen is
reported as a separate boot. No sleep or automatic USB recovery is added to this
build. Espressif describes the hardware interface in its
[USB Serial/JTAG console guide](https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32s3/api-guides/usb-serial-jtag-console.html);
the exact event/probe behavior above was checked against the installed Arduino
ESP32 core 3.3.3 `HWCDC.h` and `HWCDC.cpp`.
