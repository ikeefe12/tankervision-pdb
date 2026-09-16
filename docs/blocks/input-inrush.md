# USBC INRUSH CTRL and DC INRUSH CTRL

**Key parts:** U22/U23 LM73100, U2/U20 TVS2200, J1/J6

The two [LM73100 switches](common-lm73100-power-switch.md) join at `+VBUS`. Their
back-to-back FETs block reverse current, forming an ideal-diode OR without feeding
one connector from the other.

## USB path — U22

- `+VBUS_USB` → U22 → `+VBUS`.
- `VBUS_USB_EN` from U4 controls EN; R8 holds it low if U4 is open/unpowered.
- R86/R102 set 22.84/20.75 V OVLO; R82/R91 set 17.07/15.51 V PGTH.
- C95 4.7 nF controls output slew.
- IMON uses R122 and reaches IO9 as `VIMON_USB`; U27 produces `PG_USB` for U26.P02
  and Q12/D22.

The separate USB-PD document covers the CYPD3177 gate logic and JP1 configuration.

## DC path — U23

- The permitted J6 adapter range is **18.5–20 V**. `+VBUS_DC` has C83 1 µF and
  U20 TVS2200.
- R83/R92/R103 form the shared UVLO/OVLO ladder: 17.50/15.90 V UVLO and
  22.14/20.11 V OVLO. These thresholds provide guard bands around the adapter
  requirement; they are not exact 18.5 V and 20 V cutoffs.
- R84/R93 set 17.07/15.51 V PGTH; C98 4.7 nF controls output slew.
- IMON uses R124 and reaches IO3 as `VIMON_DC`; U28 produces `PG_DC` for U26.P00
  and Q13/D23.

The LM73100 handles reverse-polarity input without a series diode. Hot-plug overshoot
still depends on the supply/cable inductance and capacitor ESR and must be tested at
J6/U20/U23.

The conditioned PG inputs indicate that the corresponding input switch is on and
its PG threshold is met. The 100 kΩ pull-ups and comparator thresholds are sized
to read low for an absent input; see
[comparators.md](comparators.md#input-pg-unpowered-state).
