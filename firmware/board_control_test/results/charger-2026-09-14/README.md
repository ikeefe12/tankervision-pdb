# Charger characterization without the external supercapacitor

**The bypass, unloaded regulation, restart, current selection, and restored
temperature inhibition tests passed. A subsequent meter hold measured 20.67 V
against the 20.629 V schematic target.** The user confirmed that CN1 had no
supercapacitor connected.

Final run: **2026-09-14 19:26:37 UTC / 15:26:37 America/Toronto**, ESP32-S3 MAC
`68:ee:8f:58:b0:10`. Firmware uses the schematic-verified U26 P11 CE, P12 ISET,
P13 TS selection, and P10 boost enable. Backup and exterior outputs stayed off.

| Test | Settled VCAP estimate, mean | Final-window range | Charger STAT |
|---|---:|---:|---|
| Real sensor selected, open sensor, CE high | 0.6197 V | 0.6072–0.6256 V | blinking |
| Fixed TS bypass, low 1.03 A selection | 20.2289 V | 20.1756–20.2400 V | steady high |
| Restart with fixed TS bypass | 20.2234 V | 20.1756–20.2400 V | steady high |
| Fixed TS bypass, high 1.94 A selection | 20.2268 V | 20.1756–20.2400 V | steady high |
| Return to real open sensor, CE high | 0.6204 V | 0.6164–0.6256 V | blinking |

The charger input PG and boost PG remained asserted throughout the monitored
boost-on phases. CE, TS select, and ISET register values were checked against
physical expander pin readings. Both current selections produced the same settled
voltage to the resolution of this test. **The stated 1.03/1.94 A values describe
selected profiles; output current was not measured with the bank absent.**

GPIO1 stayed near 2.909 V in both temperature modes. This is expected because it
measures the real sensor branch. U14 selects the fixed nominal 1.65 V divider for
U11 TS during bypass; there is no ESP32 ADC connection to U11 TS itself.

## Voltage accuracy and sampling limits

The schematic target is **20.629 V**. This run's ESP32 estimate was approximately
**20.229 V**, about **0.400 V / 1.94% lower**. In the subsequent
[unloaded meter hold](../hold-2026-09-14/README.md), the user reported **20.67 V**
with a multimeter: about **0.20% above target**, inside the calculated
20.36–20.94 V tolerance at this operating point. This resolves the apparent
low-output concern. The ADC estimate is approximately **0.44 V / 2.1% below**
the meter result; the ADC/divider path still requires calibration. The divider
continues to use nominal ×9.2 scaling. The original ADC logs are unchanged.

The [ESP32-S3 datasheet, ADC characteristics](https://documentation.espressif.com/esp32-s3_datasheet_en.pdf)
lists up to ±50 mV total ADC error at the selected attenuation under its stated
conditions, equivalent to ±0.46 V after ×9.2 scaling before divider/board effects.
That indicates the scale of uncertainty, not a measured error bar for this board.

Each voltage sample averages 16 ADC readings. Data are grouped into 10 ms
min/max/mean bins; digital status is refreshed nominally every 20 ms. The longest
observed ADC gap was approximately **8.0 ms**. CE setters took roughly 9–10 ms;
capture starts after their readback returns. The analog path has approximately
1.1 ms filtering. Consequently, the observed maximum **20.2492 V** is a sampled
estimate, not a measurement of fast overshoot or switching ripple.

Regulation was judged using a deliberately broad 19.8–21.4 V ADC screen, no more
than 0.5 V spread, and steady STAT in the final two seconds. The measured final
spread was approximately **0.0644 V** in the three enabled bypass cases. A sampled
21.5 V guard was never triggered. It is a software bench guard, not a substitute
for the charger's hardware protection.

## Temperature-selection timing finding and correction

The first run used 100 ms settling after restoring the real open sensor. VCAP
reached approximately 20.22 V for about **0.29 seconds after the CE call returned**,
then fell toward the inhibited level. This is consistent with the BQ24640's
**400 ms typical temperature-out-of-range qualification delay**, with about
100 ms already elapsed before CE was re-enabled. The datasheet specifies 20 ms
for qualification back into the valid range.

The reusable `setThermistorOverride()` now keeps CE low and waits **100 ms after
selecting bypass, or 500 ms after selecting the real sensor**, in addition to the
existing 100 ms current-decay interval before switching. The final run's restored
real-sensor phase remained below **0.6532 V for the entire capture**, eliminating
the high-voltage pulse seen with the short delay on this board.

The first log says PASS because its inhibition criterion checked the settled
window only. The final criterion checks the entire open-sensor capture below
2 V, as well as STAT transitions. Both original and final logs are preserved.
This distinction matters when reviewing the first trace.

`begin()` and `allOff()` also restore the real sensor while leaving CE low; they do
not add this post-selection wait themselves. Call the explicit selector before an
immediate restart when the TS qualification wait is required. A final power manager
should independently reject an invalid real-sensor reading before requesting charge.

## Shutdown and artifacts

Shutdown traces were buffered immediately after verified CE-low, before serial
printing. In the low-current bypass case, the first shutdown sample was about
20.019 V and it fell to **1.564 V after 1.5 seconds**. The following disabled phase
continued down below 0.04 V. Other bypass cases showed similar decay. CE-off is
therefore effective; the rail does not discharge instantaneously despite no external
bank because the board still has roughly 16.7 µF of nominal VCAP capacitance.

The final result was `RESULT CHARGER_DEEP PASS cleanup=PASS`, with internal output
latch/pins `00/00`, exterior output latch `00`, low current selected, and the real
sensor restored. No schematic or PCB edits were required.

- [Final voltage plots](hardware-final.png) / [SVG](hardware-final.svg)
- [Final raw serial log](hardware-final.log), [normalized CSV](hardware-final.csv), [JSON summaries](hardware-final.summary.json)
- [First run including the return pulse](hardware.png), [raw log](hardware.log)
- [Final independent status read](final-status.log)
- [Final build](build-final.log), [verified upload](upload-final.log), [source/binary hashes](build-manifest.json)
- [15 passing host tests](host-tests.log), including temperature-selection timing and failure handling

Primary behavior references: [BQ24640 datasheet](https://www.ti.com/lit/ds/symlink/bq24640.pdf),
sections 6.5, 7.3.7, and 7.3.12–15; the
[BQ24640 EVM guide](https://www.ti.com/lit/ug/sluu410/sluu410.pdf), section 2.4.2,
explicitly tests unloaded output regulation with steady PG/STAT.
