# ESP32-S3 and support circuits

**Schematic labels:** `ESP32-S3`, `ESP32 LEDs`, `BOOT + RESET`,
`USB-C->ESP32 (NO PWR)`, `ESP32 UART0`

**Key parts:** IC1 ESP32-S3-WROOM-1-N8R8, U17 USBLC6-2SC6, J2/J3, SW1/SW2

IC1 runs from backed `+3V3_SS`. Local 100 nF/10 µF decoupling is present. EN uses
R48 5.1 kΩ to `+3V3_SS`; C60 10 µF and SW2 pull it low. SW1 pulls IO0 low for boot
mode. The N8R8 module reserves IO35–IO37 for octal PSRAM; all three are no-connect
in the final schematic.

## Interfaces

- J2 is USB data only. R57/R58 provide 5.1 kΩ Rd on CC; D14/D15 protect CC; U17
  protects D+/D− before IO19/IO20. J2 VBUS biases the ESD device but does not power
  the board.
- J3 exposes RXD0, TXD0, and GND. D17/D18 are PESD3V3 clamps.
- D9 is the red `+3V3_SS` power LED. IO4 `STAT_LED` drives Q5/D7 green with R34
  holding the gate low by default.

Nine analog signals use ADC1-capable IO1–IO3 and IO5–IO10; IO2 receives isolated
`VCAP_ADC`, while IO4 is the digital `STAT_LED`. IO11 is `PMUX_ST` through R44/R49,
IO42 directly drives `VCAP_EN`, and IO12 is unused. Control I²C is on IO17/18, PD
HPI I²C is on IO40/41, expander interrupts are on IO15/16, and `PD_INT` is on IO39.
See [system-architecture.md](../system-architecture.md) for the complete pin map.

**Pinout result:** GPIO35–GPIO37 remain no-connect, `CTRL_SDA`/`CTRL_SCL` are on
GPIO17/GPIO18, the control-bus interrupts are on GPIO15/GPIO16, and the PD HPI bus
and interrupt are on GPIO40/GPIO41 and GPIO39. All assignments are valid for N8R8,
and all nine analog inputs remain on ADC1 GPIO1–GPIO10.

GPIO39–GPIO42 double as optional pad-JTAG signals. They are committed here to
`PD_INT`, PD I²C, and `VCAP_EN`, so do not select pad JTAG in firmware/eFuses. IO3
`VIMON_DC` is also the JTAG-source strapping input, but the
default eFuse configuration ignores that strap. Do not program the JTAG-selection
eFuses in a way that makes its analog level control the debug path. The default USB
Serial/JTAG path remains available through J2.

On the completed PCB the module antenna extends beyond the board edge, with no board
copper beneath the radiating end. The short IO19/IO20 native-USB route is referenced
to the solid In1 GND plane. Preserve antenna clearance in the enclosure and verify
RF performance in the assembled product.

The fitted N8R8 module is specified for −40 to +65 °C ambient by default. Espressif
permits operation to +85 °C when PSRAM ECC is enabled, at the cost of 1/16 of usable
PSRAM. Treat +65 °C as the board limit unless that firmware/configuration choice is
made and validated.

Because U26/U43 remain powered from the same backed rail, resetting IC1 does not
reset their outputs. Charger and port enables therefore need deliberate retained-
state handling; see [issues.md](../issues.md).

Primary reference: [ESP32-S3-WROOM-1/1U datasheet](https://documentation.espressif.com/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf).
