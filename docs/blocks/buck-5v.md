# 20V→5V BUCKS — TPS51385 ×2

**Schematic labels:** `20V->5V SS BUCK`, `20V->5V BUCK`

**Key parts:** U1/U13 TPS51385; U7/U18 TLV7031

Both converters produce 5.1 V with a 150 kΩ/20 kΩ feedback divider. The power
stages use 1.5 µH inductors, 3×22 µF output capacitance, 2×10 µF input capacitance,
100 nF soft start, and out-of-audio mode.

| | Backed SS buck | Unbacked VBUS buck |
|---|---|---|
| converter | U1 | U13 |
| input/output | `+VBUS_SS` → `+5V_SS` | `+VBUS` → `+5V_VBUS` |
| enable | R1/R6 30 kΩ/10 kΩ self-enable; approximately 5.2 V on | U26.P14 `5V_VBUS_EN`, R114 default-low |
| PG | U7 → `5V_SS_PG` → U26.P07, Q3/D3 | U18 → `5V_VBUS_PG` → U26.P06, Q10/D16 |

Validated nominal values:

- VOUT = 0.6 V × (1 + 150/20) = 5.1 V.
- Soft start = 100 nF × 0.6 × 1.4 / 5 µA = approximately 16.8 ms
  (C25 for U1, C74 for U13).
- Charging a 2000 µF load to 5.1 V in 16.8 ms is approximately 0.61 A.
- 5 A is the design budget; it is not implied merely by the device's 7 A headline rating.

U1 is on the critical boot path and continues from the supercap after failover until
its input is too low to regulate. `VCAP_PG` remains high below that point, so it is
not a valid low-energy cutoff.

Primary reference: [TPS51385 datasheet](https://www.ti.com/lit/ds/symlink/tps51385.pdf).
