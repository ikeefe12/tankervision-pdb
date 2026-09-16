#include "BoardHardware.h"
#include "PowerDelivery.h"
#include "PowerManager.h"
#include "Protocol.h"
#include "JsonOutput.h"
#include <USB.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <math.h>
#include <string.h>

#if ARDUINO_USB_MODE || ARDUINO_USB_CDC_ON_BOOT
#error Deployment requires USBMode=default,CDCOnBoot=default; reset hooks must be disabled before USB starts.
#endif

namespace {
constexpr uint32_t kTelemetryMs = 1000, kPollMs = 100, kPingMs = 2000;
constexpr uint32_t kPongFreshMs = 10000, kFinalCleanupMs = 750, kRestartDelayMs = 250;
// DC supplies cannot negotiate. This is an installation assumption, not measured current.
constexpr uint32_t kConfiguredDcBudgetMa = 3000;
BoardHardware board;
PowerDelivery pd;
PowerManager manager;
USBCDC usbSerial;
UsbOutput output;
JsonOutput json;
Protocol::Framer framer;
Protocol::ReplayCache replay;
char bootId[9], lastError[192] = {};
uint32_t sequence = 0, telemetryCount = 0, telemetryLate = 0;
uint32_t lastTelemetry = 0, lastPoll = 0, lastPdPoll = 0, lastLoop = 0, maxLoopGap = 0;
uint32_t activeAction = 0, actionAt = 0, releaseAt = 0;
uint32_t pingId = 0, pingAt = 0, lastPongAt = 0, shutdownId = 0, lastShutdownEvent = 0;
uint32_t externalRequest = 0;
Port externalPort = Port::Vbus;
bool externalEnabled = false, receivedAck = false, receivedReady = false, receivedPong = false;
bool pongSeen = false, released = false, dcBudget = false, monitoring = false;
bool usbWasConnected = false, chargeExpected = false;
bool shedUnbacked = false;
uint8_t shutdownCleanupStage = 0;
uint32_t chargeEnabledAt = 0;
uint8_t actionStage = 0;
PowerManager::Phase reportedPhase = PowerManager::Phase::Settling;

const char *boolean(bool value) { return value ? "true" : "false"; }
const char *portName(Port port) {
  switch (port) {
    case Port::Vbus: return "vbus";
    case Port::FiveVoltVbus: return "5v_vbus";
    case Port::VbusSs: return "vbus_ss";
    case Port::FiveVoltSs: return "5v_ss";
  }
  return "unknown";
}
bool mainPresent() {
  const auto &s = board.snapshot();
  return s.internal.valid && millis() - s.internal.atMs <= 350 && s.mainSelected && (s.usbPg || s.dcPg);
}
bool sourceBudgetValid() {
  const auto &s = board.snapshot();
  if (!mainPresent()) return false;
  if (s.usbPg && !s.dcPg) return pd.budgetValid();
  return s.dcPg && !s.usbPg && dcBudget;
}
void header(const char *type) {
  json.clear();
  json.add("{\"v\":1,\"type\":\"%s\",\"boot_id\":\"%s\",\"seq\":%lu,\"uptime_ms\":%lu",
    type, bootId, (unsigned long)++sequence, (unsigned long)millis());
}
void finish(bool telemetry = false) { json.add("}\n"); output.enqueue(json, telemetry); }
void rememberError(const char *error) { snprintf(lastError, sizeof(lastError), "%s", error); }
void response(uint32_t id, bool ok, const char *code) {
  header("response"); json.add(",\"id\":%lu,\"ok\":%s,\"code\":\"%s\"", (unsigned long)id, boolean(ok), code); finish();
}
void portResult(bool ok, const char *code) {
  if (!externalRequest) return;
  header("event");
  json.add(",\"event\":\"port_result\",\"request_id\":%lu,\"port\":\"%s\",\"enabled\":%s,\"ok\":%s,\"code\":\"%s\"",
    (unsigned long)externalRequest, portName(externalPort), boolean(externalEnabled), boolean(ok), code);
  finish(); externalRequest = 0;
}
void shutdownFields(uint32_t now) {
  json.add("\"shutdown_id\":%lu,\"reason\":\"%s\",\"deadline_ms\":%lu,\"remaining_ms\":%lu",
    (unsigned long)shutdownId, PowerManager::shutdownReasonName(manager.shutdownReason()),
    (unsigned long)(manager.shutdownStartedMs() + PowerManager::kShutdownMs),
    (unsigned long)manager.shutdownRemainingMs(now));
}
void notifyShutdown() {
  if (!shutdownId) shutdownId = 1;
  lastShutdownEvent = millis(); header("event");
  json.add(",\"event\":\"shutdown_requested\","); shutdownFields(millis()); finish();
}
void ping() {
  pingAt = millis(); if (++pingId == 0) ++pingId;
  header("event"); json.add(",\"event\":\"ping\",\"ping_id\":%lu", (unsigned long)pingId); finish();
}
void expander(const char *name, const ExpanderState &s, uint32_t now) {
  json.add("\"%s\":{\"valid\":%s,\"age_ms\":%lu,\"input\":[%u,%u],\"output\":[%u,%u],\"polarity\":[%u,%u],\"config\":[%u,%u],\"physical_inputs\":%u}",
    name, boolean(s.valid), (unsigned long)(now - s.atMs), s.input[0], s.input[1], s.output[0], s.output[1],
    s.polarity[0], s.polarity[1], s.config[0], s.config[1], s.physicalInputs);
}
void telemetry() {
  const uint32_t now = millis(); const auto &s = board.snapshot(); const auto &p = pd.reading();
  ++telemetryCount; header("telemetry");
  json.add(",\"state\":\"%s\",\"reset_reason\":%u,\"settling_remaining_ms\":%lu,\"action\":\"%s\",\"hardware_operation\":%u",
    PowerManager::phaseName(manager.phase()), unsigned(esp_reset_reason()),
    (unsigned long)(manager.phase() == PowerManager::Phase::Settling && now < PowerManager::kSettlingMs ? PowerManager::kSettlingMs - now : 0),
    PowerManager::actionName(manager.pendingAction().kind), unsigned(board.operation()));
  json.add(",\"issue_bits\":%lu,\"issues\":[", (unsigned long)manager.issues());
  bool comma = false;
  for (unsigned i = 0; i < 32; ++i) if (manager.issues() & (1u << i)) {
    if (comma) json.add(","); comma = true; json.quoted(PowerManager::issueName(PowerManager::Issue(1u << i)));
  }
  json.add("],\"last_error\":"); json.quoted(lastError);
  json.add(",\"backup_armed\":%s,\"backup_low_voltage\":%s,\"source_budget_valid\":%s,\"source_budget_ma\":%lu",
    boolean(manager.backupArmed()), boolean(s.vcapV < PowerManager::kBackupArmV), boolean(sourceBudgetValid()),
    (unsigned long)(!sourceBudgetValid() ? 0 : dcBudget ? kConfiguredDcBudgetMa : p.operatingMa()));
  json.add(",\"charger\":{\"enabled_expected\":%s,\"maintenance\":%s}",
    boolean(manager.chargingEnabledExpected()), boolean(manager.maintenance()));
  json.add(",\"jetson\":{\"port_enabled\":%s,\"responsive\":%s,\"pong_seen\":%s,\"last_pong_age_ms\":",
    boolean(s.external.physicalInputs & 0x400), boolean(pongSeen && now - lastPongAt < kPongFreshMs), boolean(pongSeen));
  if (pongSeen) json.add("%lu", (unsigned long)(now - lastPongAt)); else json.add("null");
  json.add(",\"ping_id\":%lu,\"on_feedback\":%s}", (unsigned long)pingId, boolean(s.jetsonOn));
  json.add(",\"shutdown\":");
  if (manager.shutdownReason() == PowerManager::ShutdownReason::None) json.add("null");
  else { json.add("{"); shutdownFields(now); json.add(",\"ack\":%s,\"ready\":%s}", boolean(manager.shutdownAckSeen()), boolean(manager.finalReadySeen())); }
  json.add(",\"ports\":{\"vbus\":%s,\"5v_vbus\":%s,\"vbus_ss\":%s,\"5v_ss\":%s}",
    boolean(s.external.physicalInputs & 0x200), boolean(s.external.physicalInputs & 0x100), boolean(s.external.physicalInputs & 0x400), boolean(s.external.physicalInputs & 0x800));
  json.add(",\"readings\":{\"valid\":%s,\"age_ms\":%lu,\"vcap_v\":%.4f,\"ts_mv\":%lu,\"analog\":{",
    boolean(s.valid), (unsigned long)(now - s.atMs), double(s.vcapV), (unsigned long)s.tsMv);
  for (unsigned i = 0; i < 9; ++i) {
    const auto &a = s.adc[i]; if (i) json.add(",");
    json.add("\"%s\":{\"gpio\":%u,\"raw\":%u,\"mv\":%lu,\"estimated_a\":", BoardHardware::adcName(i), a.gpio, a.raw, (unsigned long)a.mv);
    if (isfinite(a.currentA)) json.add("%.5f", double(a.currentA)); else json.add("null");
    json.add("}");
  }
  json.add("},\"signals\":{");
  const char *names[] = {"PG_USB","PG_DC","VCAP_PG","SCC_PG","SCC_STAT","24V_VBUS_PG","5V_VBUS_PG","5V_SS_PG",
    "EXT_VBUS_PG","EXT_5V_VBUS_PG","EXT_SS_PG","EXT_5V_SS_PG","JET_ON_FB","PMUX_ST","VCAP_EN","STAT_LED",
    "CTRL_IN_INT_N","CTRL_EXT_INT_N","PD_INT","24V_VBUS_EN","SCC_EN","SCC_ISET_SW","SCC_TS_SW","5V_VBUS_EN",
    "EXT_VBUS_EN","EXT_5V_VBUS_EN","EXT_SS_EN","EXT_5V_SS_EN","JET_PWR_BTN_CTRL","BOOT_N"};
  const uint16_t inPins = s.internal.physicalInputs, extPins = s.external.physicalInputs;
  const bool values[] = {s.usbPg,s.dcPg,s.backupPg,s.chargerPg,s.chargerStat,s.boostPg,s.buckPg,s.ssBuckPg,
    s.extVbusPg,s.ext5vVbusPg,s.extSsPg,s.ext5vSsPg,s.jetsonOn,s.mainSelected,s.backupEnabled,s.statusLed,
    !s.internalIrqLow,!s.externalIrqLow,!s.pdIrqLow,bool(inPins&0x100),bool(inPins&0x200),bool(inPins&0x400),bool(inPins&0x800),bool(inPins&0x1000),
    bool(extPins&0x200),bool(extPins&0x100),bool(extPins&0x400),bool(extPins&0x800),bool(extPins&0x1000),!s.bootButtonLow};
  static_assert(sizeof(values)/sizeof(values[0]) == sizeof(names)/sizeof(names[0]), "signal map");
  for (unsigned i = 0; i < sizeof(values)/sizeof(values[0]); ++i) json.add("%s\"%s\":%s", i ? "," : "", names[i], boolean(values[i]));
  json.add("},\"expanders\":{"); expander("internal", s.internal, now); json.add(","); expander("external", s.external, now);
  json.add("},\"edges\":{\"internal_fall\":%lu,\"internal_rise\":%lu,\"external_fall\":%lu,\"external_rise\":%lu,\"mux_fall\":%lu,\"mux_rise\":%lu,\"pd_fall\":%lu,\"pd_rise\":%lu}",
    (unsigned long)s.internalIrqFalls,(unsigned long)s.internalIrqRises,(unsigned long)s.externalIrqFalls,(unsigned long)s.externalIrqRises,
    (unsigned long)s.muxFalls,(unsigned long)s.muxRises,(unsigned long)s.pdIrqFalls,(unsigned long)s.pdIrqRises);
  json.add(",\"pd\":{\"valid\":%s,\"age_ms\":%lu,\"device_mode\":%u,\"silicon_id\":%u,\"status\":%lu,\"type_c_status\":%u,\"pdo\":%lu,\"rdo\":%lu,\"response\":%lu,\"interrupt_status\":%u,\"attached\":%s,\"millivolts\":%lu,\"source_ma\":%lu,\"operating_ma\":%lu,\"maximum_ma\":%lu,\"versions\":[",
    boolean(p.valid),(unsigned long)(now-p.atMs),p.mode,p.silicon,(unsigned long)p.status,p.typeC,(unsigned long)p.pdo,(unsigned long)p.rdo,
    (unsigned long)p.response,p.interrupt,boolean(p.attached()),(unsigned long)p.millivolts(),(unsigned long)p.sourceMa(),(unsigned long)p.operatingMa(),(unsigned long)p.maximumMa());
  for (unsigned i = 0; i < sizeof(p.versions); ++i) json.add("%s%u", i ? "," : "", p.versions[i]);
  json.add("]}},\"telemetry\":{\"generated\":%lu,\"late_periods\":%lu,\"usb_connected\":%s,\"queued\":%u,\"dropped_messages\":%lu,\"dropped_telemetry\":%lu,\"max_loop_gap_ms\":%lu}}\n",
    (unsigned long)telemetryCount,(unsigned long)telemetryLate,boolean(output.connected()),output.queued(),(unsigned long)output.dropped(),
    (unsigned long)output.telemetryDropped(),(unsigned long)maxLoopGap);
  output.enqueue(json, true);
}

void actionDone(bool ok, const char *error = nullptr) {
  if (ok && manager.pendingAction().kind == PowerManager::Action::EnableCharging) {
    chargeExpected = true; chargeEnabledAt = millis();
  }
  if (ok && manager.pendingAction().kind == PowerManager::Action::StopCharging) chargeExpected = false;
  if (!ok) rememberError(error && *error ? error : "HARDWARE_ACTION_FAILED");
  manager.completeAction(activeAction, ok);
}
void serviceAction() {
  const auto action = manager.pendingAction(); const uint32_t now = millis();
  if (action.kind == PowerManager::Action::None) {
    if (externalRequest && !board.busy()) portResult(board.result() == BoardHardware::Result::Succeeded, board.result() == BoardHardware::Result::Succeeded ? "OK" : "HARDWARE_ERROR");
    return;
  }
  const bool first = action.id != activeAction;
  if (first) { activeAction = action.id; actionAt = now; actionStage = 0; }
  switch (action.kind) {
    case PowerManager::Action::PrepareCharging:
      if (first && !board.startReconcile()) { actionDone(false, board.error()); return; }
      if (actionStage == 0 && !board.busy()) {
        if (board.result() != BoardHardware::Result::Succeeded) { actionDone(false, board.error()); return; }
        const auto &s = board.snapshot();
        dcBudget = mainPresent() && s.dcPg && !s.usbPg && kConfiguredDcBudgetMa >= PowerDelivery::kRequestedMa;
        if (dcBudget) { actionStage = 2; if (!board.startPrepareCharge()) actionDone(false, board.error()); }
        else {
          if (!pd.poll(mainPresent() && s.usbPg) || !pd.startBudget()) { actionDone(false, pd.error()); return; }
          actionStage = 1;
        }
      } else if (actionStage == 1) {
        if (pd.result() == PowerDelivery::Result::Succeeded) {
          if (!sourceBudgetValid()) { actionDone(false, "CHARGER_INPUT_BUDGET_UNVERIFIED"); return; }
          actionStage = 2; if (!board.startPrepareCharge()) actionDone(false, board.error());
        } else if (pd.result() == PowerDelivery::Result::Failed) actionDone(false, pd.error());
      } else if (actionStage == 2 && !board.busy()) actionDone(board.result() == BoardHardware::Result::Succeeded, board.error());
      break;
    case PowerManager::Action::EnableCharging:
    case PowerManager::Action::StopCharging:
    case PowerManager::Action::EnableJetson:
      if (first) {
        bool ok = false;
        if (action.kind == PowerManager::Action::EnableCharging) {
          if (!sourceBudgetValid()) { actionDone(false, "CHARGER_INPUT_BUDGET_UNVERIFIED"); return; }
          ok = board.startEnableCharge();
        }
        else if (action.kind == PowerManager::Action::StopCharging) {
          portResult(false, "HARDWARE_ERROR"); ok = board.startDisableCharging();
        }
        else {
          if (!sourceBudgetValid()) { actionDone(false, "JETSON_INPUT_BUDGET_UNVERIFIED"); return; }
          ok = board.startPort(Port::VbusSs, true);
        }
        if (!ok) { actionDone(false, board.error()); return; }
      }
      if (!board.busy()) actionDone(board.result() == BoardHardware::Result::Succeeded, board.error());
      break;
    case PowerManager::Action::ArmBackup:
      if (first && !board.setBackup(true)) { actionDone(false, board.error()); return; }
      if (board.snapshot().internal.valid && board.snapshot().backupPg) actionDone(true);
      else if (now - actionAt >= 2000) actionDone(false, "BACKUP_PG_TIMEOUT");
      break;
    case PowerManager::Action::PingJetson:
      ping(); actionDone(true); break;
    case PowerManager::Action::RequestJetsonShutdown:
      if (first) {
        pd.cancel(); portResult(false, "SHUTTING_DOWN"); notifyShutdown();
        if (!board.startDisableCharging()) rememberError(board.error());
        shutdownCleanupStage = 1;
      }
      actionDone(true); break;
    case PowerManager::Action::CutPower:
      if (first) {
        pd.cancel(); portResult(false, "SHUTTING_DOWN");
        header("event"); json.add(",\"event\":\"power_cut\","); shutdownFields(now); finish();
        if (!board.startRestorePor()) rememberError(board.error());
      }
      if (!released && (!board.busy() || now - actionAt >= kFinalCleanupMs)) {
        if (board.result() != BoardHardware::Result::Succeeded) rememberError("EXPANDER_DEFAULT_RESTORE_FAILED");
        header("event"); json.add(",\"event\":\"power_release\",\"expander_defaults_ok\":%s", boolean(board.result() == BoardHardware::Result::Succeeded)); finish();
        // This direct GPIO action happens even if I2C failed. The CPU can stop here.
        released = true; releaseAt = millis();
        if (!board.releaseBackupForShutdown()) rememberError("BACKUP_RELEASE_READBACK_FAILED");
      }
      if (released && millis() - releaseAt >= kRestartDelayMs) esp_restart();
      break;
    default: break;
  }
}

void handle(const Protocol::Request &request) {
  using Command = Protocol::Command;
  if (request.command == Command::Status) { response(request.id, true, "OK"); telemetry(); return; }
  Protocol::ReplayCache::Reply previous;
  const bool mutation = request.command == Command::PortSet || request.command == Command::Reboot;
  const auto match = mutation ? replay.lookup(request, previous) : Protocol::ReplayCache::Match::New;
  if (match != Protocol::ReplayCache::Match::New) {
    response(request.id, match == Protocol::ReplayCache::Match::Same && previous.ok,
      match == Protocol::ReplayCache::Match::Same ? previous.code : "ID_CONFLICT"); return;
  }
  bool ok = false; const char *code = "BAD_REQUEST";
  if (strcmp(request.bootId, bootId)) code = "STALE_BOOT";
  else if (request.command == Command::Pong) {
    if (!pingId || request.pingId != pingId || millis() - pingAt >= kPongFreshMs) code = "STALE_PING";
    else { receivedPong = true; pongSeen = true; lastPongAt = millis(); ok = true; code = "OK"; }
  } else if (request.command == Command::ShutdownAck || request.command == Command::ShutdownReady) {
    if (!shutdownId || request.shutdownId != shutdownId || manager.phase() != PowerManager::Phase::ShutdownWait) code = "STALE_SHUTDOWN";
    else { if (request.command == Command::ShutdownAck) receivedAck = true; else receivedReady = true; ok = true; code = "OK"; }
  } else if (manager.phase() == PowerManager::Phase::ShutdownWait || manager.phase() == PowerManager::Phase::PowerOff) code = "SHUTTING_DOWN";
  else if (request.command == Command::Reboot) {
    ok = manager.requestFullReboot(millis()); if (ok && !shutdownId) shutdownId = 1; code = ok ? "ACCEPTED" : "SHUTTING_DOWN";
  } else if (request.command == Command::PortSet) {
    Port port = Port::Vbus; bool found = false;
    for (unsigned i = 0; i < 4; ++i) if (!strcmp(request.port, portName(Port(i)))) { port = Port(i); found = true; }
    if (!found) code = "INVALID_PORT";
    else if (port == Port::VbusSs) code = "RESERVED_PORT";
    else if (manager.phase() != PowerManager::Phase::Running) code = "NOT_READY";
    else if (board.busy() || externalRequest) code = "BUSY";
    else if (!mainPresent()) code = "SHUTTING_DOWN";
    else if (request.enabled && !sourceBudgetValid()) { code = "NOT_READY"; rememberError("PORT_INPUT_BUDGET_UNVERIFIED"); }
    else if (!board.startPort(port, request.enabled)) { code = "HARDWARE_ERROR"; rememberError(board.error()); }
    else { externalRequest = request.id; externalPort = port; externalEnabled = request.enabled; ok = true; code = "ACCEPTED"; }
  }
  if (mutation) replay.remember(request, {ok, code});
  response(request.id, ok, code);
}
void receive() {
  unsigned bytes = 0;
  while (output.roomForReply() && usbSerial.available() > 0 && bytes++ < 128) {
    const int value = usbSerial.read(); if (value < 0) break;
    const auto result = framer.push(char(value));
    if (result == Protocol::Framer::Result::None) continue;
    if (result != Protocol::Framer::Result::Line) { response(0, false, "BAD_JSON"); continue; }
    if (!*framer.line()) continue; // New connection separator, not a request.
    Protocol::Request request; const char *error = Protocol::parse(framer.line(), request);
    if (error) response(request.id, false, error); else handle(request);
  }
}
} // namespace

void setupDeployment() {
  snprintf(bootId, sizeof(bootId), "%08lX", (unsigned long)esp_random());
  manager.begin(0);
  monitoring = board.beginMonitoring(); if (!monitoring) rememberError(board.error());
  pd.begin();
  // Own the CDC instance so hooks are disabled before any USB enumeration.
  usbSerial.enableReboot(false); usbSerial.setTxTimeoutMs(2); usbSerial.setRxBufferSize(1024);
  USB.productName("TankerVision PDB"); USB.manufacturerName("TankerVision");
  uint8_t mac[6]; char serialNumber[18];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(serialNumber, sizeof(serialNumber), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  USB.serialNumber(serialNumber); USB.usbPower(2); usbSerial.begin(115200); USB.begin();
  board.poll(); pd.poll(mainPresent() && board.snapshot().usbPg);
  output.service(usbSerial); telemetry(); lastTelemetry = millis();
  lastPoll = lastPdPoll = lastLoop = millis();
}

void loopDeployment() {
  if (released) {
    // No GPIO, ADC, I2C or PD work after the final backup release.
    output.service(usbSerial);
    if (millis() - releaseAt >= kRestartDelayMs) esp_restart();
    delay(1); return;
  }
  const uint32_t now = millis();
  const uint32_t gap = now - lastLoop; if (gap > maxLoopGap) maxLoopGap = gap; lastLoop = now;
  output.service(usbSerial);
  if (usbWasConnected != output.connected()) { usbWasConnected = output.connected(); framer.reset(); }
  if (now - lastPoll >= kPollMs) { lastPoll = now; board.poll(); }
  const auto &s = board.snapshot();
  PowerManager::Inputs input;
  // Low PMUX_ST directly establishes backup selection even if an expander read fails.
  input.statusValid = (!s.mainSelected || s.internal.valid) && millis() - s.atMs <= 350;
  input.mainPresent = mainPresent(); input.vcapV = s.vcapV; input.boostPowerGood = s.boostPg;
  input.chargeFault = !s.internal.valid || !s.boostPg || !s.chargerPg || s.tsMv < 1300 || s.tsMv > 2350;
  if (chargeExpected && !sourceBudgetValid()) input.chargeFault = true;
  // Verify a sample taken after CE assertion; the previous poll legitimately has CE low.
  if (chargeExpected && uint32_t(s.internal.atMs - chargeEnabledAt) < 0x80000000u) {
    const auto &r = s.internal;
    const uint8_t pins = uint8_t(r.physicalInputs >> 8);
    input.chargeFault |= r.config[0] != 0xff || r.config[1] != 0xe0 || r.polarity[0] || r.polarity[1] ||
      (r.output[1] & 0x0f) != 0x03 || (pins & 0x0f) != 0x03;
  }
  if (manager.chargingEnabledExpected() && mainPresent() && input.chargeFault) {
    if (!s.internal.valid) rememberError("CHARGER_STATUS_UNAVAILABLE");
    else if (!sourceBudgetValid()) rememberError("CHARGER_INPUT_BUDGET_UNVERIFIED");
    else if (s.tsMv < 1300 || s.tsMv > 2350) rememberError("CHARGER_TEMPERATURE_INVALID");
    else if (!s.boostPg || !s.chargerPg) rememberError("CHARGER_POWER_GOOD_LOST");
    else rememberError("CHARGER_CONTROL_READBACK_INVALID");
  }
  input.shutdownAck = receivedAck; input.finalReady = receivedReady; input.jetsonPong = receivedPong;
  receivedAck = receivedReady = receivedPong = false;
  manager.tick(millis(), input);
  if (manager.phase() != reportedPhase) {
    reportedPhase = manager.phase();
    if (reportedPhase == PowerManager::Phase::ShutdownWait && !shutdownId) shutdownId = 1;
    header("event"); json.add(",\"event\":\"state_changed\",\"state\":\"%s\"", PowerManager::phaseName(reportedPhase)); finish();
  }
  board.setLed(manager.phase() == PowerManager::Phase::Settling || ((millis() - PowerManager::kSettlingMs) / 500) % 2 == 0);
  // Dispatch preempting actions before progressing the old hardware operation.
  serviceAction();
  if (released) { output.service(usbSerial); delay(1); return; }
  board.service();
  if (manager.phase() == PowerManager::Phase::ShutdownWait) {
    // Actual input loss latches unbacked outputs off, so restored main cannot
    // energize them again during shutdown. A reboot with uninterrupted main
    // leaves these outputs on until the final cutoff sequence.
    if (input.statusValid && !input.mainPresent) shedUnbacked = true;
    if (!board.busy()) {
      if (shutdownCleanupStage == 1) {
        if (board.result() != BoardHardware::Result::Succeeded) rememberError(board.error());
        else chargeExpected = false;
        shutdownCleanupStage = 2;
      } else if (shutdownCleanupStage == 2 && shedUnbacked) {
        if (!board.startPort(Port::Vbus, false)) rememberError(board.error());
        shutdownCleanupStage = 3;
      } else if (shutdownCleanupStage == 3) {
        if (board.result() != BoardHardware::Result::Succeeded) rememberError(board.error());
        if (!board.startPort(Port::FiveVoltVbus, false)) rememberError(board.error());
        shutdownCleanupStage = 4;
      } else if (shutdownCleanupStage == 4) {
        if (board.result() != BoardHardware::Result::Succeeded) rememberError(board.error());
        shutdownCleanupStage = 5;
      }
    }
  }
  if (manager.phase() != PowerManager::Phase::ShutdownWait && manager.phase() != PowerManager::Phase::PowerOff) pd.service();
  if (now - lastPdPoll >= 1000 && pd.result() != PowerDelivery::Result::Running) {
    lastPdPoll = now; pd.poll(mainPresent() && s.usbPg);
  }
  if (manager.jetsonEnabled() && !shutdownId && now - pingAt >= kPingMs) ping();
  if (shutdownId && manager.phase() == PowerManager::Phase::ShutdownWait && now - lastShutdownEvent >= 1000) notifyShutdown();
  receive();
  const uint32_t telemetryNow = millis(), elapsed = telemetryNow - lastTelemetry;
  if (elapsed >= kTelemetryMs) {
    telemetryLate += elapsed / kTelemetryMs - 1;
    lastTelemetry += (elapsed / kTelemetryMs) * kTelemetryMs; telemetry();
  }
  output.service(usbSerial); delay(1);
}
