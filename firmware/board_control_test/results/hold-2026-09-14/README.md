# VCAP meter hold — 2026-09-14

Uploaded the explicit hold-on-boot firmware to ESP32-S3 `68:ee:8f:58:b0:10` at the
user's request, with the external supercap disconnected. **The charger was left
enabled for the meter measurement.** This differs from the earlier bounded tests.

The firmware selects the fixed thermistor bypass, low-current setting, boost and
charger CE. It qualifies regulation and then monitors it without a duration limit.
The reusable implementation is `ChargerHold.h/.cpp`; build using `build-hold.sh`.
The uploaded binary's source hashes and explicit build flag are recorded in
[build-manifest.json](build-manifest.json).

Observed in the [20-second capture](hardware.log) and subsequent
[8-second reconnect capture](reconnect.log):

- Hold state `HOLDING`, automatic boot mode enabled, no reported fault.
- U26 output `0x0B`: boost, CE and TS override asserted; high-current selection
  and unbacked 5 V converter off. U43 output `0x00`: exterior switches/button off.
- USB input, main selection, backed 5 V, boost PG, charger PG and STAT all high.
- Backup and exterior output PGs low, as expected with those switches disabled.
- VCAP ADC approximately **20.23 V**. Continuously sampled hold range was
  20.1664–20.2400 V in the first capture and 20.1940–20.2400 V after reconnect.
  The longest reported monitor interval was 28 ms across the captures.
- GPIO1 remained approximately 2.909 V: it reads the real open sensor branch.
- Both captures observed a USB-triggered reset and automatic return to holding.
  Closing the final capture did not issue an `off` command; the uploaded build
  automatically starts the hold again if the USB close resets the MCU.

The user subsequently reported a **20.67 V multimeter reading** on VCAP during
this hold test, following the requested measurement at CN1 pin 1 (+VCAP) relative
to pin 2 (GND). This is **0.041 V / 0.20% above the 20.629 V schematic target**,
supporting correct unloaded regulation with the temperature bypass enabled.
Compared with the approximately 20.23 V ADC estimate, the firmware voltage reading
is about **0.44 V / 2.1% low** at this operating point. The meter result resolves
the apparent low-output concern; calibration of the ADC/divider measurement path
remains outstanding. No firmware scaling change was made from this single point.

`CAP=0` reports the disabled backup switch output and does not indicate raw VCAP
is low. ADC samples do not establish switching ripple or transient peaks.

[Build](build.log) and [upload](upload.log) completed successfully with flash data
hashes verified. All [23 host checks](host-tests.log) passed, including eight new
hold-mode checks for qualification, sustained operation, stop and fault behavior.
Those are mock-based software tests; the voltage observations above came from the
connected board. [Parsed hardware summary](hardware-summary.json) verifies the
observed output/PG combinations and final hold state.

Send `off` at 115200 baud to stop within the current session. Send `charger-hold`
to restart. **Every reset/power-up restarts this special build**, including a
serial-triggered reset; upload the normal `build.sh` variant to restore off-at-boot
behavior. No shutdown or serial reconnect was performed when recording the user's
meter result; the measurement hold was left running.
