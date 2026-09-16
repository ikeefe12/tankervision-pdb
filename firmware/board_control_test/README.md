# TankerVision PDB control and power-good bench firmware

For automatic operation and Jetson integration, see the separate
[deployment firmware](../deployment/README.md) and [USB API](../deployment/API.md).
The procedures and hardware results below describe bench test builds, whose
20-second cancellable timeout differs from deployment's fixed 60-second shutdown.

Standalone Arduino ESP32 sketch for the fitted ESP32-S3-WROOM-1-N8R8. The reusable
`BoardControl.h/.cpp` driver contains hardware operations; `TestRunner.cpp` contains
the serial test application. The normal build starts tests only on a serial
command. The explicit `build-hold.sh` variant automatically enables the unloaded
charger after each reset for the requested meter measurement (see below).

For the connected **21.6 V supercap bank**, use the separate
[real-bank charging and transfer test](TRANSFER-TEST.md), built with
`build-transfer.sh`. It validates a 20 V/2 A PD request, charges with the real
temperature input, blinks D7 as a 1 Hz MCU heartbeat, and monitors backup takeover
with reset and uptime evidence. If main power remains absent for **20 seconds**,
it lowers GPIO42 to shut the board down with charge left in the bank. Main return
within that window cancels shutdown. Charging starts only on `transfer-start`. Keep its
`capture_transfer.py` serial session open throughout backup operation.
The [20-second shutdown results](results/shutdown-20s-2026-09-14/README.md) confirm
return cancellation, intentional timeout/power-off, retained bank charge, and
default output states after restart with the heartbeat running.
The [USB recovery diagnostic](USB-RECOVERY-TEST.md), built with
`build-usb-recovery.sh`, adds retained USB event/state evidence and a host capture
that observes intentional power-off and subsequent serial recovery. It preserves
the same charging, heartbeat and shutdown behavior.
The [USB recovery run](results/usb-recovery-2026-09-14/README.md) recovered serial
with J2 untouched after a delayed host reopen. Retained evidence distinguishes
the natural power-on boot from a second USB-induced reset caused by that reopen;
the earlier persistent silence was not reproduced.
The [real-bank results](results/transfer-2026-09-14/README.md) record a 20.67 V
meter-confirmed endpoint and two backup intervals (20.8 and at least 48 seconds)
without an ESP32 reset, with main-power return detected after each.

The first board passed **53 converter/port checks** plus the charger CE/PG test.
See the [2026-09-14 bench report](results/2026-09-14.md) for raw logs and limits.
The later [charger characterization](results/charger-2026-09-14/README.md) exercises
both temperature selections without the external supercap, including the TS
qualification timing fix. A subsequent [meter hold](results/hold-2026-09-14/README.md)
gave a user-reported **20.67 V** against the **20.629 V** target; the ADC/divider
measurement path still needs calibration.
The latest [tests 1/2/3/6](results/control-next-2026-09-14/README.md) passed backup
switching, both expander interrupt paths, and D7 control with user-confirmed visible
blinking. The PD interface was also exercised.
That upload restored normal off-at-boot behavior. Its PD communications passed,
but its zero operating-current request did not establish a usable charge budget.
The new transfer variant explicitly requests and verifies a sufficient contract
before enabling charging; see the transfer results for hardware validation.

The pin map was checked against a fresh KiCad XML export of
`tankervision-pdb.kicad_sch` and PCB pad nets on 2026-09-14. The earlier documentation
had incorrect input-bit assignments on **both** expanders and swapped CHG LED
references. See `schematic-map.json` and the corrected
[expander documentation](../../docs/blocks/io-expanders.md).

## Build and upload

Validated build toolchain: Arduino CLI 0.35.3, ESP32 Arduino core **3.3.3**. No
third-party Arduino libraries are needed. The build selects 8 MB flash, octal
PSRAM, and native USB Serial/JTAG CDC; it does not change eFuses or use pad JTAG.

```sh
sh firmware/board_control_test/build.sh
```

`build.sh` uses `ARDUINO_CLI`, `arduino-cli` on PATH, or the macOS Arduino IDE's CLI.
On another machine install `esp32:esp32@3.3.3` and use the same board options in
the script. Native USB is the **data-only J2** connector; power the board separately
at J1 (USB-C PD), or J6 with JP1 configured as documented.

Example upload using esptool 4.8.6 (replace the serial device):

```sh
python /path/to/esptool.py --chip esp32s3 --port /dev/cu.usbmodem2101 \
  --before usb_reset --baud 460800 write_flash --flash_size 8MB \
  0x0000 firmware/board_control_test/build/board_control_test.ino.bootloader.bin \
  0x8000 firmware/board_control_test/build/board_control_test.ino.partitions.bin \
  0xe000 /path/to/esp32/core/tools/partitions/boot_app0.bin \
  0x10000 firmware/board_control_test/build/board_control_test.ino.bin
```

The existing flash backup, when captured, is `results/pre-test-flash.bin` (ignored
by Git). `build/` contains the executable artifacts and is also ignored.

## First test

Use a valid 20 V USB-PD source and leave JP1 closed. Alternatively use only an
18.5–20 V DC source with JP1 open. For the first test, leave J7–J10 unloaded, or
connect only loads explicitly suitable for repeated power cycling and the labeled
voltages. This firmware does not read the negotiated PD current budget and is
therefore intended for unloaded control testing first.

Open serial at 115200 and send `status`, then `test`. For a timestamped capture:

```sh
python firmware/board_control_test/capture.py --port /dev/cu.usbmodem2101 \
  --command test --seconds 40 --log firmware/board_control_test/results/basic.log
```

The basic sequence reconciles retained outputs, checks the input and backed buck,
cycles the boost and unbacked buck, and cycles J7/J8/J9/J10 individually. Each
tested PG must be stable for 100 ms within 2 seconds. It checks other port PGs stay
low while one port is enabled. Each enabled stage has a 1.5-second LED dwell.
PG and input validity are polled throughout that dwell. `observation_ms` is the
post-command checking interval, not an oscilloscope measurement of startup time;
enable functions themselves also wait for stable PG before returning.
Errors stop the sequence and trigger cleanup; a failed read never becomes a PG
value. The final state has all controlled converters and output jacks disabled.

**J8 dependency:** `setPort(Port::FiveVoltVbus, true)` enables U13 and waits for
`5V_VBUS_PG` before enabling U36. Disabling J8 leaves the buck available to other
callers; the test explicitly disables it afterward. Disabling a buck under an
enabled J8 or disabling boost under an enabled charger is rejected.

| Stage | Expected electronic observation | LED / connector |
|---|---|---|
| Input baseline | exactly one of USB/DC PG high; backed buck PG high | D22 USB / D23 DC; D25 main selection; D3 backed 5 V |
| Boost U5 off → on → off | `24V_VBUS_PG` 0 → 1 → 0 | D2 |
| Buck U13 off → on → off | `5V_VBUS_PG` 0 → 1 → 0 | D16 |
| J7 U35 off → on → off | `EXT_VBUS_PG` 0 → 1 → 0 | D35; input-domain voltage |
| J8 U36 off → on → off | buck PG first, then `EXT_5V_VBUS_PG` 0 → 1 → 0 | D34; nominal 5.1 V |
| J9 U37 off → on → off | `EXT_SS_PG` 0 → 1 → 0 | D36; backed input-domain voltage |
| J10 U38 off → on → off | `EXT_5V_SS_PG` 0 → 1 → 0 | D37; nominal 5.1 V |
| Charger supply | `SCC_PG` high with valid charger input, including CE low | D12 CHG PG |

U1 backed buck and the three LDOs have no ESP32 enable. Their power presence can
be observed but firmware cannot toggle them individually. U22 USB and U23 DC input
switches are hardware-controlled, also without an ESP32 enable.

The disabled asynchronous boost still passes input power through L1/D1. **Boost
off is not a disconnected charger input.** BQ24640 PG indicates input validity,
not CE state or charging current. Its STAT can blink for CE low or TS faults. Thus
neither `SCC_PG` remaining high nor blinking D13 alone establishes a failure.
See [TI BQ24640 sections 7.3.14–15](https://www.ti.com/lit/ds/symlink/bq24640.pdf)
and [LM5155 PGOOD definition](https://www.ti.com/lit/ds/symlink/lm5155.pdf).

## Additional commands and full-board test set

| Command / bench step | Preconditions and evidence |
|---|---|
| `status`, `watch` | raw expander input/output bytes and decoded PG, mux source, VCAP and thermistor ADC; `watch` toggles periodic output |
| `control-next` | **No loads or supercap.** Tests backup-switch control from regulated VCAP, both expander interrupts, read-only PD HPI, and three visible D7 pulses. Ends with controlled outputs off; see sequence below |
| `pd` | Read-only CYPD3177 identity, raw version bytes, active fixed PDO/RDO and interrupt status on the dedicated bus; reports negotiated power budget, not measured current |
| `led-on`, `led-off` | visually confirm GPIO4 controls D7; D9 is the fixed 3.3 V power LED |
| `adc` | all nine ADC1 channels in averaged calibrated millivolts; compare VCAP ×9.2 to a meter and calibrate IMON against known loads |
| `charger-inhibit` | real NTC path electrically open, confirmed by TS ≥2.7 V; normally run with CN1 and J4 disconnected. Enables/disables CE briefly with open-NTC protection retained and no TS override. Checks input PG and expander pin readback; demonstrates CE control only, not charging |
| `charger` | connect a correctly rated, balanced, fused bank and real NTC; verify source current budget first. Uses low 1.03 A profile for about 2 seconds, checks TS/VCAP and supply PG, then disables CE before boost. Measure actual bank current externally |
| `charger-deep` | **CN1 supercap disconnected**, real sensor branch ≥2.7 V. Compares open-sensor inhibition with fixed-TS bypass, unloaded regulation at both current profiles, restart, and return to the real sensor; buffers voltage traces and shutdown decay locally |
| `charger-hold` | **CN1 supercap disconnected**. Holds VCAP high with fixed TS bypass and low-current selection until `off` or a detected fault. Other power tests require `off` first |
| `backup` | connect a charged bank (driver accepts 6–21 V) and retain valid main input. Cycles GPIO42 and checks VCAP PG on/off. Does not perform source-loss transfer |
| `off` | requests controlled converters and ports off; driver retains an active backup supply when main input is absent |
| Current selection | final firmware calls `setChargeHighCurrent()` only while CE is low. Validate both 1.03 A and 1.94 A against a meter with source budget and bank configured |
| TS selection | `setThermistorOverride(true)` selects the fixed divider; `false` restores the real NTC. Both require CE low and at least 100 ms charge-current decay. After selection, the function waits 100 ms for bypass or 500 ms for the real path to allow TS qualification. The `charger-deep` command exercises both selections; hot/cold calibration still needs a sensor fixture |
| Jetson button/feedback | call `setJetsonButton()` for a bounded pulse with J5 connected; observe J5 contact behavior and `jetsonOn` feedback. No Jetson button pulses are issued by the first test |
| Expander interrupts | probe GPIO15/16 while inducing PG transitions and correlate active-low INT with an input read; basic polling does not prove interrupt delivery |
| PD HPI | read-only identity, negotiated PDO/RDO and available current on GPIO40/41, address 0x08; verify GPIO39 interrupt on attach/contract events before adding source-budget control |
| Backup transfer | with bank and load characterized, scope main/backup rails and PMUX_ST during source removal/return; establish a load-aware energy cutoff. Requires dedicated supervision firmware, not the basic test loop |
| Reset retention | after enabling a known unloaded channel, reset ESP32 and verify logged retained state and off reconciliation; cold power cycle should restore TCA9535 input defaults |
| Load/analog validation | known resistive/capacitive loads, DMM/scope checks at each connector, current calibration and thermal measurements; PG alone does not prove regulation accuracy or load capacity |

## Next control sequence: tests 1, 2, 3 and 6

Build the normal `build.sh` variant, upload, then send `control-next`. The external
ports and CN1 must remain unloaded. This replaces the automatic meter-hold build;
reset and completion return the controlled outputs to off. No current ADC channels
are read by this sequence.

1. With controlled outputs off, `PdDiagnostics` uses the independent I2C controller
   on GPIO40/41 at 100 kHz, address 0x08, with a 25 ms bus timeout. It records three
   snapshots of CYPD3177 identity, raw version bytes, PD status, active PDO/RDO and
   GPIO39/interrupt status. It issues register-pointer reads only. It does not
   renegotiate power, reset the controller, or claim an unobserved PD event test.
2. `ExpanderInterrupts` installs GPIO15/16 edge handlers which only capture counts,
   timestamps and pending flags. I2C reads and acknowledgments stay in the main
   task. This preserves evidence of short interrupt pulses cleared by driver reads.
3. Enable boost and charger with fixed TS and the low-current profile. Qualify
   VCAP in the existing 19.8–21.4 V ADC screening window with steady STAT. Cycle
   GPIO42 three times while checking backup PG, main selection, regulation, and
   the corresponding U26 interrupt. For backup-off, observe INT low before a
   status read and confirm that the changed backup PG is read and INT releases.
4. Cycle the unbacked buck and each exterior port, observing the expected PG bits,
   interrupts on the correct expander, and released IRQ lines after reads. Keep
   the charger regulating for steady STAT. Pre-enable the buck for J8 so its port
   transition can be distinguished from the upstream converter transition.
5. Disable controlled power stages, then pulse GPIO4/D7 on for 1.5 seconds and off
   for 1.5 seconds, three times. Firmware reports pin readback; a person must
   confirm actual visible LED operation. Final GPIO4 and GPIO42 are low.

Backup arming is testable without a supercap because the charger maintains raw
VCAP. The standalone `backup` command does not establish that supply; use this
combined sequence for the no-bank setup. Source-loss transfer is not attempted.
Disconnecting J1 would also remove the ESP32's power in this setup, so live PD
detach/reattach interrupt validation remains a separate fixture test.

```sh
python firmware/board_control_test/capture.py --port /dev/cu.usbmodem2101 \
  --command control-next --seconds 50 --log /tmp/control-next.log
```

## Charger characterization without a supercap

Send `charger-deep` for the complete bounded sequence. The nominal CV target is
20.629 V. TI also tests the BQ24640 EVM with the load off in its
[EVM guide, section 2.4.2](https://www.ti.com/lit/ug/sluu410/sluu410.pdf), so unloaded
operation should reach regulation; no external bank is needed for this check.

Each active phase clears CE before emitting buffered CSV. Voltage is sampled using
the driver's 16-reading ADC average; 10 ms min/max/mean bins are retained, and
status/enable pin readback is checked nominally every 20 ms. The capture reports
the actual longest gap. An additional 1.5 seconds of shutdown readings are buffered
immediately after CE-low readback, before serial output. The CE call latency and
approximately 1.1 ms ADC filtering limit what can be concluded about fast edges.

The software trips above a measured 21.5 V. The final two seconds of a regulation
phase must stay within 19.8–21.4 V, span no more than 0.5 V, and report steady STAT
high. This is a coarse ADC screening window, **not** validation of the charger's
tighter 20.36–20.94 V calculated tolerance or switching ripple. Open-sensor phases
must remain below 2 V throughout the capture and exhibit STAT transitions.

The BQ24640 specifies a typical 400 ms temperature-out-of-range deglitch interval.
The first characterization used only 100 ms between restoring the real path and
asserting CE: VCAP briefly reached the high rail before the TS fault stopped
charging. The reusable selection function now waits 500 ms with CE low when
restoring the real sensor. This timing wait does not measure U11 TS or replace
normal firmware checks of the real sensor voltage.

GPIO1 reads the real sensor branch regardless of the selection. Its approximately
2.91 V reading should remain unchanged while U14 applies the fixed approximately
1.65 V divider to the charger's TS pin. The firmware has no ADC on U11 TS itself.

```sh
python firmware/board_control_test/capture.py --port /dev/cu.usbmodem2101 \
  --command charger-deep --seconds 85 --log /tmp/charger.log
python firmware/board_control_test/analyze_charger.py /tmp/charger.log --plot
```

The analyzer exports normalized CSV, JSON, and optional PNG/SVG plots (`--plot`
requires matplotlib). The bypass API has no autonomous timeout; the characterization
sequence supplies its own bounded interval and restores CE, boost, bypass, current
selection, backup, and exterior enables to off/default. The separately requested
meter hold below instead keeps the unloaded rail on under continuous polling.

## Leave VCAP high for a meter measurement

The [2026-09-14 hold report](results/hold-2026-09-14/README.md) records the uploaded
automatic hold variant and its live/reconnect verification. It was left running
for the requested meter check. The user then measured **20.67 V** with a multimeter.

With the external supercap disconnected, build the explicit automatic hold variant
and upload it using the same esptool command above:

```sh
sh firmware/board_control_test/build-hold.sh
```

This build selects `setThermistorOverride(true)`, the low 1.03 A current limit,
and enables boost followed by charger CE. It leaves the external output switches,
unbacked 5 V converter, and backup switch off. It intentionally starts again after
**every ESP32 reset**, including a reset caused by opening/closing USB serial.
The ordinary `build.sh` build still starts with all controlled outputs off.

Measure **CN1 pin 1 (+) to CN1 pin 2 (−/GND)**. The schematic target is approximately
**20.63 V**. The user measured **20.67 V**, about 0.2% above target. The ADC estimate
of about 20.23 V is approximately 0.44 V / 2.1% low at this operating point;
ADC/divider calibration remains outstanding. `VCAP_PG`/`CAP` remains low because
it senses the disabled backup switch's output, not raw VCAP at CN1.

`ChargerHold::start()`, `service()`, and `stop()` provide reusable mode control.
The application polls every approximately 20 ms without waiting for USB. Startup
must qualify VCAP at 19.8–21.4 V with STAT high for 100 ms within two seconds.
While holding, it checks that voltage window, supply/boost/charger PG, output
configuration/readback, and inactive output PGs. A reading above 21.5 V trips
immediately even during startup. These are software ADC checks, not measurements
of switching ripple or calibrated protection thresholds. Faults latch and request
all outputs off; `status` reports the fault and shutdown result. No automatic retry
occurs within the running session.

There is **no time limit** while holding. Send `status` or `watch` at 115200 baud
for VCAP and hold-state readings. Send `off` to stop; it stays stopped until an
explicit `charger-hold` command or reset. Power removal ends the measurement;
this special build enables charging again when power returns. Stop before
connecting a supercap or load. Rebuild/upload with `build.sh` when the automatic
measurement mode is no longer wanted.

## Reuse in final firmware

Include `BoardControl.h` and instantiate `BoardControl`. `begin(Stream&)` records
retained expander state and reconciles it to off. All setters return success/failure;
read `error()` immediately after a failure. Output register, direction, polarity,
and driven pin levels are checked. P0 remains inputs; unused P1 pins remain inputs.
The TCA9535 output latch is programmed before enabling its output drivers, following
the [TI register definition](https://www.ti.com/lit/ds/symlink/tca9535.pdf).

The driver is synchronous and intended to be called from one task. A final power
manager must supply source budget policy, continuous voltage/temperature guards,
fault handling, watchdog behavior, and backup sequencing. The initial test records
those remaining validations separately from observed control/PG passes.

Opening or closing the native USB serial device can reset the ESP32 on this host.
The startup register dump is buffered for later `status` requests. Use one open
serial session for manual multi-command experiments, and inspect boot messages
before interpreting state retention across a host reconnect.
