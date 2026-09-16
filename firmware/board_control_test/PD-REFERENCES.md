# CYPD3177 diagnostics references and scope

`PdDiagnostics` owns the separate `TwoWire(1)` interface on GPIO40/41 at 100 kHz
with a 25 ms transfer timeout. GPIO39 is sampled and CHANGE interrupts are counted.
`begin()`, `readSnapshot()`, and the `pd` diagnostic send only two-byte register
pointers followed by reads. The separate, explicit `requestFixed20VCurrent()`
method also writes a sink profile as documented below. The original complete HPI
specification is currently behind Infineon sign-in; no CYPD3176/3178-only command
map has been substituted.

Read addresses and basic status interpretation were checked against the
Infineon-authored **EZ-PD BCR HPI Utility user guide**, document 002-29388 Rev. *B,
sections 2.4 and 2.5.4, which explicitly supports CYPD3177. Its register list
includes identity, READ_ALL_VERSION, PD_STATUS, TYPE_C_STATUS and active PDO/RDO.
Only documented attached/polarity/data-role/power-role/partner-revision fields
are decoded from status. PD_STATUS remains visible raw; this test does not
claim a separately verified established-contract status bit. The source ceiling
comes from the selected fixed PDO and is not a measurement of consumption.
[Infineon document mirror](https://manuals.plus/m/40338896f11310847c62d43000624e4a13715d2edc77eb60a61c0f73def52b27.pdf).

Infineon confirms that device mode/silicon ID documentation has discrepancies,
and reiterates CURRENT_PDO at 0x1010 and CURRENT_RDO at 0x1014. Production values
0x95/0x2004 are used for this fixture. TYPE_C_STATUS is read as one byte. The
disputed EVENT_STATUS location is not accessed.
[CYPD3177/CY4533 support discussion](https://community.infineon.com/t5/EZ-PD-USB-Type-C/CY4533-%E3%81%AE%E8%B3%AA%E5%95%8F/td-p/1193497),
[identity read confirmation](https://community.infineon.com/t5/EZ-PD-USB-Type-C/CYPD3177-I2C-communication-no-response/td-p/909323).

READ_ALL_VERSION is preserved as raw bytes only. Infineon support says CYPD3177's
actual firmware revisions cannot be distinguished through an HPI register; the
device date code is required. Raw bytes therefore do not establish 3.0C versus
4.05 firmware.
[CYPD3177 firmware-version limitation](https://community.infineon.com/t5/EZ-PD-USB-Type-C/CYPD3177-Firmware-Version-via-HPI/td-p/816328).

The original read-only diagnostic generates or acknowledges no PD events. Exact acknowledgment
semantics were not available from a verified CYPD3177 specification, and changing
the source contract could interrupt this USB-powered test. A low GPIO39 matching
a pending INTERRUPT register is evidence of a connected level path; it does not
prove event generation, ISR handling of a fresh falling edge, or acknowledgment
and release. Naturally observed edges are reported separately.

## Grounded ISNK pins and observed RDO

The 2026-09-14 hardware run returned stable PDO `0x000641D6` (20 V, source
advertises 4.7 A) and RDO `0x4080000A` (object 4, operating-current field 0 mA,
maximum-current field 100 mA). The zero operating field is consistent with the
board configuration: both ISNK pins are grounded. Tables 3/4 assign zero to each
grounded pin in the
[CYPD3177 datasheet](https://www.infineon.com/assets/row/public/documents/24/49/infineon-cypd3177-24lqxq-datasheet-en.pdf).

The CYPD3177-specific capability algorithm in section 2.4 of the
[CY4533 guide](https://www.infineon.com/assets/row/public/documents/24/44/infineon-cy4533-ez-pd-barrel-connector-replacement-evk-guide-usermanual-en.pdf)
first compares the source's advertised current against the configured sum, then
places that configured sum in the RDO operating-current field. It does not
automatically request the source's maximum current. The reason for the reported
100 mA maximum field has not been established from available documentation.

Therefore the reads establish working ESP32 communication and selected 20 V
data, but the 94 W source advertisement is not a validated board power budget.
Infineon also states that increasing the requested current requires changed
ISNK settings or host control through HPI.
[Infineon current-negotiation explanation](https://community.infineon.com/t5/EZ-PD-USB-Type-C/Current-Negotiation-for-CYPD3177/td-p/917678).
No current-request modification was attempted in that read-only test.

## Volatile current request for the connected-supercapacitor test

`requestFixed20VCurrent(milliamps, log, accepted)` adds a bounded, explicit control
operation. This must run with downstream converters, charger, and ports disabled,
because renegotiation can interrupt input power. It first verifies an attached
20 V source advertising enough current, then uses two sink PDOs: 5 V/900 mA with
the Higher Capability flag and fixed 20 V at the requested current. The callable
API limits requests to 100–3000 mA in 10 mA increments.

The command sequence was checked against the **CYPD3177** demo in the
Infineon-authored HPI Utility guide, 002-29388 Rev. *B, section 2.6, **Figure 26 on
page 21**. The figure explicitly shows little-endian pointers `00 18` for profile
data, `05 10` for the sink mask, and `00 14` for the four-byte response read.
The signature is bytes `50 4B 4E 53`, followed by PDOs in little-endian order;
mask `03` selects the first two PDOs. The guide says this configuration is volatile.
[Guide, section 2.6](https://manuals.plus/m/40338896f11310847c62d43000624e4a13715d2edc77eb60a61c0f73def52b27.pdf),
[Figure 26 mirror](https://www.manualslib.com/manual/2866120/Infineon-Ez-Pd.html?page=21).

Infineon's CYPD3177-specific command example independently documents the data
memory address, signature, zero-filling unused PDOs, renegotiation after selecting
the sink mask, and `PD_RESPONSE` success code `02`.
[Infineon CYPD3177 PDO-change example](https://community.infineon.com/t5/Knowledge-Base-Articles/Change-the-PDO-of-CYPD3177-through-I2C-with-a-MiniProg3/ta-p/246422).

Success requires a success response and matching **active** PDO/RDO data stable
for 500 ms within five seconds. The original source PDO, its 20 V selection, and
source object position must remain the same; operating current must meet the
request, with valid maximum-current bounds and no capability mismatch/GiveBack.
A stale success response with unchanged insufficient RDO times out. An already
adequate active contract is checked for stability without configuration writes.
The caller must also recheck board input PG before enabling its load.

The method reports GPIO39 and interrupt status but does not acknowledge them;
interrupt acknowledgment/release is still unverified. This is a configuration and
active-contract readback check, not a USB-PD protocol-analyzer capture or actual
input-current measurement. Host mock tests cover the exact payload, mask, stable
readback, stale-response timeout, insufficient-source rejection and I2C failure.
