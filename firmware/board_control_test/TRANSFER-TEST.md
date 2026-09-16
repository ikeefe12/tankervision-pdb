# Real-supercap charge and source-transfer test

This build charges the user-authorized **21.6 V bank** toward the board's fixed
**20.629 V nominal target**, then supervises operation after main power removal.
Each main-power loss starts a **20-second grace period**. Main returning within
that period cancels shutdown. Otherwise the ESP32 deasserts GPIO42/VCAP_EN,
disconnecting its only supply and leaving a partly charged bank. D7 now blinks
at 1 Hz (500 ms on, 500 ms off) while the main loop runs, independent of voltage.
The procedure below defines the test; completed hardware results belong in a
separate capture/report. External output ports remain off throughout.

Use J1 for a 20 V USB-PD source capable of at least 2 A, with JP1 closed and no
DC input at J6. Keep J2 connected to the computer for data; **J2 does not power the
board**. Connect the bank at CN1 pin 1 (+), pin 2 (GND), and its real thermistor at
J4. Leave J7–J10 unloaded. Replace any automatic meter-hold firmware before
connecting the bank: this transfer build starts with charging and backup disabled.

Build from the repository root, then use the [README upload command](README.md#build-and-upload)
with the resulting files in `firmware/board_control_test/build/`:

```sh
sh firmware/board_control_test/build-transfer.sh
```

Keep main power connected during upload and serial setup. Start **one persistent
capture**; it sends the initial `status` command automatically:

```sh
python firmware/board_control_test/capture_transfer.py \
  --port /dev/cu.usbmodem2101 \
  --log firmware/board_control_test/results/transfer.log
```

Use the actual serial device. Opening or closing native USB serial can reset this
ESP32 and release GPIO42, cutting its backup supply. Keep this process and J2
connected through charging, transfer, and return. Do not use the timed `capture.py`
tool or open a second serial monitor for this test. The transfer logger has no
timed exit or automatic reconnect; a USB loss is recorded explicitly.

1. Inspect `status`: require `init=1`, `MAIN=1 USB=1 DC=0 SS=1`,
   `OUT=0x00 EOUT=0x00`, and `EN42=0`. Compare the displayed VCAP against a meter
   at CN1. Initialization failure leaves the test disabled.
2. Type `transfer-start` in the same capture process. Charging stays off while the
   application requests/validates a fixed **20 V, at least 2000 mA** operating
   allowance. Require `PD_REQUEST PASS` with a matching, nonzero active PDO/RDO
   current request stable for 500 ms; the adapter's advertised current alone is
   insufficient. A failed request prevents charging. An already sufficient
   contract is verified without rewriting it. The PD bus is closed afterward.
3. Require `START PASS state=CHARGING`. Charging uses the real NTC
   (`override=0`) and low current profile (`ISET_HIGH=0`, nominal 1.03 A limit).
   Watch VCAP rise and the PG/status messages. No current-monitor ADC channels
   are read; charger/source current is not measured by this test.
4. Wait for `READY_FOR_METER_CHECK`. Confirm the actual CN1 voltage is near
   **20.63 V** with the meter before removing J1. Readiness is an ADC plateau
   heuristic, not charge-current taper or full-capacity proof. Charging remains
   enabled while ready and main power remains present.
5. Remove **J1 only**, leaving J2 and the capture process connected. Look for
   `MUX_EDGE`, `BACKUP_CONFIRMED`, then uninterrupted heartbeats with
   `MAIN=0 USB=0 DC=0 CAP=1 SS=1 EN42=1`. Charger CE goes low first; boost follows
   after at least 100 ms. GPIO42 remains armed during the 20-second grace period.
   Serial reports `loss_ms` and `shutdown_in_ms`; D7 keeps blinking on backup.
6. Reconnect J1 **before 20 seconds** to check cancellation. Require
   `MAIN_RESTORED shutdown_cancelled=1`, valid main PG, and
   charging still off. Send `transfer-stop` to release the backup arm while main
   is valid. After a fresh main-power heartbeat, type `/quit` to close the logger.
   Its normal quit path refuses to close without that fresh confirmation.

For the cutoff check, leave J1 absent for more than 20 seconds. Expect
`INTENTIONAL_BACKUP_SHUTDOWN reason=MAIN_TIMEOUT`, followed by loss of board power
and serial. The capture tool recognizes a disconnect immediately following that
marker as `EXPECTED_POWER_OFF`. Check the remaining bank voltage with the meter.
Restore J1 before reopening serial, then use `status` to inspect the previous
`MAIN_TIMEOUT_20S` NVS checkpoint. Charging, backup and exterior switches start
disabled after power restoration; D7 resumes its heartbeat. Charging needs an
explicit new `transfer-start` command.

The supervision policy uses the existing, uncalibrated ADC-derived raw bank
voltage at GPIO2, scaled by 9.2:

| Condition | Behavior |
|---|---|
| MCU loop running after successful initialization | GPIO4/D7 toggles every 500 ms, regardless of VCAP or charging state. D9 indicates 3.3 V power. |
| Bank reaches at least 10 V with valid main power | Arm GPIO42 and require backup-switch PG. |
| VCAP 20.0–20.95 V, span no more than 0.15 V for 30 s, STAT high, backup PG valid | Report ready for the endpoint meter check. |
| Real-sensor ADC outside 1300–2350 mV while charging | Latch a fault and stop charging; no thermistor override. |
| VCAP reaches 21.0 V ADC, expected PG/enable state fails, or charge plateau is not reached within two hours | Latch a fault and request shutdown of charging and exterior loads. Preserve an armed backup for supervision. |
| Main absent for 20 seconds | Log/persist `MAIN_TIMEOUT_20S`, then explicitly release GPIO42. The ESP32 powers off with charge remaining in the bank. |
| Main absent, backup armed, VCAP at or below 7.0 V ADC before timeout | Earlier low-energy cutoff: log/persist `LOW_VOLTAGE_CUTOFF`, then release GPIO42. |

The 7 V cutoff is for this unloaded board test, not a validated threshold for
external loads. A previous unloaded meter check showed 20.67 V actual versus
about 20.23 V ADC, so the ADC thresholds and plateau do not establish calibrated
bank protection or charger accuracy.

`transfer-stop` always requests charging shutdown. With main power valid it also
releases the backup arm. On backup it preserves GPIO42 and continues the original
20-second deadline and voltage supervision; it does not abruptly remove the
ESP32's only supply. If stopped on backup, send `transfer-stop` again after main
returns to release the arm and finish monitoring. Charging never restarts
automatically on source return or reset. A fresh run requires off/stopped state,
valid main power, and an explicit `transfer-start` with renewed PD validation.
Reset only with main connected; reset itself releases backup.

Repeated main removal and return are supported within one armed run. The `cycle`
counter increments on each new loss, and backup confirmation, duration and the
20-second deadline apply to that episode. Charging stays off after the first
loss until a new explicit charging run is started.

`status` reports the current session tag, reset reason, previous NVS checkpoint,
and heartbeat. `events` replays the latest 48 RAM events. Heartbeats include
monotonic uptime, sequence numbers, PG/enables, VCAP, sample count, maximum service
gap, mux-edge count, backup duration, time remaining, and dropped serial bytes.
`LED_HEARTBEAT` records each verified D7 transition. `StatusHeartbeat` is a reusable
loop-driven helper; it uses no PWM or timer interrupt that could keep blinking
while application processing has stopped. NVS stores one complete record at
milestones such as readiness, main return,
and intentional cutoff; it is not a continuous trace. Check for
`NVS_UNAVAILABLE`, `WRITE_FAILED`, `STATUS_INVALID`, or a new boot/session before
claiming continuity.

Same-session uptime and PG observations during the grace period support an ESP32 continuity result
under the observed conditions. They do not measure transfer droop, microsecond
switch timing, maximum load, available capacitance, charging current, or rail
ripple. Those need external measurements; a missing USB stream or a reset cannot
be treated as a successful transfer.
