# OUTPUT POWER SWITCHES — four switched jacks

**Schematic labels:** `VBUS POWER SWITCH`, `5V VBUS POWER SWITCH`, `SS POWER SWITCH`,
`5V SS POWER SWITCH`

**Key parts:** U35–U38 LM73100; J7–J10; U31–U34 input TVS; U39–U42 output TVS

All four are instances of the
[common LM73100 circuit](common-lm73100-power-switch.md). U43 controls them and
49.9 kΩ pulldowns hold every enable low while its expander pin is high impedance.

| Port | Switch | Rail → connector | Backed? | EN / PG / IMON |
|---|---|---|:---:|---|
| VBUS | U35 | `+VBUS` → J7 `VBUS_OUT` | no | U43.P11 / U43.P00 via U45 / IO6 |
| 5 V VBUS | U36 | `+5V_VBUS` → J8 `+5V_VBUS_OUT` | no | U43.P10 / U43.P01 via U44 / IO5 |
| SS | U37 | `+VBUS_SS` → J9 `+VBUS_SS_OUT` | yes | U43.P12 / U43.P02 via U46 / IO7 |
| 5 V SS | U38 | `+5V_SS` → J10 `+5V_SS_OUT` | yes | U43.P13 / U43.P03 via U47 / IO8 |

The 20 V ports use TVS2200; the 5 V ports use TVS0500. D30–D33 clamp negative
output excursions. LEDs D35/D34/D36/D37 show VBUS/5 V VBUS/SS/5 V SS power-good.

Both 20 V ports use 22.84/20.75 V OVLO and an approximately 47 ms ramp. U35 uses
17.07/15.51 V PGTH on the unbacked VBUS port; U37 uses 9.91/9.00 V PGTH so the
backed VBUS port remains indicated across more of the supercap discharge range.
U36 and U38 both use 7.04/6.40 V OVLO, 4.48/4.07 V PGTH, and an approximately
84 ms ramp.

Design notes:

- 5.5 A is a device ceiling, not a guaranteed connector/system rating. Include
  switch dissipation, copper, connector, source, and mux/buck limits.
- `VBUS_OUT` can be anywhere in the accepted input range; its load must tolerate it.
- J6 and J7–J10 use the same PJ-202AH connector. A supply inserted into a 5 V output
  can overstress the TVS0500 even though U36/U38 block reverse current into the board.
- LM73100's 5.5 A continuous rating is not a precise output current limit. Its
  approximately 21.9 A fixed fast trip/latch-off behavior is accepted; rate/fuse
  external loads and bound firmware retries accordingly.
