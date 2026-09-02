# TankerVision Power Distribution Board (PDB) — Design Documentation

Verified 2026-08-24 against `tankervision-pdb.kicad_sch` and the completed
`tankervision-pdb.kicad_pcb` using KiCad 9.0.7 and fresh ERC and parity-aware PCB
DRC. Calculations also use the active devices' manufacturers' datasheets.

> The 452-reference schematic and 452-footprint PCB match one-to-one, including
> UUIDs, values, footprint assignments, manufacturing fields, and pad nets. ERC
> and PCB DRC are clean with zero unconnected items; the enabled solder-mask bridge
> rule also passes. The revision-1.0 JLCPCB order uses epoxy-filled-and-capped vias,
> and its BOM, placement file, and rendered component orientations pass the final
> manufacturing-package audit. The exact order parameters and the two accepted
> graphics-only connector-footprint exclusions are documented in
> [pcb-layout.md](pcb-layout.md). Complete the bench checks in
> [issues.md](issues.md) before deployment.

## What this board does

The PDB accepts power from USB-C PD or an **18.5–20 V** DC barrel-jack supply.
Two reverse-blocking input switches feed `+VBUS`. A TPS2121
then selects `+VBUS` or an armed supercapacitor bank to supply the backed `+VBUS_SS`
domain. An always-on 5 V buck and 3.3 V LDO boot the ESP32-S3, which controls the
charger, backup switch, auxiliary buck, and four protected output ports through two
TCA9535 expanders.

The intended loss-of-input behavior is a short TPS2121 switchover to the supercap,
followed by firmware-controlled load shedding and an orderly Jetson shutdown.

## Block index

Schematic labels, rather than drawing coordinates, are the stable way to find each
circuit.

| Document | Schematic label(s) | Key parts |
|---|---|---|
| [blocks/usbc-pd.md](blocks/usbc-pd.md) | `USBC PD` | J1, U8 CYPD3177, U2, U3/U4 |
| [blocks/input-inrush.md](blocks/input-inrush.md) | `USBC INRUSH CTRL`, `DC INRUSH CTRL` | U22/U23 LM73100 |
| [blocks/common-lm73100-power-switch.md](blocks/common-lm73100-power-switch.md) | repeated topology | U22–U24, U35–U38 |
| [blocks/power-mux.md](blocks/power-mux.md) | `POWER MUX`, `2V5 VCAP VREF` | U19 TPS2121, U51 LM4040 |
| [blocks/supercap-switch.md](blocks/supercap-switch.md) | `SUPER CAP SWITCH` | U24 LM73100, CN1 |
| [blocks/supercap-charger.md](blocks/supercap-charger.md) | `SUPERCAP CHARGER` | U11 BQ24640, U12/U14 TS5A3159 |
| [blocks/boost-24v.md](blocks/boost-24v.md) | `24.5V BOOST` | U5 LM5155 |
| [blocks/buck-5v.md](blocks/buck-5v.md) | `20V->5V SS BUCK`, `20V->5V BUCK` | U1/U13 TPS51385 |
| [blocks/ldos.md](blocks/ldos.md) | `3V3 VBUS LDO`, `3V3 VCAP LDO`, `3V3 SS LDO` | U48–U50 |
| [blocks/output-switches.md](blocks/output-switches.md) | four output-switch labels | U35–U38 LM73100 |
| [blocks/esp32.md](blocks/esp32.md) | ESP32/support labels | IC1, U17 |
| [blocks/io-expanders.md](blocks/io-expanders.md) | internal/external I²C GPIO | U26/U43 TCA9535 |
| [blocks/comparators.md](blocks/comparators.md) | distributed status conditioning | U4/U6/U7/U15/U16/U18/U27–U30/U44–U47 |
| [blocks/analog-switches.md](blocks/analog-switches.md) | PD-bus gating, ADC isolation, charger selection, mux indication | U3/U9/U12/U14/U25 TS5A3159 |
| [blocks/jetson-power-btn.md](blocks/jetson-power-btn.md) | `JETSON POWER BTN` | J5, Q11 |
| [blocks/adc-monitoring.md](blocks/adc-monitoring.md) | `VCAP ADC DIV` plus IMON networks | R195/R196/R198, U9, D26–D28/D38–D41 |
| [system-architecture.md](system-architecture.md) | — | power flow, control map, pin map, sequencing |
| [pcb-layout.md](pcb-layout.md) | — | final PCB parity, stackup, routing, fabrication decisions |
| [issues.md](issues.md) | — | open design concerns and release checks |

## Power rails

| Net | Source | Nominal | Supercap-backed? | Purpose |
|---|---|---:|:---:|---|
| `+VBUS_USB` | J1 / USB-PD VBUS | 5 V before contract; 20 V requested | — | raw USB input |
| `+VBUS_DC` | J6 | 18.5–20 V required | — | raw DC input |
| `+VBUS` | U22/U23 ideal-diode OR | 18.5–20 V during valid operation | no | main input rail |
| `+24V` | U5 boost | 24.5 V | no | charger input |
| `+VCAP` | U11 / external bank at CN1 | 0–20.63 V target | is backup | raw supercap rail |
| `+VCAP_VALID` | U24 | equals VCAP | — | armed mux IN2 |
| `+VBUS_SS` | U19 mux | VBUS or VCAP | yes | backed root rail |
| `+5V_VBUS` | U13 buck | 5.1 V | no | unbacked 5 V |
| `+5V_SS` | U1 buck | 5.1 V | yes | backed 5 V |
| `+3V3_VBUS` | U49 LDO | 3.3 V | no | VBUS-domain supervision/switch supply |
| `+3V3_VCAP` | U50 LDO | 3.3 V | self | VCAP-domain supervision |
| `+3V3_SS` | U48 LDO | 3.3 V | yes | ESP32, expanders, backed logic |
| `+3V3_PDC` | U8 internal regulator | 3.3 V | no | USB-PD controller domain |
| `+3V3_SCC` | U11 VREF | 3.3 V reference | no | charger ISET/TS networks only |
| `+2V5_VCAP_VREF` | U51 shunt reference | 2.5 V | from VCAP_VALID | mux crossover reference |

`_SS` means downstream of the power mux and therefore available during the intended
safe-shutdown interval. The 3.3 V rails are deliberately separate domains and must
not be treated as interchangeable.

## Repeated circuit patterns

- Seven LM73100 protected switches provide inrush control, reverse blocking, power
  good, and current monitoring. See
  [common-lm73100-power-switch.md](blocks/common-lm73100-power-switch.md).
- Fourteen TLV7031 comparators condition open-drain status signals; U4 converts the
  CYPD3177 active-low PFET gate signal into `VBUS_USB_EN`. See
  [comparators.md](blocks/comparators.md).
- Five TS5A3159 SPDT switches gate the PD-bus pull-ups, isolate `VCAP_ADC`, select
  charger current, select the real/fixed TS network, and steer mux-source LEDs. The
  charge-voltage divider is fixed and has no analog switch. See
  [analog-switches.md](blocks/analog-switches.md).
- Each expander-driven enable has a 49.9 kΩ pulldown. `VCAP_EN` is driven directly
  by ESP32 IO42 and has its own 49.9 kΩ default-low resistor.
