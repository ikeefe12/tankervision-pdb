# Issues, Accepted Constraints, and Release Checks

Verified 2026-08-24 against the current schematic and completed PCB with KiCad
9.0.7, fresh ERC, parity-aware PCB DRC, and a targeted audit of the normally ignored
silkscreen checks. ERC reports **0 errors and 0 warnings**. PCB DRC reports **0
violations, 0 unconnected items, 0 footprint errors, and 0 schematic-parity
issues**. The solder-mask changes pass DRC; the ordered via process and the library
choices below are manufacturing decisions rather than unresolved CAD blockers. See
[pcb-layout.md](pcb-layout.md) for the physical-layout evidence.

The production BOM contains 444 fitted references grouped into 84 unique orderable
parts. Every fitted reference has a manufacturer, manufacturer part number, and
JLCPCB/LCSC part number. The eight intentionally excluded footprints are FID1–FID3,
JP1, and MH1–MH4.

Final principal references are U8 CYPD3177, U19 TPS2121, U22/U23/U24 input and
supercap LM73100 switches, U26/U43 TCA9535 expanders, U11 BQ24640, U48
TLV1117LV33, U49/U50 TLV70933, and U51 LM4040A25.

## Schematic design-freeze status

The ESP32 pinout correction is valid and this review finds no remaining electrical
schematic blocker; the free-text cleanup listed below remains before documentation
freeze. On IC1 `ESP32-S3-WROOM-1-N8R8`:

- GPIO35–GPIO37 are no-connect, as required by the module's octal PSRAM;
- `CTRL_SDA`/`CTRL_SCL` use GPIO17/GPIO18 with R35/R36 2.2 kΩ pull-ups;
- `CTRL_IN_INT_N`/`CTRL_EXT_INT_N` use GPIO15/GPIO16;
- `PD_SDA`/`PD_SCL` use GPIO40/GPIO41 and `PD_INT` uses GPIO39;
- `VCAP_EN` uses GPIO42, `STAT_LED` uses GPIO4, and `PMUX_ST` remains on GPIO11;
- all nine analog measurements use ADC1-capable GPIO1–GPIO3 and GPIO5–GPIO10; and
- native USB remains on GPIO19/GPIO20 and UART0 remains on its module pins.

GPIO39–GPIO42 are also the optional pad-JTAG pins. The board uses them for `PD_INT`,
PD I²C, and `VCAP_EN`, so firmware must retain the default USB Serial/JTAG path or
permanently disable pad JTAG. IO3 `VIMON_DC` is the JTAG-source strap, but the
default `EFUSE_STRAP_JTAG_SEL = 0` configuration ignores its level.

Reference: [ESP32-S3-WROOM-1/1U datasheet](https://documentation.espressif.com/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf).

## PCB and BOM validation status

The current schematic contains 452 unique references and the saved PCB contains the
same 452 references. The mapping is one-to-one: there are no missing, extra, or
duplicate references and no value, footprint, manufacturing-field, primary-UUID,
or pad-net mismatches. C116–C119 are present on the PCB, and the C117/C118 UUID and
rail assignments are correct: C117 bypasses `+3V3_VBUS`; C118 bypasses `+3V3_SS`.
R145 remains synchronized at 487 kΩ; with R159 = 100 kΩ it gives U38 the same
7.04/6.40 V nominal OVLO thresholds as U36.

All 42 assigned footprint names resolve to installed project or KiCad libraries.
The 84 unique fitted parts pass the package-family, package-designator, body/pitch,
pin-count, and pad-number audit. No LCSC code or manufacturer part number maps to
conflicting values or footprints.

The completed four-layer board uses 1 oz copper on all layers. In1 is an
uninterrupted GND plane; In2 is predominantly GND with selected routing. Main power
trunks use wide traces and copper zones, the converter placements follow the
relevant datasheet priorities, U11 SRP/SRN use paired Kelvin routing, and the ESP32
antenna extends beyond the board edge. Static placement/routing review finds no
additional electrical connectivity blocker.

The Default netclass clearance is 0.152 mm globally, not only at U8. This meets the
selected fabricator's copper capability, but it is a board-wide density choice. The
global solder-mask expansion is now 0.05 mm, the minimum mask-bridge width is
0.10 mm, and intentional opening merges are allowed only between pads within the
same footprint. `solder_mask_bridge` is enabled at warning severity and the current
board has no solder-mask-bridge violations.

## Fabrication and release disposition

1. **Solder-mask bridging is resolved.** The board now uses 0.05 mm global
   expansion and a 0.10 mm minimum bridge, with same-footprint pad-opening merges
   explicitly allowed. The bridge rule is no longer suppressed and reports zero
   violations.
2. **Epoxy-filled and capped vias were ordered.** Via use inside pads has been
   reduced. The current board contains 23 via centers within solderable pad
   outlines: three at U5, five at U8, four at U11, and eleven distributed among
   smaller pads. The revision-1.0 order specifies epoxy-filled-and-capped covering
   for all 1,859 through vias, replacing the earlier accepted ordinary plugging
   approach. Production-file confirmation is enabled.
3. **The two footprint-library exclusions are intentional.** J1 and J4 retain
   `lib_footprint_mismatch` exclusions because their local copies have deliberately
   adjusted pin-1 indicators. Pad numbers, copper geometry, drills, and electrical
   connectivity are unaffected; the warnings are intentionally ignored for this
   release.
4. **Functional silkscreen labels are resolved.** The obsolete J6 range label has
   been replaced by `+20V DC IN`, while the electrical documentation retains the
   accepted **18.5–20 V** input range. Enabled silkscreen-overlap DRC is clean. The
   normally ignored optional audit still finds 20 mask-clipping and three
   edge-clipping warnings, primarily footprint outlines and pin-1 circles; these
   can be trimmed by fabrication and do not change connectivity. No dedicated
   `TP*` footprints are present, so component/via probing remains an accepted
   testability decision.
5. **The revision-1.0 JLCPCB placement rendering is accepted.** The ordered BOM
   contains all 444 fitted references and the ordered position file contains those
   references plus FID1–FID3. Coordinates, rotations, and top-side assignments
   match the saved PCB. A visual polarity/pin-1/keying audit of all 130
   orientation-sensitive fitted references found no reversed, mirrored, or
   incorrectly rotated component. See the order record in
   [pcb-layout.md](pcb-layout.md).

## Schematic text cleanup before freeze

The netlist and component values are correct, but several free-text calculation
callouts still contain earlier rounded values. These notes do not affect the PCB,
but they should be corrected so the released schematic agrees with the calculations
below:

- U22 and U35: 22.84/20.75 V OVLO and 17.07/15.51 V PGTH;
- U23: 17.50/15.90 V UVLO, 22.14/20.11 V OVLO, and 17.07/15.51 V PGTH;
- U24: 22.84/20.75 V OVLO and 4.48/4.07 V PGTH;
- U36 and U38: 7.04/6.40 V OVLO and 4.48/4.07 V PGTH; and
- U37: 22.84/20.75 V OVLO and 9.91/9.00 V PGTH.

The POWER MUX note should not describe 22.8 V as a generally valid input: normal
operation is limited to 22 V recommended maximum, the nominal OV threshold is
22.73 V, and 24 V is the absolute maximum. The backed U1 buck note should also use
at least 21 V as its design-input ceiling because the 20.629 V nominal supercap
target can reach approximately 20.94 V at the retained worst-case corner. These
are annotation-only corrections and require no PCB relink.

## Final design basis

- USB-C requests only 20 V: U8 `VBUS_MIN` and `VBUS_MAX` are tied high to
  `+3V3_PDC`. Raw `+VBUS_USB` is 5 V before negotiation; U22 remains disabled until
  U4 validates the active-low `VBUS_FET_EN` state.
- The permitted DC adapter is **18.5–20 V**. U23's R83/R92/R103 ladder gives
  **17.50/15.90 V UVLO** and **22.14/20.11 V OVLO** nominal rising/falling
  thresholds.
- JP1 is default-closed for USB-C and must be opened before DC operation. USB-C and
  DC are specified as mutually exclusive input configurations.
- U19 compares R72/R76 = 34 kΩ/5.62 kΩ against U51's 2.5 V reference. The
  nominal IN1→IN2 crossover is **17.625 V**, with approximately **17.577–17.673 V**
  initial component corners. TPS2121 comparator offset places the expected
  IN2→IN1 return threshold at approximately **17.66–17.91 V**.
- U19 OV1 and OV2 use the final precision dividers R74/R80 and R89/R100 =
  **102 kΩ/4.99 kΩ**, all ±0.1%, ±25 ppm/°C. Nominal rising OV is **22.73 V**.
  Including TPS2121 reference limits, initial resistor tolerance, and pin leakage
  gives approximately **21.60–23.64 V**; a deliberately conservative opposing-TC
  estimate over the full component range is approximately **21.50–23.75 V**.
- The TPS2121 supervisors are not relied upon to protect the mux input pins from an
  excessive external source. U22/U23/U24 and the specified source limits must keep
  both U19 inputs below 24 V absolute maximum; normal operation remains below the
  22 V recommended maximum.
- Nominal `+VBUS_SS` capacitance is approximately **241.1 µF**: 200 µF bulk at
  C78/C79, 20 µF at C80/C81, 1 µF at C103, and 20.1 µF at the U1 input.
- A 17.625→20.629 V lower-to-higher transfer adds about **13.8 mJ** and **0.72 mC**
  to that capacitance. U19 intentionally regulates the total load-plus-charging
  current at its R97-programmed limit (about 4–5 A, 4.5 A typical); this is not an
  expected OCP event. At a 40 W constant-power load, settling is expected to be a
  few hundred microseconds rather than only the 5 µs source-selection interval.
- U11 has a fixed R43/R50 = 300 kΩ/34 kΩ, ±0.1% VFB divider for **20.629 V
  nominal**. Including BQ24640 full-temperature feedback accuracy, resistor
  tolerance/TC, and VFB leakage gives the retained worst-case estimate of
  approximately **20.36–20.94 V**.
- `VCAP_EN` is driven directly by ESP32 IO42 with R38 default-low. An ESP32
  brownout during backup releases U24 and intentionally powers the board down.
- Raw `+VBUS_DC` has C83 = 1 µF. Raw `+VCAP` has approximately 16.8 µF of local
  capacitance rather than 100 µF. Large bulk is behind controlled-slew devices.
- `VBUS_ADC` has been removed and IO4 is now `STAT_LED`. `VCAP_ADC` uses
  R195/R198 = 82 kΩ/10 kΩ, R196 = 2.2 kΩ, and C50 = 100 nF for a ÷9.2 scale.
  U9 connects it to IO2 only while `+3V3_SS` exists. The ADC sees approximately
  3.01 V at the TVS2200 27.7 V clamp, about 3.07 V at 1% divider corners.
- U3 hardware-gates R26/R27/R28, the CYPD3177 HPI interrupt/I²C pull-ups. With
  both `+3V3_PDC` and `+3V3_SS` present they pull up to `+3V3_SS`; with only the
  backed rail present they pull low; with `+3V3_SS` absent the powered-off switch
  isolates them.

## Accepted design constraints

These items are signed off and do not require another schematic change. They must
remain visible to firmware, integration, and test teams.

### TPS2121 transfer behavior

When U19 changes from a lower-voltage source to a higher-voltage source, the selected
channel uses active current limiting while it charges the output capacitance and
supplies the load. A normal transfer is not expected to hit a separate OCP trip.
Sustained or repetitive current limiting, source droop, and selection chatter remain
bench-test cases.

### LM73100 overload behavior

The seven LM73100 paths are rated for 5.5 A continuous current, but the fixed fast
trip is approximately 21.9 A and latches off; it is not a precise 5.5 A current
limiter. This behavior is accepted. Connector loads, harnesses, and external wiring
must be rated and fused appropriately, and firmware retry of a latched channel must
be bounded.

### `VBUS_USB_EN` single-fault limit

U4 logic is valid and R8 provides an independent default-low state. A stuck-high U4
output, open R21, or shorted R14 can still enable U22 before a valid contract. The
design intentionally does not claim single-fault tolerance for that low-probability
combination; attach, detach, reset, reversal, and incompatible-source testing is
required.

### Supercap protection allocation

The precision VFB divider provides normal regulation, and the independent
`VCAP_ADC` path lets firmware disable `SCC_EN` on abnormal bank voltage. The external
bank provides cell balancing and temperature protection, with a correctly sized fuse
physically close to the bank in its external wiring. Board-level tolerance of
simultaneous VFB and monitoring failures is not claimed and is accepted.

### Configured USB/DC separation

JP1 is a user configuration control, not automatic source isolation. Leave it closed
for USB-C, open it for DC, and do not connect USB-C and DC power simultaneously.

## Required firmware behavior

1. On `PMUX_ST` transition, disable the charger, boost, unbacked buck, and unbacked
   outputs; retain only explicitly required shutdown loads.
2. On input return, validate the source and USB contract, then restore loads one at
   a time. Read and reconcile retained U26/U43 state after an ESP32-only reset.
3. After a full TCA9535 power cycle, write safe output-register values before
   changing ports from inputs to outputs; the registers power up high although the
   pins power up as inputs.
4. Define a load-aware minimum `VCAP` threshold. At 40 W the load approaches the
   minimum 4 A U19 limit near a 10 V bank; shed loads and release `VCAP_EN` well
   before losses or transients remove current-limit margin.
5. Do not interpret `PMUX_ST` high alone as proof of VBUS: it means IN1 selected or
   mux output Hi-Z.
6. Read the negotiated PDO/RDO and budget charger plus load current. U8's current
   straps accept any advertised source current and do not enforce a board power
   limit.
7. Select ISET and TS only while `SCC_EN` is low. Enable U5, wait for stable
   `24V_VBUS_PG`, then enable U11. Disable U11 and allow current to decay before
   disabling U5. The asynchronous boost has a passive input-to-output path while
   disabled and is not an output disconnect.
8. Do not select pad JTAG on GPIO39–GPIO42 or enable GPIO3 strap-selected JTAG;
   those pins are assigned to `PD_INT`, PD I²C, and `VCAP_EN`, while GPIO3 carries
   `VIMON_DC`. Use the default USB Serial/JTAG interface through J2 when hardware
   debugging is required.

## Remaining bench and integration checks

- Validate U48 TLV1117LV33 temperature with final SOT-223 copper, ambient,
  enclosure, average ESP32 load, and radio peaks. At a 350 mA rail load it dissipates
  about 0.63 W from 5.1 V.
- Validate connector hot-plug with worst cable inductance. Reduced raw capacitance
  lowers inrush but does not eliminate ceramic/cable ringing.
- Use clear silkscreen and keyed/color-coded harnesses because J6 and J7–J10 share
  the PJ-202AH family. An external source accidentally connected to a 5 V output can
  overstress its TVS despite LM73100 reverse blocking.
- Provide load-local flyback/regeneration handling where required. Reverse blocking
  can trap returned energy on the connector side of an open output switch.
- Default U14 to the real thermistor. The fixed TS path is a time-limited diagnostic
  override with independent voltage and temperature monitoring.
- TS5A3159A U3/U9/U12/U14/U25 and LM4040A25I U51 are specified only to +85 °C
  ambient. Use wider-temperature parts if the product requirement exceeds that.
- IC1 ESP32-S3-WROOM-1-N8R8 is specified for −40 to +65 °C ambient by default.
  Espressif allows +85 °C with PSRAM ECC enabled and a 1/16 reduction in usable
  PSRAM; otherwise +65 °C is the controlling product-ambient limit.

## Minimum bench validation

Capture source voltage/current, `+VBUS`, `+VCAP_VALID`, `+VBUS_SS`, `+5V_SS`,
`+3V3_SS`, `PMUX_ST`, and relevant enables during:

1. USB attach with valid and incompatible sources, cable reversal, PD reset,
   detach, and every supported contract current.
2. DC hot-plug, slow ramp, current limiting, brownout through the crossover, hard
   removal, and return using 18.5–20 V sources plus deliberate invalid-source tests.
3. Mux failover/return at zero and maximum load with VCAP full, half-full, and at
   the minimum firmware-allowed voltage, including repeated interruptions.
4. ESP32 reset on VBUS, ESP32 brownout on backup, full `+3V3_SS` power cycle, and
   retained-expander recovery.
5. Charger/boost sequencing, both current selections, TS faults and override,
   firmware VCAP-overvoltage shutdown, and source return with retained state.
6. JP1 closed with USB-C and JP1 open with DC-only power; confirm `VDC_OUT` remains
   isolated from DC in the open-jumper configuration.
7. Charged-bank connection/removal, worst cable inductance, ADC power-domain
   isolation, and maximum-bank operation across temperature without U19 OV2
   nuisance trips.
8. Output startup into capacitive loads, overloads, shorts, inductive interruption,
   regeneration, and bounded recovery from LM73100 latch-off.

Primary references: [TPS2121](https://www.ti.com/lit/ds/symlink/tps2121.pdf),
[LM73100](https://www.ti.com/lit/ds/symlink/lm7310.pdf),
[BQ24640](https://www.ti.com/lit/ds/symlink/bq24640.pdf),
[TLV1117LV](https://www.ti.com/lit/ds/symlink/tlv1117lv.pdf), and
[CYPD3177](https://www.infineon.com/assets/row/public/documents/24/49/infineon-cypd3177-24lqxq-datasheet-en.pdf).
