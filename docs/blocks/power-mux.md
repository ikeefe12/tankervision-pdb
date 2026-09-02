# POWER MUX — TPS2121 VBUS/supercap priority mux

**Schematic labels:** `POWER MUX`, `2V5 VCAP VREF`

**Key parts:** U19 TPS2121, U30 TLV7031, U25 TS5A3159, U51 LM4040A25,
R72/R76/R74/R80/R89/R100/R97/R197

## Function and selection logic

U19 IN1 is `+VBUS`, IN2 is armed `+VCAP_VALID`, and OUT is backed `+VBUS_SS`.
The part operates in XREF priority mode:

- PR1 = VBUS × R76/(R72+R76) = VBUS × 5.62/(34+5.62).
- CP2 is U51's 2.5 V reference, biased from `+VCAP_VALID` through R197 10 kΩ.
- IN1 is selected while PR1 exceeds CP2: **17.625 V nominal**.
- Initial R72/R76/U51 tolerance gives approximately **17.577–17.673 V** for the
  falling IN1→IN2 crossover.
- U19's 5–40 mV comparator offset gives an expected IN2→IN1 return threshold of
  approximately **17.66–17.91 V**.

This crossover is intentionally below every valid 18.5–20 V DC input and the
negotiated 20 V USB input. VBUS is expected to cross it decisively during failure
or restoration, so no extra slow-crossover hysteresis network is fitted.

## Input overvoltage settings

OV1 uses R74/R80 and OV2 uses R89/R100. Both are 102 kΩ/4.99 kΩ, ±0.1%,
±25 ppm/°C:

- nominal rising threshold: **22.73 V**;
- nominal falling threshold: **22.30 V**;
- initial full TPS2121 reference/resistor/leakage range: approximately
  **21.60–23.64 V**;
- conservative full-temperature estimate with opposing resistor TC:
  approximately **21.50–23.75 V**.

The low corner remains above the 20.94 V high estimate for the charged bank. These
OV comparators turn off a mux channel but do not remove excessive voltage from the
associated input pin. Source limits and U22/U23/U24 must therefore keep U19 inputs
below the 24 V absolute maximum; the system does not rely on U19 to protect itself.

R97 = 22 kΩ sets a characterized current-limit range of approximately 4–5 A,
4.5 A typical. C93 = 1 µF configures soft start/input settling.

U51 is the industrial-temperature LM4040A25I grade (−40 to +85 °C). Confirm that
+85 °C covers the product ambient requirement.

## Status and indication

U19 ST is pulled up to `+3V3_SS` and conditioned by U30:

- `PMUX_ST` low: IN2/supercap selected;
- `PMUX_ST` high: IN1 selected **or the output is Hi-Z**.

R44 5.1 kΩ feeds ESP32 IO11 and R49 49.9 kΩ provides a local pulldown. U25
steers `+3V3_SS` to D29 (red, supercap/IN2) when low and D25 (green, main/IN1)
when high. The green indication shares the same IN1-versus-Hi-Z ambiguity as ST.

## Output capacitance and transfer behavior

Nominal `+VBUS_SS` capacitance is approximately **241.1 µF**:

- C78/C79: 2 × 100 µF;
- C80/C81: 2 × 10 µF;
- C103: 1 µF;
- U1 input C5/C6/C7: 20.1 µF.

At 40 W and the 17.625 V crossover, load current is about 2.27 A. A 5 µs
break-before-make interval costs only about 47 mV on the nominal capacitance.

When switching from a lower to a higher voltage, TPS2121 intentionally uses its
active current limiter. A nominal 17.625→20.629 V transfer adds approximately
13.8 mJ and 0.72 mC to the capacitors. With a 40 W constant-power load, expect
roughly a few hundred microseconds of current-limited settling; this is not an
anticipated OCP event. The accepted concern is only prolonged/repeated current
limiting from a low bank, excessive load, source droop, or selection chatter.

The backup path includes both U24 and U19. At 2 A, using 28 mΩ and 56 mΩ typical
resistance respectively, the static path drop is approximately 0.17 V.

The completed PCB keeps U19 and its local capacitors compact and routes the three
power rails with wide copper zones/trunks. This passes static layout review; scope
the output during the transfer cases below because DRC cannot validate the dynamic
current-limit interval or source stability.

## Firmware and validation

- Define a load-aware minimum bank voltage. A 40 W load approaches the minimum
  4 A current limit at a 10 V bank before converter losses are included.
- Test return at full, half-full, and minimum allowed VCAP. Input return to a low
  bank creates a larger upward step than the nominal full-bank crossover.
- Validate failover/return at zero and maximum load, min/max capacitance and ESR,
  temperature, slow input ramps, and repeated source interruptions.
- Do not use `PMUX_ST` alone as proof of valid VBUS.
- Verify direct IO42 `VCAP_EN` remains asserted during intended backup and releases
  the path on the firmware shutdown threshold or an ESP32 brownout.

Primary references: [TPS2121 datasheet](https://www.ti.com/lit/ds/symlink/tps2121.pdf)
and [LM4040 datasheet](https://www.ti.com/lit/ds/symlink/lm4040-n.pdf).
