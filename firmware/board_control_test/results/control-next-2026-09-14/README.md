# Tests 1 / 2 / 3 / 6 — 2026-09-14

Executed on ESP32-S3 `68:ee:8f:58:b0:10`, with USB main power, no external loads
and no supercap. The complete sequence began at **20:06:20 UTC / 16:06:20 Toronto**.

**Backup switching and both expander interrupt paths passed. PD communication
passed, but the requested power budget remains unresolved. D7 GPIO control and
visible LED operation both passed, with the user confirming the repeat run.** No ADC current channels
were read. The automatic VCAP meter hold was replaced by the normal off-at-boot
test build, and controlled outputs were left off.

## Backup switch and expander interrupts

The charger maintained unloaded VCAP with low-current selection and fixed TS
bypass. GPIO42/U24 was enabled and disabled **three times**. Each on command
changed U26 inputs `DC → FC` and each off command restored `FC → DC`, confirming
backup PG assertion/deassertion. Main selection remained high. The VCAP ADC stayed
within its screening window; this did not meter-measure `+VCAP_VALID` or perform
a source-loss transfer.

There were **18 successful PG/interrupt transitions**: six backup transitions,
four buck transitions, and eight transitions across J7–J10. Every transition
produced exactly one captured falling edge and one rising edge on the expected
expander INT pin, and none on the other pin. Both lines were released afterward.
J8's upstream buck was enabled before the port tests to separate its two PG events.

All three backup-off cases observed internal INT held low before an input read,
read the expected changed backup-PG bit, and observed INT high afterward. This
proves input-read acknowledgment as well as GPIO15 routing. The GPIO16 events
were captured even when the existing setters' register reads cleared them before
returning. ISR handlers contain no I2C or serial operations.

## PD result and current-request finding

Three complete, stable HPI snapshots reported:

| Field | Observed value |
|---|---|
| Device mode / silicon ID | `0x95` / `0x2004` |
| PD status / Type-C status | `0x000DA400` / `0x8B` |
| Active source PDO | `0x000641D6`: fixed **20 V, 4.7 A**, advertised maximum **94 W** |
| Active RDO | `0x4080000A`: object 4, **0 mA operating, 100 mA maximum**, mismatch clear |
| GPIO39 / HPI interrupt register | Low / `0x01` pending |
| Naturally observed PD interrupt edges | Zero during the read window |

The initial checker rejected zero operating current and returned an overall
`CONTROL_NEXT FAIL` despite 73 successful control checks. Review of the exact
CYPD3177 behavior established that zero agrees with the board's grounded ISNK
pins: [Infineon's CY4533 guide, section 2.4](https://www.infineon.com/assets/row/public/documents/24/44/infineon-cy4533-ez-pd-barrel-connector-replacement-evk-guide-usermanual-en.pdf)
specifies that their configured sum becomes the RDO operating current.

The final uploaded diagnostic distinguishes **COMMUNICATION PASS** from
**POWER_REQUEST UNVALIDATED**, with an overall **PARTIAL** PD result and a false
return value. The source's 94 W advertisement does not establish that allowance
for the board. The origin of the observed 100 mA maximum field is not explained
by the available documentation. Current-request configuration needs resolution
before loaded operation; no contract/configuration change was made in this test.

No PD event was generated or acknowledged. The low GPIO39 agrees with a pending
register bit, but fresh-event delivery and acknowledgment remain untested. Raw
version bytes are recorded without claiming a silicon firmware revision. See
[PD reference notes](../../PD-REFERENCES.md) for exact scope and documentation.

## LED and completion state

GPIO4 readback passed for three **1.5-second on / 1.5-second off** D7 pulses.
The user subsequently requested another `control-next` run, captured at
**20:16:20 UTC / 16:16:20 Toronto** in [manual-repeat.log](manual-repeat.log), and
confirmed that the status LED blinked as expected. D7 visual operation is now
**PASS**. The raw log's `visual_confirmation=PENDING` text predates the user's
confirmation and is preserved unchanged.

The full sequence ended with `OUT=00`, `EOUT=00`, GPIO42=0, GPIO4=0 and VCAP ADC
0.0000 V, main input selected. There were **73 passing control checks and zero
failed control checks**; the separate PD request check prevented an overall pass.

## Evidence

- [Complete hardware log](hardware.log): backup, both interrupt paths, PD and D7.
- [User-requested repeat](manual-repeat.log): the full sequence on the final
  uploaded firmware; another 73 control checks passed and D7 was visually confirmed.
- [Initial build](build.log), [verified upload](upload.log), [initial source/binary hashes](build-manifest-first.json).
- [Final PD reporting build](build-final.log), [final upload](upload-final.log), [PD retest](pd-final.log), [final status](final-status.log), [final hashes](build-manifest.json).
- [23 existing driver/hold host checks](host-tests.log), [7 interrupt host checks](irq-host-tests.log), [PD transport/decoder host checks](pd-host-tests.log).

The reporting update only changed `PdDiagnostics.h/.cpp`. Backup, IRQ and LED
sequence code is unchanged from the full hardware run. The final upload was
retested with `pd` and `status`, then the full sequence was repeated at the user's
request for the visual LED check.
