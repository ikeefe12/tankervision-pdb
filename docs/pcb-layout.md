# PCB layout and fabrication review

Verified 2026-08-24 against `tankervision-pdb.kicad_pcb`, the matching schematic,
fresh ERC and parity-aware DRC reports, the revision-1.0 manufacturing archive, the
JLCPCB parts-placement rendering, and a targeted audit of the normally ignored
silkscreen checks using KiCad 9.0.7.

## Final CAD status

- Schematic and PCB each contain 452 unique references. Values, footprints,
  manufacturing fields, primary UUIDs, and normalized pad nets match exactly.
- The production BOM contains 444 fitted references in 84 orderable groups. FID1–
  FID3, JP1, and MH1–MH4 are intentionally excluded from the BOM.
- DRC reports 0 violations, 0 unconnected items, 0 footprint errors, and 0
  schematic-parity issues. ERC reports 0 errors and 0 warnings. In particular,
  `solder_mask_bridge` is enabled at warning severity and reports no violations.
- Gerbers, plated/non-plated drill files, drill maps, the top-side position file,
  and a STEP model export all generate without missing-model or plot errors.
- The ordered revision-1.0 BOM contains the same 444 fitted references and the
  ordered position file contains those references plus FID1–FID3. Every position,
  rotation, and side agrees with the saved PCB after applying the export origin;
  no JLCPCB rotation correction is required.
- The board outline is 100 mm × 100 mm. All fitted parts are on the front side;
  the position export contains the 444 fitted BOM references plus three fiducials.

“Clean DRC” above means the rules currently enabled by the project. The two
intentional footprint-library exclusions, ordered filled-and-capped-via process, and
optional ignored silkscreen checks are documented below.

## Stackup and routing

The saved board is four-layer, 1.6 mm FR-4 with 35 µm (1 oz) copper on all four
layers:

```text
F.Cu / 0.10 mm prepreg / In1.Cu / 1.24 mm core / In2.Cu / 0.10 mm prepreg / B.Cu
```

In1.Cu is an uninterrupted GND plane. In2.Cu is predominantly GND with selected
power/signal routing. The board uses extensive ground stitching and filled ground
zones on both outer layers. Main power paths use 2–3 mm traces and copper zones;
narrow 0.2–0.5 mm branches on those nets are sense or local component connections,
not the intended full-current trunks.

The project minimums are 0.152 mm copper clearance and track width, 0.5 mm
copper-to-edge clearance, 0.6/0.3 mm minimum via diameter/drill, and 0.15 mm minimum
via annular width. The placed routing normally uses 0.3 mm signal traces and
0.7/0.3 mm through vias. The resulting 0.20 mm via annular ring meets the selected
fabricator's published recommended value for a 1 oz multilayer board.

## Critical placement review

- U19 and its input/output capacitors are compact, and the `+VBUS`,
  `+VCAP_VALID`, and `+VBUS_SS` paths use wide copper. The final nominal output
  capacitance remains approximately 241.1 µF.
- U1 and U13 have their high-frequency input capacitors, inductors, output
  capacitors, bootstrap parts, and feedback components grouped tightly around the
  converters. Feedback routing is kept away from the principal switch copper.
- U11, Q4/Q6, L3, R40, and their ceramics form a coherent top-layer power stage.
  SRP and SRN leave the shunt as a paired Kelvin route on B.Cu rather than sharing
  the high-current path. Switching-node copper is localized. Final switching,
  thermal, and EMI validation is still a bench requirement.
- U5, Q1, D1, R9, and L1 have dedicated power copper and nearby ceramics. The
  gate/current-sense and boost switching loop is longer than the smallest possible
  evaluation-module-style placement, so ringing, loss, and EMI must be checked at
  maximum charger power.
- IC1 is at the board edge with its PCB antenna extending beyond the board. The
  short native-USB pair is routed over the In1 ground reference; its approximately
  1 mm length skew is immaterial at ESP32-S3 full-speed USB rates.
- The four grounded M3 mounting holes deliberately connect the mechanical hardware
  to board GND. Integration must treat that as an intentional chassis/enclosure
  bonding choice.

These checks agree with the manufacturers' layout priorities for
[LM5155](https://www.ti.com/lit/ds/symlink/lm5155.pdf),
[BQ24640](https://www.ti.com/lit/ds/symlink/bq24640.pdf), and
[TPS51385](https://www.ti.com/lit/ds/symlink/tps51385.pdf). The antenna placement
also follows Espressif's
[ESP32-S3 PCB-layout guidance](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/pcb-layout-design.html).

## Fabrication decisions

### 1. Solder-mask bridging is resolved

The board now uses 0.05 mm global solder-mask expansion and a 0.10 mm minimum mask
bridge. Opening merges are explicitly allowed between pads within the same
footprint, which accommodates fine-pitch and custom footprints without globally
suppressing the bridge check. `solder_mask_bridge` is enabled at warning severity;
the current board reports zero bridge violations.

For a 1 oz multilayer board, JLCPCB currently publishes 1:1 pad/opening capability,
a 0.10 mm minimum mask bridge, and at least 0.09 mm between an opening and a
neighboring trace. The final Gerber should still be inspected through the
fabricator's production-file confirmation, as for any dense mask design.

Reference: [JLCPCB PCB capabilities](https://jlcpcb.com/capabilities/Capabilities).

### 2. Epoxy-filled and capped vias were ordered

Via-in-pad use has been reduced. A geometric audit of the current PCB finds 23
0.7/0.3 mm via centers within solderable pad outlines: three in U5's exposed pad,
five in U8's exposed pad, four in U11's exposed pad, and eleven distributed among
smaller pads. The revision-1.0 production order specifies **Epoxy Filled & Capped**
via covering for the board's 1,859 through vias.

JLCPCB describes this process as non-conductive epoxy fill followed by copper
capping. It is the appropriate ordered treatment for the remaining via-in-pad
locations and removes the earlier reliance on ordinary via plugging. Production-
file confirmation remains enabled so any process reinterpretation can be caught
before fabrication.

Reference: [JLCPCB via-covering guidance](https://jlcpcb.com/help/article/pcb-via-covering).

### 3. Two graphical library mismatches are intentionally excluded

DRC contains explicit `lib_footprint_mismatch` exclusions for J1
`CONN12_C-31-M-12_HRO` and J4 `CONN_S2B-PH-K-S_JST`. Their local footprint copies
deliberately adjust the pin-1 indicator. Pad numbers, copper geometry, drills, and
connectivity are unaffected. The two exclusions are intentional for this release
and are not schematic-to-PCB parity errors.

### 4. Silkscreen and test access

The board's functional labels have been updated. J6 is now marked `+20V DC IN`;
the accepted electrical range remains **18.5–20 V**. Enabled `silk_overlap` DRC is
clean.

There are no dedicated `TP*` footprints. The prototype can be probed at connector,
component, or via copper, but the extensive transient-validation plan is easier and
safer with accessible GND plus `+VBUS`, `+VCAP_VALID`, `+VBUS_SS`, `+5V_SS`,
`+3V3_SS`, `PMUX_ST`, and key enable test points. Treat their omission as an
accepted prototype/testability decision if no space remains.

`silk_over_copper` and `silk_edge_clearance` remain ignored. A temporary audit finds
20 mask-clipping and three edge-clipping warnings, mainly footprint outlines and
pin-1 circles. They may be trimmed in the fabricated legend but do not alter copper,
mask openings, assembly placement, or connectivity.

## Revision 1.0 JLCPCB order record

Order placed 2026-08-24 using
`manufacturing/rev-1_0/tankervision-pdb-mfr-rev-1_0.zip`. This records the
order-screen values so the
manufactured configuration can be reproduced. The JLCPCB product-detail screen
reports 110 mm × 100 mm, while the KiCad source outline remains 100 mm × 100 mm;
the order also specifies that JLCPCB adds assembly rails/fiducials, which accounts
for the production-format dimension without changing the source-board outline.

### PCB fabrication

| Parameter | Ordered value |
|---|---|
| Gerber file | `tankervision-pdb-rev-1_0_Y16` |
| Base material / material type | FR-4 / FR4 TG135 |
| Layers / thickness | 4 / 1.6 mm |
| Product-screen dimension | 110 mm × 100 mm |
| PCB quantity / different designs | 5 / 1 |
| Product type | Industrial/Consumer electronics |
| Delivery format | Single PCB |
| Stackup | No requirement; no layer sequence specified |
| Solder mask / silkscreen | Green / white |
| Silkscreen technology | Ink-jet printing |
| Via covering | **Epoxy Filled & Capped** |
| Minimum via hole / diameter | `0.3 mm / (0.4/0.45 mm)` as reported by JLCPCB |
| Via plating method | Horizontal Electroless Copper Plating |
| Surface finish | Lead-free HASL |
| Outer / inner copper | 1 oz / 1 oz |
| Electrical test | Flying Probe Fully Test |
| Appearance quality | IPC Class 2 Standard |
| Production-file confirmation | Yes |
| Mark on PCB | Remove Mark |
| Board-outline tolerance | ±0.2 mm (Regular) |
| Gold fingers / castellations / edge plating | No / No / No |
| Press-fit / blind slot / countersink / backdrill | No / No / No / No |
| Deburring or edge rounding | No |
| 4-wire Kelvin / impedance report / inspection report | No / No / No |
| UL marking | No |
| Paper between PCBs | No |
| Package box | With JLCPCB logo |
| Shortage preference | Require Full Quantity |
| PCB-screen build time / weight | 3 days (PCBA Only) / 0.59 kg |

### PCBA

| Parameter | Ordered value |
|---|---|
| PCBA type / assembly side | Standard / top side |
| PCBA quantity | 5 |
| Panel format | 1 × 1 |
| Edge rails / fiducials | Added by JLCPCB |
| Confirm parts placement | Yes |
| File handling | Complete file; proceed with supplied files |
| Solder paste | High temperature |
| Packaging | Antistatic bubble film |
| Depanel boards and edge rail before delivery | No |
| Bake components / photo confirmation | No / No |
| Board cleaning / conformal coating | No / No |
| Special stencil / stencil storage / fixture storage | No / No / No |
| PCBA flying-probe / function test | No / No |
| Nitrogen reflow | No |
| Assembly remark | No |
| Product description | Research/Education/DIY/Entertainment/DIY; HS code 902300 |
| PCBA-screen build time / weight | 5–6 days / 1.46 kg |

The bare PCB order includes JLCPCB's full flying-probe electrical test. The PCBA
screen separately records no assembly-level flying-probe or functional test; the
assembled boards therefore still require the bring-up procedure in
[issues.md](issues.md).

### JLCPCB placement-rendering audit

The uploaded position file has 447 unique top-side rows: all 444 fitted BOM
references plus FID1–FID3. Its coordinates and rotations match the PCB exactly.
The JLCPCB rendering was then checked against PCB pad numbers, polarity marks, and
connector keying for all 130 orientation-sensitive fitted references:

- IC1 and U1–U51 have their pin-1 marks on the intended PCB pad-1 corners. This
  includes U8 CYPD3177, U11 BQ24640, U19 TPS2121, U22–U24/U35–U38 LM73100, and
  U26/U43 TCA9535.
- Q1–Q18 and D1–D41 agree with their pad-1, cathode, or package-dot markers. The
  green indicators D7 and D25 (`C965815`, XL-2012UGC) have JLCPCB's `+` marker on
  pin 2/anode; the PESD3V3S1UL parts D11 and D17–D19 retain pin 1/cathode on the
  protected signal and pin 2/anode on GND.
- Polarized capacitors C8–C11 and C78/C79 have the rendered `+` on PCB pad 1.
- CN1 has `+VCAP` on pin 1 and GND on pin 2. J1/J6 open toward the left board edge,
  J2 opens toward the bottom edge, J7–J10 open toward the right edge, and the J3–J5
  keying agrees with the footprints and accessible board edges.
- SW1/SW2 and all remaining package dots, cathode bars, LED polarity marks, and
  connector outlines agree with the PCB. Rotated red reference text in JLCPCB's
  viewer is only an overlay and is not a component-orientation indicator.

No reversed, mirrored, or incorrectly rotated fitted component was found in the
provided JLCPCB rendering. The submitted placement may proceed without an
orientation correction.

## Release interpretation

Static connectivity, placement, routing, plane continuity, copper-rule, and
model/export checks pass. The previous solder-mask and functional-silkscreen items
are resolved. The two graphical library exclusions are explicitly accepted, and
the order records epoxy-filled-and-capped vias plus production-file and placement
confirmation. Converter stability, ringing, EMI, thermal rise, hot plug, power-mux
transfer, overload behavior, and firmware sequencing still require the bench tests
listed in [issues.md](issues.md).
