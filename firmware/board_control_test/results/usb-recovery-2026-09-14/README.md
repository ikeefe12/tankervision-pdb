# USB recovery diagnostic result — 2026-09-14

This run investigates application serial silence after the existing 20-second
backup cutoff and main-power restoration. See the
[physical procedure](../../USB-RECOVERY-TEST.md). The diagnostic does not add an
automatic USB repair. The physical run recovered application serial after the
delayed host reopen, with J2 left connected. The user confirmed that D7 returned
to blinking. The earlier persistent serial silence did not recur in this run.

The diagnostic firmware built with Arduino ESP32 core 3.3.3 and uploaded with
esptool hash verification to ESP32 MAC `68:ee:8f:58:b0:10`. All 22 sketch source
files match the copied build sources; `firmware-sha256.json` records the source
and binary hashes. The existing 35 supervisor scenarios and heartbeat checks
pass, as do 12 host-capture tests covering reconnection/error handling.

The first persistent host open produced a USB-induced reset and application
session **74A8F4FD**. Baseline initialization passed with main and backed supply
PG valid, controlled outputs off and GPIO42 low. D7 transitioned every 500–501 ms
outside synchronous startup operations. `status` and `usb-status` both returned.
The diagnostic reported USB RX events and matching consumed bytes/completed
command lines, a valid receive queue, and a connected CDC gate.

Both retention paths were verified before charging: the report retrieved the
preceding session **F85AFC35** from the checked RTC journal and its independent
NVS snapshot. That preceding boot occurred after upload, before the host capture
opened. Its lack of received commands is expected and is **not** a reproduction
of the main-power recovery failure. The new session also successfully stored its
10-second snapshot at uptime 10.011 seconds. These records demonstrate that the
logger can retrieve evidence from a boot preceding a host-induced USB reset.

An explicit `transfer-start` negotiated and verified **20 V / 2000 mA** from the
source, with PDO `0x000641D6`, RDO `0x408320C8` stable for 506 ms. Charging uses the
real temperature input (about 1895 mV), override off and the low current profile.
Backup armed with PG high; the exterior switches remain off. VCAP reached about
20.45 V ADC, consistent with the previously meter-confirmed 20.67 V endpoint;
this run does not introduce ADC calibration or a new meter measurement.

The persistent `live.log` records the physical sequence and remains open. Device
events, open attempts, byte counts, firmware sessions and diagnostic records
share UTC/host-monotonic timestamps. After device disappearance and reappearance,
the host waits 25 seconds and attempts one reopen. A read error or silence alone
never causes a reopen. The expected post-restoration observation window is 60
seconds with J2 untouched. A later J2 replug, if required, is a separately recorded
intervention. No current-monitor ADC channels are read.

Readiness completed: `confirm_VCAP_near_20.63V_before_removing_J1; keep_J2_connected session=74A8F4FD t=68936 VCAP_ADC=20.4516 stable_plateau_s=30 backup_armed=1`. The stable charging/armed baseline is frozen in `baseline.log` and `baseline.json`; the physical power-cycle capture continues in `live.log`.

## Physical result

| Observation | Evidence |
|---|---|
| Main loss detected | Session `74A8F4FD`, uptime 135.783 s; backup confirmed at 135.861 s. |
| Powered throughout the grace period | All 20 captured backup heartbeats had MAIN/USB low, CAP/SS PG high and GPIO42 high. No missing heartbeat sequence numbers or new application session before intentional cutoff. |
| Intentional timeout | Cutoff marker at uptime 155.788 s, **20.005 s** after detected loss; VCAP **20.3596 V ADC**. |
| Power removed | Brownout message approximately 0.257 s after the intentional cutoff marker, followed by device disappearance. This is expected decay after the deliberate switch release. |
| Main restoration and natural boot | USB device returned at 22:20:35.364 UTC. Retained records identify a separate **POWERON** session `107DCBBD`, with a valid RTC record at uptime 25.274 s and an independent NVS snapshot near 10 s. |
| Host reconnect | Host reopened once at 22:21:00.376 UTC, **25.012 s** after device arrival. The open caused a USB-induced ESP32 reset into session `472F46E9`. |
| Application serial recovered | Application BOOT arrived about 0.287 s after OPEN_OK. Both scheduled `status`/`usb-status` probe pairs returned, with matching received and parsed command bytes. No physical J2 replug or automatic firmware USB repair was needed. |
| Default state restored | Session `472F46E9` remained running for at least 60 s with valid main/SS PG, `OUT=0x00`, `EOUT=0x00`, `EN42=0` and the heartbeat running. VCAP was about **20.27 V ADC**; charging remained off. |

Before the host reopened, natural-boot session `107DCBBD` detected USB SOF at
uptime 258 ms but recorded `cdc_gate=0`, zero RX and zero TX events. The host had
deliberately kept its serial handle closed during those 25 seconds, so these
values are expected; they do not demonstrate a stuck connection. The retained
natural-boot record was successfully recovered after the separate USB reset.

This establishes successful power-off/restart and serial recovery under this
diagnostic procedure. It does **not** establish serial recovery in the original
natural-boot session, because the host open reset that session. It also does not
identify why the earlier attempt stayed silent or prove that a 25-second delay
alone fixes it: both instrumentation and host reconnect timing differ. Repeated
controlled comparisons would be required to establish reliability and isolate
the cause. The frozen physical capture is `completed.log`; timings and terminal
status are summarized in `analysis.json`.
