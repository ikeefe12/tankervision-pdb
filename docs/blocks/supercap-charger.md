# SUPERCAP CHARGER — BQ24640 fixed-voltage charger

**Schematic label:** `SUPERCAP CHARGER`

**Key parts:** U11 BQ24640, Q4/Q6, L3, R40, U12/U14 TS5A3159,
U10 TVS2700, J4 thermistor connector

## Function

U11 converts boosted `+24V` into a synchronous CC/CV charge for the external bank
at CN1. The fixed target is 20.63 V. Firmware selects 1.03 A or 1.94 A charge
current and normally uses the real thermistor; a fixed TS override is retained for
controlled diagnostics.

The previous programmable VFB switch/digipot is absent. VFB is a direct trace to the
fixed divider, matching the charger configuration validated by the designer.

## Power stage

- `+24V` passes through D8 to `/VBUS_SCC`; Q4/Q6 and L3 10 µH form the synchronous
  buck stage. R40 = 10 mΩ is the SRP/SRN current shunt.
- D8 prevents the charged bank from feeding the boost output when U5 is off.
- U10 TVS2700 protects `+24V`. R33 10 Ω/C42 1 µF filter U11 VCC; REGN and
  bootstrap components follow the BQ24640 application topology.
- The completed PCB keeps Q4, Q6, C48, L3, R40, and the input/output ceramics in a
  coherent top-layer power stage with localized switch-node copper. SRP/SRN leave
  R40 as a paired Kelvin route on B.Cu rather than sharing the high-current path.
  Scope PH, gate drive, charge current, and VFB and validate EMI/temperature under
  maximum-power charging.

## Fixed charge voltage

R43 = 300 kΩ from `+VCAP` to `/SCC_VFB`; R50 = 34 kΩ from `/SCC_VFB` to GND.
Both are ±0.1%, ±25 ppm/°C precision 0805 parts:

`V_TARGET = 2.1 V × (1 + 300/34) = 20.629 V`.

There is no feed-forward capacitor across R43. The final route keeps VFB away from
the principal switch-node copper; validate loop stability and transient response
with the real bank cable and bank ESR.

Including the BQ24640 ±0.7% full-temperature feedback specification, initial
resistor tolerance, VFB leakage, and conservative independent temperature drift
gives an estimated **20.36–20.94 V** charge range.

The charger OVP decision shares VFB. The accepted system architecture adds
independent ESP32 monitoring through the isolated `VCAP_ADC` path; the external bank
provides cell balancing and temperature protection, and its wiring includes a fuse
close to the stored-energy source.

## Charge-current selection

U12 defaults low through R116:

| `SCC_ISET_SW` | Divider from `+3V3_SCC` | Nominal current |
|---:|---|---:|
| 0 | R41 150 kΩ / R45 10 kΩ | 1.03 A |
| 1 | R42 150 kΩ / R46 20 kΩ | 1.94 A |

These use `I_CHG = V_ISET/(20 × R40)`. The higher setting is approximately 40 W
into a full bank before losses and must remain within the boost, switch, source,
thermal, inductor, connector, and negotiated USB-contract budgets.

The two settings create about 10.3 mV and 19.4 mV across R40. TI specifies current
accuracy at discrete 5/20/40 mV conditions, so characterize actual low-range current
rather than assigning both settings the headline 40 mV accuracy.

## Temperature selection

U14 defaults to the real thermistor path through R117:

- low: J4 NTC network (`SCC_TS_ADC`) through R47/C56 to U11 TS;
- high: fixed R56/R59 = 100 kΩ/100 kΩ midpoint, overriding charger TS faults.

R62 6.2 kΩ pulls the sensor node to `+3V3_SCC`; R69 47 kΩ provides open-sensor
bias and ESP32 IO1 reads the node. An open sensor rises to approximately 2.91 V and
a short falls near 0 V, so both wiring failures suspend charging while the real path
is selected. The fixed midpoint is a service mode, not an unattended default.

The BQ24640 temperature-out-of-range qualification delay is 400 ms typical;
temperature returning to the valid range has a 20 ms typical delay. Keep CE low
while changing U14, and allow the selected network and fault detector to settle
before re-enabling it. The tested `setThermistorOverride()` API waits 100 ms after
selecting the fixed divider and 500 ms after restoring the real sensor, in
addition to the CE-low decay interval before switching. A 100 ms real-sensor wait
allowed an approximately 300 ms charging pulse during the no-bank bench test;
the 500 ms wait removed that pulse on the tested board.

ESP32 IO1 remains connected to the real sensor branch when the override is active.
It does not measure the selected voltage at U11 TS. See the
[charger characterization](../../firmware/board_control_test/results/charger-2026-09-14/README.md)
for unloaded voltage, status, timing, and measurement limits.

## Enable and status

- `SCC_EN`: U26.P11 → U11 CE, with R115 49.9 kΩ default-low.
- `SCC_PG_N`: U11 PG → U15 → `SCC_PG`, Q8/D12, U26.P03.
- `SCC_STAT_N`: U11 STAT → U16 → `SCC_STAT`, Q9/D13, U26.P04.

Both conditioned signals are active high. `SCC_PG` indicates a valid charger
input, not that CE is asserted or charging is occurring. It can remain high with
the boost disabled because L1/D1/D8 pass the input supply through to U11. Evaluate
`SCC_STAT` over time and alongside CE, thermistor voltage, and bank voltage; a
blinking D13 is status activity, not proof of successful charging.

With a charged bank and boost off, the passive charger-input voltage can be lower
than the bank. BQ24640 then sleeps and deasserts PG even when main power has
returned. The real-bank transfer test observed charger PG low and TS ADC near
zero during this condition, followed by recovery as bank voltage fell. Use the
input-switch PG and mux status to assess main return; qualify the thermistor
after enabling boost and allowing the charger reference to recover.

U26 has no reset pin. An ESP32-only reset can leave CE and profile selection in
their previous states while `+3V3_SS` remains powered. This retained behavior is an
accepted architecture choice; firmware must read and reconcile U26 before writing.

## Release checks

- Confirm external bank series count, voltage rating, balancing, temperature
  protection, and external fuse.
- Validate both charge currents across source voltage, USB contract, boost ripple,
  supercap ESR, and thermal limits.
- Keep `SCC_EN` low while changing U12/U14 and while starting or stopping U5.
- Scope VFB, PH, charge current, and bank voltage during startup, current selection,
  TS faults, input loss, and input return.

Primary reference: [BQ24640 datasheet](https://www.ti.com/lit/ds/symlink/bq24640.pdf).
