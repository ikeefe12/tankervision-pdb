# JETSON POWER BTN — remote button and state sense

**Schematic label:** `JETSON POWER BTN`

**Key parts:** J5, Q11, D11/D19, R153/R155/R174

J5 lets firmware request an orderly Jetson shutdown and observe a 3.3 V-compatible
power-state signal.

```text
J5.1 JET_ON_FB ── D11 ESD ── GND
                 └ R155 5.1 kΩ ─┬─ U43.P04
                                  └─ R174 49.9 kΩ ── GND

J5.2 PWR_BTN* ─── Q11 drain; Q11 source ── GND
                 └ D19 ESD ── GND
Q11 gate ── JET_PWR_BTN_CTRL from U43.P14; R153 49.9 kΩ to GND
J5.3 GND
```

R155/R174 scale `JET_ON_FB` by 49.9/(5.1+49.9), approximately 0.907, and R174
defines the expander input low when the external signal is absent.

Q11 is an open-drain button press, so the PDB does not drive a voltage into an
unpowered Jetson. A high U43.P14 state presses the active-low button. Firmware should
use a characterized momentary pulse for shutdown, reserve a long press for fault
recovery, and verify state using `JET_ON_FB`.

Verify J5 pinout and acceptable sense voltage against the exact Jetson carrier and
harness. This block is control only; Jetson power comes through one of the protected
output ports.
