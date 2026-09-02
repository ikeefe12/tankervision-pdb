# I²C → GPIO EXPANDERS — internal and external control

**Schematic labels:** `I2C -> GPIO INTERNAL CONTROL`, `I2C -> GPIO EXTERNAL CONTROL`

**Key parts:** U26/U43 TCA9535, powered by `+3V3_SS`

Both devices share `CTRL_SDA`/`CTRL_SCL` with R35/R36 2.2 kΩ pull-ups. The final
schematic connects them to ESP32 IO17/IO18. GPIO35–GPIO37 remain no-connect for the
N8R8 module's octal PSRAM.

U26 has A0–A2 low and uses address 0x20. U43 has A0 high and A1/A2 low and uses
0x21. TCA9535 has no internal pull-ups and no reset input. External 49.9 kΩ
pulldowns define safe enable states after a full expander power cycle.

## U26 — internal, 0x20

INT is `CTRL_IN_INT_N` → IO15, pulled up by R77 10 kΩ.

| Pin | Signal | Direction / meaning |
|---|---|---|
| P00 | `PG_USB` | input, USB switch PG |
| P01 | `PG_DC` | input, DC switch PG |
| P02 | `VCAP_PG` | input, backup switch PG |
| P03 | `SCC_PG` | input, charger PG |
| P04 | `SCC_STAT` | input, charger status |
| P05 | `24V_VBUS_PG` | input, boost PG |
| P06 | `5V_VBUS_PG` | input, unbacked buck PG |
| P07 | `5V_SS_PG` | input, backed buck PG |
| P10 | `24V_VBUS_EN` | output, U5 enable |
| P11 | `SCC_EN` | output, U11 CE |
| P12 | `SCC_ISET_SW` | output; low 1.03 A, high 1.94 A |
| P13 | `SCC_TS_SW` | output; low real NTC, high fixed override |
| P14 | `5V_VBUS_EN` | output, U13 enable |
| P15–P17 | unused | — |

`VCAP_EN` is no longer an expander output; ESP32 IO42 drives U24 directly.

## U43 — external, 0x21

INT is `CTRL_EXT_INT_N` → IO16, pulled up by R137 10 kΩ.

| Pin | Signal | Direction / meaning |
|---|---|---|
| P00 | `EXT_VBUS_PG` | U35 VBUS output status input |
| P01 | `EXT_5V_VBUS_PG` | U36 5 V VBUS output status input |
| P02 | `EXT_SS_PG` | U37 backed VBUS output status input |
| P03 | `EXT_5V_SS_PG` | U38 backed 5 V output status input |
| P04 | `JET_ON_FB` | Jetson-state input |
| P10 | `EXT_5V_VBUS_EN` | U36 5 V VBUS output enable |
| P11 | `EXT_VBUS_EN` | U35 VBUS output enable |
| P12 | `EXT_SS_EN` | U37 backed VBUS output enable |
| P13 | `EXT_5V_SS_EN` | U38 backed 5 V output enable |
| P14 | `JET_PWR_BTN_CTRL` | Jetson-button output |
| P05–P07/P15–P17 | unused | — |

## Reset behavior

Ports power up as inputs. An ESP32-only reset does not reset either expander because
`+3V3_SS` remains powered and there is no reset pin. Existing outputs—including
`SCC_EN`, the TS override, and external-port enables—can persist. This behavior is
accepted; firmware must read and deliberately reconcile both devices after reset.
A full rail power cycle returns ports to inputs, where the external pulldowns
establish default-low enables. See [issues.md](../issues.md).

Primary reference: [TCA9535 datasheet](https://www.ti.com/lit/ds/symlink/tca9535.pdf).
