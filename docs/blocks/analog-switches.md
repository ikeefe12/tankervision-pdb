# TS5A3159 analog switches — final schematic audit

**Key parts:** U3, U9, U12, U14, U25 — TS5A3159ADBVR

All five symbols use the TI DBV pinout and KiCad SOT-23-6 footprint: pin 1 NO,
pin 2 GND, pin 3 NC, pin 4 COM, pin 5 V+, and pin 6 IN. `IN = 0` connects COM to
NC; `IN = 1` connects COM to NO.

Each switch has a local 100 nF V+-to-GND bypass capacitor: U3/C4, U9/C54,
U12/C70, U14/C71, and U25/C89.

The TS5A3159A's powered-off protection keeps its signal-path pins high impedance
with V+ = 0 V. Its control input is also allowed up to 5.5 V independently of V+.
Those properties are required where backed control/analog nodes can outlive the
switch supply.

| Ref | V+ / IN | COM function | IN low → NC | IN high → NO |
|---|---|---|---|---|
| U3 | `+3V3_SS` / divided `+3V3_PDC` | HPI pull-up supply | GND | `+3V3_SS` |
| U9 | `+3V3_SS` / `+3V3_SS` | ESP32 IO2 | GND | `VCAP_ADC` |
| U12 | `+3V3_VBUS` / `SCC_ISET_SW` | U11 ISET | 1.03 A network | 1.94 A network |
| U14 | `+3V3_VBUS` / `SCC_TS_SW` | U11 TS | real J4 thermistor | fixed TS override |
| U25 | `+3V3_SS` / `PMUX_ST` | source-LED supply | D29 red: supercap/IN2 | D25 green: VBUS/IN1 or Hi-Z |

The part's −40 to +85 °C operating range must cover the product ambient range.
If operation above +85 °C is required, choose a wider-temperature part with the
same pinout, powered-off behavior, and analog/control limits.

Primary reference: [TS5A3159A datasheet](https://www.ti.com/lit/ds/symlink/ts5a3159a.pdf).

## U3 — CYPD3177 HPI pull-up gating

R11 10 kΩ from `+3V3_PDC` and R16 100 kΩ to GND drive U3 IN. COM supplies the
top of R26 10 kΩ (`PD_INT`) and R27/R28 2.2 kΩ (`PD_SDA`/`PD_SCL`).

- With `+3V3_PDC` and `+3V3_SS` present, U3 selects NO and the HPI signals pull up
  to `+3V3_SS`.
- With only `+3V3_SS` present, U3 selects grounded NC, holding the unpowered-device
  bus inactive through the pull-up resistors.
- With `+3V3_SS` absent, U3 is unpowered and isolates COM from both throws, so
  `+3V3_PDC` cannot back-power the ESP32 domain through the HPI signals.

This removes the need for firmware to sequence a separate PD-bus enable.

## U9 — `VCAP_ADC` isolation

U9 IN is tied to its `+3V3_SS` supply. When that rail is valid, COM connects ESP32
IO2 to NO and the R195/R198 divider. When the rail is absent, powered-off protection
isolates the charged-bank divider. NC is grounded but is not selected during normal
powered operation.

## U12 — charge-current selection

U12 COM drives `/SS_ISET`. Both selectable dividers use U11's `+3V3_SCC` reference.
With R40 = 10 mΩ and `I_CHG = V_ISET/(20 × R40)`:

| `SCC_ISET_SW` | Divider | ISET | Nominal charge current |
|---:|---|---:|---:|
| 0 | R41 150 kΩ / R45 10 kΩ | 0.206 V | 1.03 A |
| 1 | R42 150 kΩ / R46 20 kΩ | 0.388 V | 1.94 A |

R79 holds IN low while U26.P12 is high impedance, so the hardware default is the
lower current. Switch resistance is negligible relative to the divider resistors;
include leakage and resistor/reference tolerance in final current limits.

## U14 — thermistor/override selection

U14 COM reaches U11 TS through R47 100 Ω; C56 100 nF filters the charger pin.

- IN low/NC selects `SCC_TS_ADC`, the actual J4 sensor network: R62 6.2 kΩ to
  `+3V3_SCC`, R69 47 kΩ to GND, and the external NTC to GND.
- IN high/NO selects R56/R59 = 100 kΩ/100 kΩ, approximately half of
  `+3V3_SCC`, deliberately overriding U11 temperature faults.

R117 holds IN low while U26.P13 is high impedance. The real sensor is therefore the
safe startup/default path. Treat the high state as a controlled diagnostic mode;
the ESP32 ADC is not an independent hardware temperature cutoff.

## U25 — mux-source LEDs

U25 is entirely within the backed `+3V3_SS` domain. U30 drives `PMUX_ST`:

- low selects NC and D29 red, indicating U19 IN2/supercap;
- high selects NO and D25 green, indicating U19 IN1/VBUS or output Hi-Z.

U19 ST does not distinguish IN1 from output Hi-Z, so firmware must combine
`PMUX_ST` with input/status measurements before concluding that VBUS exists.

## Fixed VFB path

U11 VFB is a direct trace to fixed R43/R50 = 300 kΩ/34 kΩ. No analog switch is
present in the feedback loop. This preserves the charger's validated topology and
removes switch leakage, capacitance, resistance variation, and power sequencing
from the regulation node. Both resistors use hand-solderable 0805 footprints so a
future build can fit another fixed target.

## Acceptance checks

- Keep both charger profile selects low before asserting `SCC_EN`.
- Change ISET or TS only with U11 disabled unless the transient is characterized.
- Bench-test loss of `+3V3_VBUS` while the bank remains charged and confirm no
  back-power through U12/U14.
- Verify U3 bus recovery across USB attach/detach and U9 isolation across backed
  rail power-up/down.
- Verify the required ambient range against the +85 °C limit.
