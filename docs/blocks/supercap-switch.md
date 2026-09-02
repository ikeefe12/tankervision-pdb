# SUPER CAP SWITCH — backup-path arming

**Schematic label:** `SUPER CAP SWITCH`

**Key parts:** U24 LM73100, CN1 XT30, U21 TVS2200, U29 TLV7031

U24 connects raw bank rail `+VCAP` to U19 IN2, `+VCAP_VALID`, only while ESP32 IO42
asserts `VCAP_EN`. R38 49.9 kΩ holds the path off during reset or brownout.

- Raw `+VCAP` has U21 TVS2200 and approximately 16.8 µF of local ceramic
  capacitance rather than the former 100 µF bulk. CN1 connects the external bank.
- R85/R96 = 487 kΩ/27 kΩ set 22.84/20.75 V OVLO.
- R87/R94 = 82 kΩ/30 kΩ set 4.48/4.07 V PGTH. U29, powered by
  `+3V3_VCAP`, produces `VCAP_PG` for U26.P02 and Q14/D24.
- C99 4.7 nF provides the controlled output ramp, annotated at approximately 47 ms
  for the stated load-capacitance condition.
- R127 is the IMON load; D28/R110/C61 feed `VIMON_SC` to ESP32 IO10.
- U24 reverse blocking prevents charging through the mux path. U11 is the intended
  charger.

`VCAP_PG` only means the U24 output exceeded its low PG threshold; it does not prove
that enough energy remains to regulate `+5V_SS`. Firmware must use `VCAP_ADC` and a
load-aware shutdown threshold.

Once armed, U19 performs source selection without firmware intervention. Direct
IO42 control intentionally releases U24 if the ESP32 browns out during backup,
returning the board to its default-off state. Validate failover at maximum load
before the backed logic rail can reach its brownout threshold.

U24 OVLO protects the mux path, not the direct U11-to-bank charging connection. The
accepted protection allocation uses precise VFB regulation, isolated ESP32 bank
monitoring, external cell balancing/temperature protection, and an external fuse
near the bank.
