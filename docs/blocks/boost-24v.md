# 24.5V BOOST — LM5155 charger supply

**Schematic label:** `24.5V BOOST`

**Key parts:** U5 LM5155, Q1 IPC50N04, L1 6.8 µH, D1 PMEG060V100,
R9 12 mΩ, U6 TLV7031

U5 boosts `+VBUS` to 24.5 V for the supercap charger. The circuit follows TI's
24 V/2 A-class application values but uses a 12 mΩ current-sense resistor; validate
the limit, ripple, magnetics, and thermals over the specified 18.5–20 V source range
and deliberate brownout/invalid-source tests.

| Item | Schematic value / result |
|---|---|
| output | R10/R20 = 47 kΩ/2 kΩ; 1.00 V × (1 + 47/2) = 24.5 V |
| switching frequency | approximately 434 kHz from R17 = 49.9 kΩ |
| nominal peak-current threshold | 100 mV / 12 mΩ = 8.3 A |
| soft start | C30 = 220 nF; 220 nF × 1 V / 10 µA = approximately 22 ms |

Q1 is the low-side switch; R9 is its source shunt; R4/C27 filter CS. D1 rectifies
into the 24 V output. R15/C31/C32 form compensation. The completed PCB gives
Q1/D1/R9/L1 dedicated wide copper, ground stitching, and nearby ceramics. The
gate/current-sense and boost switching loop is still longer than the smallest
possible evaluation-module-style placement, so scope SW/GATE/CS and verify loss,
ringing, EMI, and temperature at maximum charger power.

`24V_VBUS_EN` comes from U26.P10 with R115 49.9 kΩ default-low. U5 PGOOD is pulled
up and conditioned by U6, then drives `24V_VBUS_PG` to U26.P05 and Q2/D2.

The charger is the only intended load. At 1.94 A into a nearly full 20.63 V bank,
the stage must supply roughly 40 W plus loss. At the minimum valid 18.5 V input,
that is at least 2.2 A before loss and about 2.4 A at 90% efficiency. Coordinate
this with the negotiated USB contract or DC-source rating.

Primary reference: [LM5155 datasheet](https://www.ti.com/lit/ds/symlink/lm5155.pdf).
