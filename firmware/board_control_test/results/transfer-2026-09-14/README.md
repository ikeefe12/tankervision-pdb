# Real-bank charging and backup transfer — 2026-09-14

Bank rated 21.6 V, real temperature resistor connected, no
external loads. The user authorized charging to the board's 20.629 V target.
Main input is J1 USB-C; data-only J2 stays connected to one persistent logger.

The dedicated `build-transfer.sh` firmware uploaded successfully; esptool verified
all written hashes. Build uses Arduino ESP32 3.3.3, N8R8 board options and enabled
brownout detection. Source/build hashes are in `firmware-sha256.json`.

Before upload, VCAP was 0.000 V ADC and real TS was 1935 mV, with main USB and
backed 5 V PG valid. All controlled enables were off.

Charging began at uptime 5.619 s in session `0B958541`, with low ISET and real TS
(`OUT=0x03`: boost and charger CE only). No current ADC channels were read.

| Check | Observed evidence |
|---|---|
| USB-PD power request | Source PDO remained `0x000641D6` (20 V/4.7 A advertised). Active RDO changed from `0x4080000A` to `0x408320C8` (2 A operating/maximum), HPI success `0x02`, stable for 503 ms before charging. |
| D7 threshold | At uptime 58.908 s, raw VCAP 10.0004 V, GPIO4/D7 read high. |
| Backup arming | GPIO42 high at the same threshold; next status frame at 58.917 s showed backup PG high, while main selection remained high. |
| Software verification | 30 driver/hold scenarios, 22 transfer-supervisor scenarios, and PD transport/decoder/request checks passed; firmware build clean. |
| Charge endpoint | Ready plateau at uptime 156.631 s, ADC 20.4608 V. User confirmed **20.67 V** by meter. |
| Main-loss detection | Loss latched at uptime 187.419 s; GPIO11 edge activity, MAIN=0, USB=0, DC=0, backup PG=1, backed 5 V PG=1. |
| Charger shutdown on loss | First post-cleanup frame showed CE low (`OUT=0x01`) at 187.494 s; boost also low (`OUT=0x00`) at 187.613 s. Backup GPIO42 remained high. These are observed status-frame times, not oscilloscope switching times. |
| ESP32 continuity | 21 successive backup heartbeats, same session `0B958541`, no reset/USB loss or fault. Each had MAIN=0, USB=0, CAP=1, SS=1, EN42=1. Maximum heartbeat spacing was 1001 ms. |
| First return | At uptime 208.215 s, MAIN and USB PG returned high; elapsed confirmed backup selection was **20.790 s**. Charging stayed off. |
| Second backup interval | A second input loss produced 49 consecutive heartbeats with MAIN=0, USB=0, DC=0, CAP=1, SS=1 and EN42=1, spanning **48.046 s** (uptime 427.937–475.983 s). Same session and continuous heartbeat sequence, no reset or brownout detected. VCAP fell from 19.2556 to 18.9796 V ADC. |
| Second return | GPIO11 rose at uptime 476.091 s. USB PG and main selection recovered; charger PG and real TS subsequently also confirmed a powered input domain. Charging remained off. |

The first transfer returned before the automatic 30-second marker. The second
lasted longer than 30 seconds, independently verified from raw PG/enables and
continuous uptime. It exposed a software reporting bug: the one-shot state
machine retained `RETURNED` and the first interval's duration during a second
loss. The raw signals and cutoff supervision remained active. The repeated-loss
fix now resets episode confirmation/timing and reporting on each loss; **25
supervisor regression scenarios pass**, including repeated losses after return,
stopped operation and faults. `initial-source.zip` exactly matches the source
hashes for the observed run; subsequent fix build/upload evidence is separate.

GPIO11 counted three edges during each of the first three source transitions and
one on the second return. This is edge-count evidence, not a measured mux waveform
or transfer-droop characterization.

The largest supervisor service gap was 165 ms during initial backup arming, which
uses a bounded PG-stability wait. Steady charging sampling was approximately
20 ms. No current-monitor ADC reads or external-load tests were performed.

The reporting fix was built cleanly and uploaded with esptool hash verification
after main power was stable and backup had been deliberately disarmed. The new
boot reported the prior NVS `MAIN_RESTORED` checkpoint and started with outputs
off. A new explicit run in session `3D580E9B` revalidated the volatile PD request:
the reconnected controller had reverted to RDO `0x4080000A`, and the command
restored `0x408320C8`, stable for 505 ms. Real-TS charging and backup arming passed
again. See `live-repeat-fix.log` and `firmware-repeat-fix-sha256.json`. Repeated
transfer reporting in this updated build is covered by host regression tests;
the two physical transfer intervals above were captured on the initial build.

`live.log` is the continuous capture; `pre-upload.log`, `upload.log`, build/test
logs, and extracted `voltage.csv` retain supporting evidence. Host arrival times
can bunch USB packets: use MCU uptime for intervals and heartbeat continuity.

The VCAP ADC/divider path remains uncalibrated. Earlier unloaded operation measured
20.67 V by meter with lower ADC readings. A voltage plateau alone does not prove
current taper or bank capacity. Same-session heartbeat/PG evidence can establish
observed ESP32 continuity; this test does not measure rail droop, ripple, charge
current, or loaded transfer performance.

Final state after the update: recharged to a stable **20.46 V ADC** plateau,
real TS selected, low ISET, D7 on, backup armed with PG high, main selected, all
external ports off. Charging and the persistent serial monitor remain active.
The new boot is an intentional USB/upload reset after the completed transfer
tests, not a reset during either backup interval.
