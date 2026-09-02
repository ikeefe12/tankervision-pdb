# USBC PD — USB-C Power Delivery sink front end

**Schematic label:** `USBC PD`

**Key parts:** J1, U8 CYPD3177-24LQXQT, U2 TVS2200, U3 TS5A3159,
U4 TLV7031, JP1

## Function and configuration

U8 is Infineon's CYPD3177 BCR sink controller. `VBUS_MIN` and `VBUS_MAX` are tied
to `+3V3_PDC`, so the autonomous configuration requests a specific 20 V PDO rather
than the former 12–20 V window. Before negotiation, raw `+VBUS_USB` can be the USB
default 5 V; U22 remains off until U4 validates U8's gate state.

- J1 VBUS is `+VBUS_USB`, protected by U2 TVS2200 and local C2/C3/C82.
- D4/D5 protect CC1/CC2; U8 supplies the sink terminations.
- U8's internal 3.3 V output is `+3V3_PDC`, present only with USB VBUS.
- `ISNK_COARSE` and `ISNK_FINE` are grounded. U8 accepts any advertised source
  current; firmware must read the PDO/RDO and enforce the board power budget.
- HPI address is 0x08 on `PD_SDA`/`PD_SCL`; `PD_INT` reaches ESP32 IO39.
- FAULT drives Q7/D6. D+/D−, GPIO1, and SAFE_PWR_EN are unused.

The controller negotiates voltage autonomously but does not limit total downstream
load current. A 20 V contract can still provide less power than the high-current
charger plus enabled outputs require; 5 A operation also requires the appropriate
e-marked cable.

## Contract-gate conversion

U8 `VBUS_FET_EN` is a PFET gate driver: near VBUS while off and near GND after an
acceptable contract. U4 converts that polarity:

- IN+ = `+VBUS_USB` × 10/(200+10) via R2/R7 = VBUS/21;
- IN− = `VBUS_FET_EN` × 10/(100+10) via R21/R14 = gate/11;
- no contract: IN− > IN+, U4/R8 hold U22 off;
- valid contract: IN− ≈ 0, U4 asserts `VBUS_USB_EN` and U22 ramps `+VBUS`.

R8 49.9 kΩ is the independent output pulldown. Divider logic, common-mode range,
and failure behavior are detailed in [comparators.md](comparators.md).

The architecture accepts that this path is not single-fault tolerant: a stuck-high
U4, open R21, or shorted R14 could enable U22 early. Bench-test attach, detach,
reset, cable reversal, and incompatible sources.

## HPI power-domain isolation

U3 gates the top of R26 10 kΩ (`PD_INT`) and R27/R28 2.2 kΩ
(`PD_SDA`/`PD_SCL`). R11 10 kΩ from `+3V3_PDC` and R16 100 kΩ to GND select:

- `+3V3_SS` pull-ups while both domains are powered;
- grounded pulls while only the backed rail is present;
- powered-off high impedance while `+3V3_SS` is absent.

This prevents either power domain from back-powering the other through the HPI
signals and requires no firmware-controlled bus enable.

## `VDC_OUT` configuration jumper

JP1 is a default-closed solder jumper between U8 `VDC_OUT` and shared `+VBUS`.

- **USB-C operation:** leave JP1 closed so `VDC_OUT` monitors the system side of
  U22 as intended.
- **DC operation:** open JP1 before applying J6 power so DC cannot energize
  `VDC_OUT` while U8 is unpowered.

USB-C and DC power must not be connected simultaneously. JP1 is a deliberate user
configuration control, not automatic source protection.

Primary reference: [CYPD3177 datasheet](https://www.infineon.com/assets/row/public/documents/24/49/infineon-cypd3177-24lqxq-datasheet-en.pdf).
