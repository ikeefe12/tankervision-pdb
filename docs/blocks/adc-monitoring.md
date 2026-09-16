# VCAP divider and current monitoring

**Schematic label:** `VCAP ADC DIV`; IMON networks are distributed through the
seven LM73100 blocks.

## Supercap-voltage ADC

The former `VBUS_ADC` channel has been removed and ESP32 IO4 is now `STAT_LED`.
`VCAP_ADC` uses:

```text
+VCAP ─ R195 82 kΩ ─┬─ R196 2.2 kΩ ─ U9 NO
                    └─ R198 10 kΩ ─ GND
U9 COM ─┬─ ESP32 IO2
        └─ C50 100 nF ─ GND
U9 IN and V+ ─ +3V3_SS; U9 NC ─ GND
```

The divider scale is `10/(82+10)`, or input ÷9.2. It gives approximately 2.24 V
at the 20.63 V nominal charge target and 3.01 V at the TVS2200 27.7 V maximum
clamp. With 1% divider corners the latter is approximately 3.07 V, below the
ESP32's 3.6 V absolute maximum.

U9 TS5A3159 is powered by `+3V3_SS` and its IN pin is tied high. While powered it
connects the divider to IO2; while unpowered its powered-off protection makes the
signal path high impedance, preventing a charged bank from back-powering the
ESP32. R196 limits residual transient/clamp current.

During the 2026-09-14 unloaded charger hold, the user measured **20.67 V** with a
multimeter while the nominally scaled ESP32 estimate was approximately **20.23 V**.
The measurement path reads about **0.44 V / 2.1% low** at this operating point.
ADC/divider calibration remains outstanding; no correction was applied from this
single comparison. See the [meter hold report](../../firmware/board_control_test/results/hold-2026-09-14/README.md).

## LM73100 IMON channels

All seven channels use an 820 Ω IMON load, a DDZ9678 1.8 V clamp, 2.2 kΩ series
resistor, and 100 nF at the ESP32. Nominal transfer is:

`V_IMON = I × 181 µA/A × 820 Ω = 0.148 V/A`.

| Signal | Source | Load / clamp / series / filter | ESP32 |
|---|---|---|---|
| `VIMON_USB` | U22 | R122 / D26 / R106 / C58 | IO9 |
| `VIMON_DC` | U23 | R124 / D27 / R108 / C53 | IO3 |
| `VIMON_SC` | U24 | R127 / D28 / R110 / C61 | IO10 |
| `VIMON_EXT_VBUS` | U35/J7 | R180 / D39 / R160 / C44 | IO6 |
| `VIMON_EXT_5V_VBUS` | U36/J8 | R177 / D38 / R161 / C39 | IO5 |
| `VIMON_EXT_SS` | U37/J9 | R187 / D41 / R166 / C46 | IO7 |
| `VIMON_5V_SS` | U38/J10 | R181 / D40 / R168 / C49 | IO8 |

LM73100 IMON accuracy is ±15% at or above 1 A, ±20% from 0.5–1 A, and
unspecified below 0.5 A. Calibrate voltage measurements against a DMM, treat small
IMON readings as qualitative, and use filtered/averaged thresholds in firmware.
