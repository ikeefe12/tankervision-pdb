# 20-second main-loss shutdown and MCU heartbeat

Requested behavior: wait 20 seconds for main power to return after its loss;
otherwise deassert the supercap switch, leaving the board unpowered and the bank
partly charged. D7 now indicates MCU main-loop activity instead of VCAP >10 V.

`SupercapTransfer` owns the grace-period policy and reports a terminal cutoff
request. `TransferApplication` records the reason and voltage, commits the NVS
checkpoint, then calls `BoardControl::releaseBackupForShutdown()` to lower GPIO42.
Main return cancels the timer; each later loss starts a new timer. The existing
7 V low-energy cutoff remains an earlier fallback. Restore main power for a new
boot with charger, backup and exterior enables off; charging needs an explicit
`transfer-start`.

`StatusHeartbeat` toggles D7 every 500 ms, independent of voltage and charging
state. It runs from the application loop, with no delay or autonomous timer/PWM.
It does not replay missed toggles after a blocking operation.

Validation: 35 supervisor scenarios and focused heartbeat checks pass. Cases
include the 19,999/20,000 ms boundary, main return at the boundary, repeated losses,
counter rollover, known-loss timeout despite I2C/cleanup failures, fault/stopped
states, earlier low-voltage cutoff, and heartbeat timing/stop/fault recovery.
The supervisor tests fail on any attempted LED write. See the captured build and
test logs.

The firmware built cleanly and uploaded with esptool hash verification. Source
and binary SHA-256 values are in `firmware-sha256.json`. The uploaded board booted
in session `351DAA24` with controlled outputs off. The persistent capture verified
D7 high/low transitions every 500–501 ms while stopped, then while charging.
The explicit start verified the existing 20 V/2 A PD request stable for 527 ms,
used the real temperature input, and armed backup with PG high. VCAP stabilized
near 20.45 V ADC. The startup operation briefly delays heartbeat toggles because
the heartbeat intentionally follows loop execution.

The physical test passed both requested branches in session `351DAA24`:

| Check | Observed result |
|---|---|
| Main return cancels shutdown | Loss at uptime 33.406 s; main restored at 40.106 s, cancelling after 6.700 s of detected absence (6.625 s confirmed backup). Charging stayed off. |
| Next loss starts a fresh timer | Second loss at uptime 53.429 s; timeout marker at 73.432 s: **20.003 seconds**. |
| Charge retained at cutoff | Last valid VCAP was **20.3136 V ADC** immediately before GPIO42 release. |
| Intentional power loss | The ESP32 emitted a brownout message approximately 0.31 s after the shutdown marker as its supply fell, then USB disappeared. The logger recorded `EXPECTED_POWER_OFF`. This followed deliberate power removal, not a failure during the grace period. |
| Heartbeat | 143 verified D7 transitions; it continued throughout both backup intervals. |
| Continuity before cutoff | One session, no missing heartbeat sequence numbers; backup and backed 5 V PG remained valid while waiting. |

After main restoration the user confirmed D7 blinking, showing that initialization
and the application loop resumed. The initial host serial read showed only the
ROM banner; reconnecting data-only J2 recovered serial. The final status read in
session `6BB58936` verified **prior phase `MAIN_TIMEOUT_20S`**, previous session
`351DAA24`, and checkpoint uptime 73.432 s. Initialization passed, `OUT=0x00`,
`EOUT=0x00`, `EN42=0`, `MAIN=1`, `USB=1`, `SS=1`, and VCAP remained **19.964 V ADC**.
D7 continued its heartbeat while the charger and all controlled switches stayed
off. Charging was not restarted, and the read-only capture was closed with main
power present. See `after-data-reconnect.log` for this final default-state check.
See `live.log` and
`analysis.json` for timing and `after-power-return.log` for that initial reconnect
attempt. No current-monitor ADC channels were read.
