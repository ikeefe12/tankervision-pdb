# Tankervision deployment USB API v1

The ESP32 owns charging, backup power, the Jetson supply, and the shutdown deadline. The Jetson reads board telemetry, acknowledges pings and shutdown requests, and controls the three optional external outputs. USB disconnects do not suspend the ESP32 power supervisor.

After the initial ten-second settling interval, firmware verifies its input budget and prepares charging through the real temperature-sense path. A healthy charger remains enabled after VCAP passes 20 V so it can maintain the bank while the Jetson runs. A charger fault or shutdown stops charging; fault reporting remains visible in telemetry. Maintenance charging does not change the fixed shutdown deadline.

The USB-PD request is fixed 20 V at **3 A** (60 W at the input), with a 5 V/900 mA fallback PDO. Firmware requires the verified 20 V contract before authorizing its loads. Budget the Jetson, approximately 23 W during supercap replenishment, optional external outputs, and conversion losses together within the input allowance. This setting is not a claim that every Jetson model or operating mode fits that budget. DC input uses a configured installation allowance rather than negotiation.

## Transport and framing

Use J2's deployment TinyUSB CDC endpoint at 115200 baud. Assert DTR and RTS and leave them stable. Deployment firmware disables CDC reboot hooks **before** starting USB; see [the exact Arduino 3.3.3 transport configuration](jetson/USB-TRANSPORT.md). This API is not supported by the earlier board-control test sketch.

Each message is one UTF-8 JSON object followed by LF. Host requests are limited to 512 bytes excluding LF. Device messages are limited to 8192 bytes. An oversized or invalid line is discarded through LF; a subsequent valid line starts a new frame. Do not send shell commands, human-readable serial commands, or binary framing.

Requests use a strict flat schema: unescaped ASCII field names and string values, unsigned decimal integer IDs, and JSON booleans. Duplicate or unknown fields, nested values, floating-point IDs, negative IDs, leading-zero numbers, and trailing data are rejected. Field order and ordinary JSON whitespace are insignificant. The supplied client emits the accepted subset directly.

Every device message has these fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `v` | integer | Protocol version, `1`. |
| `type` | string | `telemetry`, `event`, or `response`. |
| `boot_id` | string | Eight uppercase hexadecimal digits identifying this firmware boot. |
| `seq` | uint32 | Message sequence within this boot; may wrap. Dropped telemetry can create gaps. |
| `uptime_ms` | uint32 | ESP32 milliseconds since boot; wraps after about 49.7 days. |

Treat a changed `boot_id` as a new board session. Discard pending operations and shutdown work tied to the previous boot. Use `remaining_ms` to schedule host work; `deadline_ms` is in the ESP32 uptime domain and is not a host wall-clock timestamp.

## Telemetry and status

The ESP32 emits telemetry approximately once per second when USB can accept it. A `status` request also requests an immediate snapshot. Telemetry includes `state`, `readings`, `ports`, and `shutdown`, together with the common envelope.

`state` is one of `SETTLING`, `CHARGE_PREPARE`, `CHARGING`, `JETSON_STARTING`, `RUNNING`, `SHUTDOWN_WAIT`, or `POWER_OFF`. These report the power-management phase, not a guarantee that every requested hardware operation succeeded. Inspect telemetry validity, issues, controls, and power-good readings as well.

`readings` contains board measurements and digital status. Raw ADC counts and millivolts are observations; nominal current conversions are uncalibrated. Voltage and PG checks govern this firmware's power policy. Current estimates do not establish load-test validation.

| Telemetry field | Meaning |
| --- | --- |
| `reset_reason` | Numeric ESP-IDF reset reason for this boot. |
| `settling_remaining_ms` | Remaining initial 10-second settling interval. |
| `action` | Pending power-policy action name, or `NONE`. |
| `hardware_operation` | Board adapter operation enum: 0 none, 1 reconcile, 2 prepare charge, 3 enable charge, 4 disable charging, 5 port change, 6 restore POR. |
| `issue_bits`, `issues` | Latched policy issue bitmask and corresponding string names. |
| `last_error` | Most recently recorded adapter error, or an empty string. |
| `backup_armed` | Policy's successful backup-arm result; inspect `signals.VCAP_EN` and `signals.VCAP_PG` for physical evidence. |
| `backup_low_voltage` | VCAP is below the 10 V arming threshold. |
| `source_budget_valid`, `source_budget_ma` | Accepted input budget and its current allowance. A DC budget is an installation configuration, not a negotiated or measured current. |
| `charger.enabled_expected`, `charger.maintenance` | Policy expects charging to remain enabled; maintenance becomes true after successful initial qualification and ends on a charger fault or shutdown. |
| `jetson.port_enabled` | Sensed J9 enable-pin level; inspect J9 power-good separately. |
| `jetson.responsive`, `jetson.pong_seen`, `jetson.last_pong_age_ms` | Current pong freshness, whether any pong has been seen, and age or `null`. |
| `jetson.ping_id`, `jetson.on_feedback` | Current ping identifier and physical Jetson-on feedback. |
| `telemetry.generated`, `telemetry.late_periods` | Generated snapshots and missed nominal one-second periods. |
| `telemetry.usb_connected`, `telemetry.queued` | CDC connection state and queued frames. |
| `telemetry.dropped_messages`, `telemetry.dropped_telemetry`, `telemetry.max_loop_gap_ms` | Transmission/backpressure counters and largest observed main-loop gap. |

The `readings` object has these children:

| Field | Meaning |
| --- | --- |
| `valid`, `age_ms` | Both expander reads succeeded, and snapshot age. Check individual expander validity too; analog/GPIO samples are still collected on an I2C failure. |
| `vcap_v`, `ts_mv` | Divider-derived raw VCAP rail voltage and charger-temperature ADC voltage. VCAP currently uses `analog.VCAP.mv * 0.0092`; no meter correction is applied. |
| `analog` | `TS`, `VCAP`, `IMON_DC`, `IMON_J8`, `IMON_J7`, `IMON_J9`, `IMON_J10`, `IMON_USB`, and `IMON_BACKUP`. Each has `gpio`, averaged 12-bit `raw`, averaged `mv`, and nominal `estimated_a`; current is `null` for TS and VCAP. |
| `signals` | Named physical digital levels listed below. `true` means electrically high; active-low interrupt names must be interpreted accordingly. |
| `expanders.internal`, `expanders.external` | `valid`, `age_ms`, two-byte arrays `input`, `output`, `polarity`, `config`, and the uint16 `physical_inputs` value. Physical inputs undo the polarity register inversion. |
| `edges` | Counts `internal_fall`, `internal_rise`, `external_fall`, `external_rise`, `mux_fall`, `mux_rise`, `pd_fall`, and `pd_rise`. These count interrupt/GPIO transitions, not individual expander pin events. |
| `pd` | CYPD3177 status with `valid`, `age_ms`, `device_mode`, `silicon_id`, `status`, `type_c_status`, `pdo`, `rdo`, `response`, `interrupt_status`, `attached`, `millivolts`, `source_ma`, `operating_ma`, `maximum_ma`, and raw 16-byte `versions`. A stale or invalid PD snapshot must not be treated as a new active contract. |

`signals` keys are:

```text
PG_USB PG_DC VCAP_PG SCC_PG SCC_STAT 24V_VBUS_PG 5V_VBUS_PG 5V_SS_PG
EXT_VBUS_PG EXT_5V_VBUS_PG EXT_SS_PG EXT_5V_SS_PG JET_ON_FB PMUX_ST
VCAP_EN STAT_LED CTRL_IN_INT_N CTRL_EXT_INT_N PD_INT
24V_VBUS_EN SCC_EN SCC_ISET_SW SCC_TS_SW 5V_VBUS_EN
EXT_VBUS_EN EXT_5V_VBUS_EN EXT_SS_EN EXT_5V_SS_EN JET_PWR_BTN_CTRL
BOOT_N
```

The signal names refer to the board's conditioned ESP32/expander signals; for example, do not interpret `SCC_STAT` as the unconditioned charger IC's open-drain pin. `PMUX_ST:true` means main selected or mux output Hi-Z, so valid input PG is also required to establish main power; `false` selects backup. `SCC_TS_SW:false` selects the real thermistor path. `BOOT_N:false` means SW1/BOOT is pressed; GPIO0 uses the ESP32 internal pull-up because this net has no external pull-up.

`ports` reports sensed enable-pin levels for the four named external outputs. These are not raw output-latch bits: an expander POR latch of `0xFFFF` does not make an input-configured pin appear enabled. Inspect expander validity and the corresponding PG alongside each enable level. Raw output latches and pin direction remain available in `readings.expanders`. The named outputs are:

| API name | Connector | Ownership |
| --- | --- | --- |
| `vbus` | J7, external VBUS | Jetson may request on/off. |
| `5v_vbus` | J8, external 5 V from VBUS | Jetson may request on/off; the ESP32 sequences its required converter. |
| `vbus_ss` | J9, supervised VBUS / Jetson supply | Reserved for the ESP32 power supervisor. |
| `5v_ss` | J10, external supervised 5 V | Jetson may request on/off. |

`shutdown` is `null` before a shutdown starts. During shutdown it contains:

```json
{"shutdown_id":17,"reason":"MAIN_LOSS","deadline_ms":123456,"remaining_ms":59000,"ack":true,"ready":false}
```

The same shutdown identifier persists until reset. Reconnection during shutdown must inspect this object even if the original `shutdown_requested` event was missed.

## Host requests and replies

Every request supplies `v:1`, a positive uint32 `id`, and `cmd`. Every command except `status` also supplies the current `boot_id` exactly as received. IDs correlate asynchronous replies; use a new ID for each new request. The client randomizes its initial ID on process start and never automatically retries a mutation.

| Command | Additional fields | Result |
| --- | --- | --- |
| `status` | None; omit `boot_id`. | Available in every phase; response plus current telemetry. |
| `pong` | `boot_id`, `ping_id` | Acknowledges the current ping for this boot. |
| `shutdown_ack` | `boot_id`, `shutdown_id` | Records receipt of this shutdown request. |
| `shutdown_ready` | `boot_id`, `shutdown_id` | Records that host cleanup is complete and host poweroff is about to start. |
| `port_set` | `boot_id`, `port`, `enabled` | Asynchronously sets one optional output; `enabled` must be a JSON boolean. |
| `reboot` | `boot_id` | Starts the same fixed shutdown sequence with reason `FULL_REBOOT`. |

Examples:

```json
{"v":1,"id":101,"cmd":"status"}
{"v":1,"id":102,"cmd":"pong","boot_id":"AABB0011","ping_id":12}
{"v":1,"id":103,"cmd":"port_set","boot_id":"AABB0011","port":"5v_vbus","enabled":true}
{"v":1,"id":104,"cmd":"shutdown_ack","boot_id":"AABB0011","shutdown_id":17}
{"v":1,"id":105,"cmd":"shutdown_ready","boot_id":"AABB0011","shutdown_id":17}
{"v":1,"id":106,"cmd":"reboot","boot_id":"AABB0011"}
```

Replies add `id`, boolean `ok`, and a string `code` to the common device envelope:

```json
{"v":1,"type":"response","boot_id":"AABB0011","seq":20,"uptime_ms":64000,"id":103,"ok":true,"code":"ACCEPTED"}
```

`OK` means a synchronous request completed. `ACCEPTED` means an asynchronous request was accepted; it does not mean that a power switch has changed or that reboot has finished. A port operation later emits:

```json
{"v":1,"type":"event","boot_id":"AABB0011","seq":21,"uptime_ms":64100,"event":"port_result","request_id":103,"port":"5v_vbus","enabled":true,"ok":true,"code":"OK"}
```

Match `request_id` and inspect the corresponding port and PG telemetry. A missing result must not trigger a blind retry. A new `status` request can establish the current state.

Error codes include `BAD_JSON`, `BAD_REQUEST`, `UNSUPPORTED_VERSION`, `UNKNOWN_COMMAND`, `STALE_BOOT`, `STALE_PING`, `STALE_SHUTDOWN`, `INVALID_PORT`, `RESERVED_PORT`, `BUSY`, `HARDWARE_ERROR`, `ID_CONFLICT`, `NOT_READY`, `SHUTTING_DOWN`, and `QUEUE_FULL`. Errors have `ok:false`. Malformed input may have response ID zero when no valid request ID can be recovered. Hosts should tolerate new error codes.

The device reserves a response cache for the latest **16 `port_set`/`reboot` requests**. Duplicates retained there replay their response without repeating the hardware action; a retained ID with different contents returns `ID_CONFLICT`. Status, pong, ACK, and ready do not consume this cache. Do not rely on duplicate suppression after eviction, and do not reuse old IDs deliberately. A duplicate accepted response does not replay the completed `port_result` event. Use current telemetry to resolve uncertainty. Repeated ACK/ready are naturally idempotent for the current shutdown ID; a new reboot request during shutdown is rejected as `SHUTTING_DOWN`.

## Ping and shutdown sequence

The ESP32 sends `event:"ping"` with a `ping_id` every two seconds while checking host presence. Answer promptly with `pong` using the same ID and boot identifier. Confirmation expires after ten seconds without a valid pong. USB enumeration or a serial open alone does not prove the Jetson application is alive.

On main loss or an accepted full-reboot request, the ESP32 emits:

```json
{"v":1,"type":"event","boot_id":"AABB0011","seq":30,"uptime_ms":64456,"event":"shutdown_requested","shutdown_id":17,"reason":"MAIN_LOSS","deadline_ms":124456,"remaining_ms":60000}
```

The final power-cutoff sequence begins **60 seconds after the original shutdown request starts**. It disables outputs and restores expander defaults with a maximum 750 ms cleanup allowance, then releases GPIO42 backup enable even if cleanup failed. If the ESP32 is still powered, it calls `esp_restart()` after another 250 ms. ACK, ready, repeated requests, USB disconnects, and main-power return do not move the original deadline. Complete host shutdown within 60 seconds; do not budget against the cleanup allowance. `shutdown_ack` and `shutdown_ready` are independent observations; ready does not require a prior ACK and neither requests early cutoff.

At shutdown entry the charger is disabled. Actual main loss also turns off the unbacked J7/J8 enables and J8's converter after charger cleanup; they remain off if main returns during shutdown. Backed J9/J10 and GPIO42 remain available until final cutoff. An API reboot with uninterrupted main leaves the optional ports enabled until final cutoff.

The Jetson should:

1. Match the current boot and shutdown ID and send `shutdown_ack` promptly.
2. Stop accepting new work and finish application cleanup while continuing USB reception and pong responses.
3. Send `shutdown_ready` once cleanup succeeds, immediately before invoking the configured operating-system poweroff action.
4. Complete operating-system shutdown before the original cutoff. The ESP32 eventually disables external ports, restores expander defaults, and releases backup power; if still powered, it restarts.

A ready message expresses the Jetson application's intent. It is not proof that Linux has reached its halted state. A cleanup failure, timeout, or lost USB connection must not be represented as successful final readiness. The ESP32 deadline still applies. The device may also emit `state_changed` with `state`, `power_cut` with the shutdown fields when final cutoff starts, and `power_release` with `expander_defaults_ok` immediately before GPIO42 release. Hosts should ignore unfamiliar event names while retaining their contents for diagnostics.

See [the Python client and host integration instructions](jetson/README.md) for a monitor that performs no host shutdown by default and an explicitly enabled shutdown handler.
