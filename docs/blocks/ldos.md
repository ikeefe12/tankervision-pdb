# 3V3 LDOs — separate logic domains

**Schematic labels:** `3V3 VBUS LDO`, `3V3 VCAP LDO`, `3V3 SS LDO`

| Ref | Input → output | Part | Main loads |
|---|---|---|---|
| U49 | `+VBUS` → `+3V3_VBUS` | TLV70933, 150 mA/30 V | VBUS-domain comparators/LEDs; U12/U14 supplies |
| U50 | `+VCAP` → `+3V3_VCAP` | TLV70933, 150 mA/30 V | U29 and VCAP status indication |
| U48 | `+5V_SS` → `+3V3_SS` | TLV1117LV33, 1 A/5.5 V | ESP32, U26/U43, mux status/LED switch, I²C pull-ups |

The rail separation prevents supervision domains from directly feeding one another.
It also means U12/U14 can lose V+ while the BQ24640-derived analog networks remain
alive; their TS5A3159 powered-off protection is therefore required.

U48 uses the correct TLV1117LV pinout: pin 1 GND, pin 2 and tab OUT, pin 3 IN. Its
ceramic input and output capacitance exceed the 1 µF nominal datasheet requirement;
effective output capacitance must remain above 0.5 µF after bias and temperature.
The normal 5.1 V input is below the 5.5 V recommended maximum.

At a 350 mA SS-domain load, U48 dissipates `(5.1−3.3) × 0.35 = 0.63 W`. The final
PCB provides local copper and ground stitching, but a static CAD review cannot
establish junction temperature. Validate it with the real ambient, enclosure, and
ESP32 radio duty cycle; this is a thermal test item, not a pinout or stability
concern.

Two other named 3.3 V rails are not general logic LDO outputs: `+3V3_PDC` comes from
U8 CYPD3177 while USB is present, and `+3V3_SCC` is U11's BQ24640 reference used for
charger analog networks.

Primary references: [TLV1117LV datasheet](https://www.ti.com/lit/ds/symlink/tlv1117lv.pdf)
and [TLV709 datasheet](https://www.ti.com/lit/ds/symlink/tlv709.pdf).
