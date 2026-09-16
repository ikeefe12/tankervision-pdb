#include "TransferApplication.h"
#include "BoardControl.h"
#include "PdDiagnostics.h"
#include "SupercapTransfer.h"
#include "StatusHeartbeat.h"
#if PDB_USB_RECOVERY_TEST
#include "UsbRecoveryDiagnostics.h"
#endif
#include <Preferences.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <stdarg.h>

namespace {
BoardControl board;
SupercapTransfer transfer(board);
StatusHeartbeat ledHeartbeat(board);
Preferences evidence;
bool initialized = false, storageReady = false, cutoffDone = false;
bool reportedBackup = false, reportedReturn = false, reportedReady = false;
bool heartbeatFaultReported = false;
uint32_t session = 0, sequence = 0, lastHeartbeat = 0, lastEdges = 0;
uint32_t reportedLossCount = 0;
uint32_t previousSession = 0, previousUptime = 0;
String command, previousPhase;
esp_reset_reason_t resetReason;
volatile uint32_t muxEdges = 0, muxFalls = 0, muxRises = 0, muxEdgeAt = 0;
volatile bool muxLevel = true;
SupercapTransfer::State lastState = SupercapTransfer::State::Stopped;
uint32_t previousSignals = UINT32_MAX;
constexpr unsigned kEvents = 48, kEventBytes = 420;
char events[kEvents][kEventBytes] = {};
unsigned eventCount = 0;
uint32_t droppedBytes = 0;
char initializationError[192] = {};
struct EvidenceRecord {
  uint32_t magic, version, session, uptime, sequence;
  float vcap;
  char phase[32];
};
constexpr uint32_t kEvidenceMagic = 0x50444254, kEvidenceVersion = 1;

#if PDB_USB_RECOVERY_TEST
UsbRecoveryDiagnostics usbDiagnostics;
uint8_t savedUsbBlob[UsbRecoveryDiagnostics::kBlobBytes] = {};
uint8_t olderUsbBlob[UsbRecoveryDiagnostics::kBlobBytes] = {};
size_t savedUsbBytes = 0;
size_t olderUsbBytes = 0;
uint8_t usbCheckpointWindows = 0;
bool usbPriorArchived = false;
#endif

bool serialReady() {
  const bool connected = bool(Serial);
#if PDB_USB_RECOVERY_TEST
  // Observe the existing HWCDC probe; avoid adding an extra isConnected() call.
  usbDiagnostics.observeConnection(connected);
#endif
  return connected;
}

class StartupLog : public Stream {
 public:
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  size_t write(uint8_t c) override {
    if (used + 1 < sizeof(data)) { data[used++] = c; data[used] = 0; }
    return 1;
  }
  char data[1200] = {};
  size_t used = 0;
} startup;

void emit(const char *format, ...) {
  char *line = events[eventCount++ % kEvents];
  va_list args;
  va_start(args, format);
  vsnprintf(line, kEventBytes, format, args);
  va_end(args);
  if (serialReady()) {
    const size_t n = strlen(line);
    droppedBytes += n - Serial.write(reinterpret_cast<const uint8_t *>(line), n);
    Serial.println();
  }
}

const char *resetName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_SW: return "SOFTWARE";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_USB: return "USB";
    default: return "OTHER";
  }
}

void ARDUINO_ISR_ATTR muxInterrupt() {
  const bool level = digitalRead(11);
  muxEdges = muxEdges + 1;
  if (level) muxRises = muxRises + 1; else muxFalls = muxFalls + 1;
  muxLevel = level;
  muxEdgeAt = millis();
}

void checkpoint(const char *phase) {
  if (!storageReady) { emit("EVIDENCE NVS_UNAVAILABLE phase=%s", phase); return; }
  // Milestones only. Main-loss cleanup occurs before any flash transaction.
  // Store one complete record so a partial series of field writes cannot mix
  // one session's phase with another session's identity or uptime.
  EvidenceRecord record = {};
  record.magic = kEvidenceMagic;
  record.version = kEvidenceVersion;
  record.session = session;
  record.uptime = millis();
  record.sequence = sequence;
  record.vcap = transfer.voltage();
  snprintf(record.phase, sizeof(record.phase), "%s", phase);
  if (evidence.putBytes("last", &record, sizeof(record)) != sizeof(record))
    emit("EVIDENCE WRITE_FAILED phase=%s session=%08lX", phase, static_cast<unsigned long>(session));
}

void bootReport() {
  emit("BOOT session=%08lX reset=%s(%u) prior_session=%08lX prior_phase=%s prior_uptime_ms=%lu init=%u error=%s",
       static_cast<unsigned long>(session), resetName(resetReason), unsigned(resetReason),
       static_cast<unsigned long>(previousSession), previousPhase.c_str(),
       static_cast<unsigned long>(previousUptime), initialized, initializationError);
  if (serialReady()) Serial.print(startup.data);
  emit("CONFIG target_V=20.629 bank_rating_V=21.6 override=0 ISET_HIGH=0 LED=heartbeat LED_period_ms=1000 main_loss_timeout_ms=20000 arm_V=10 cutoff_V=7 ADC_trip_V=21.0 auto_start=0");
#if PDB_USB_RECOVERY_TEST
  emit("USB_TEST enabled=1 automatic_repair=0 snapshot_windows_ms=10000,45000 snapshots_only_with_main_and_outputs_off=1");
#endif
}

void heartbeat() {
  Status s;
  const bool valid = transfer.active()
      ? (s = transfer.lastStatus(), transfer.statusValid()) : board.readStatus(s);
  if (!valid) {
    emit("HEARTBEAT session=%08lX seq=%lu t=%lu state=%s STATUS_INVALID error=%s",
         static_cast<unsigned long>(session), static_cast<unsigned long>(++sequence),
         static_cast<unsigned long>(millis()), transfer.stateName(), board.error());
    return;
  }
  if (serialReady()) Serial.printf("HEARTBEAT session=%08lX seq=%lu t=%lu uptime_us=%llu state=%s VCAP=%.4f TS_mV=%lu IN=0x%02X OUT=0x%02X EOUT=0x%02X MAIN=%u USB=%u DC=%u CAP=%u SS=%u BOOST=%u CHG_PG=%u STAT=%u EN42=%u LED=%u samples=%lu max_gap_ms=%lu edges=%lu backup_ms=%lu dropped_bytes=%lu cycle=%lu loss_ms=%lu shutdown_in_ms=%lu\n",
      static_cast<unsigned long>(session), static_cast<unsigned long>(++sequence),
      static_cast<unsigned long>(millis()), static_cast<unsigned long long>(esp_timer_get_time()),
      transfer.stateName(), s.vcapV, static_cast<unsigned long>(s.tsMv),
      s.internalInputs, s.internalOutputs, s.externalOutputs, s.mainSelected, s.usbPg,
      s.dcPg, s.backupPg, s.ssBuckPg, s.boostPg, s.chargerPg, s.chargerStat,
      digitalRead(42), digitalRead(4), static_cast<unsigned long>(transfer.samples()),
      static_cast<unsigned long>(transfer.maxServiceGapMs()),
      static_cast<unsigned long>(muxEdges), static_cast<unsigned long>(transfer.backupDurationMs()),
      static_cast<unsigned long>(droppedBytes), static_cast<unsigned long>(transfer.sourceLossCount()),
      static_cast<unsigned long>(transfer.sourceLossElapsedMs()),
      static_cast<unsigned long>(transfer.sourceLossRemainingMs()));
}

void start() {
  if (!initialized || transfer.active() || digitalRead(42) || cutoffDone) {
    emit("START REFUSED init=%u active=%u backup_enabled=%u cutoff_latched=%u; reset only with main connected for a fresh test",
         initialized, transfer.active(), digitalRead(42), cutoffDone);
    return;
  }
  Status s;
  if (!board.readStatus(s) || !s.mainSelected || !s.usbPg || s.dcPg || !s.ssBuckPg ||
      s.internalOutputs || s.externalOutputs) {
    emit("START REFUSED requires valid USB main and all controlled outputs off: %s", board.error());
    return;
  }
  checkpoint("VALIDATING_PD");
  emit("PD_REQUEST begin fixed_20V_2000mA; charger remains off during renegotiation");
  PdDiagnostics pd;
  PdSnapshot accepted;
  bool budget = pd.begin() && pd.requestFixed20VCurrent(2000, Serial, accepted);
  if (!budget) emit("PD_REQUEST FAIL %s; charging remains disabled", pd.error());
  pd.end();  // No PD bus transactions during charging or main loss.
  if (!budget) { checkpoint("PD_FAILED"); return; }
  emit("PD_REQUEST PASS PDO=0x%08lX RDO=0x%08lX voltage_mV=%lu operating_mA=%lu limit_mA=%lu",
       static_cast<unsigned long>(accepted.currentPdo), static_cast<unsigned long>(accepted.currentRdo),
       static_cast<unsigned long>(accepted.selectedMillivolts()),
       static_cast<unsigned long>(accepted.operatingMilliamps()),
       static_cast<unsigned long>(accepted.limitMilliamps()));
  checkpoint("STARTING_CHARGE");
  reportedBackup = reportedReturn = reportedReady = false;
  reportedLossCount = 0;
  previousSignals = UINT32_MAX;
  const bool ok = transfer.start(true);
  emit("START %s state=%s error=%s", ok ? "PASS" : "FAIL", transfer.stateName(), transfer.error());
  checkpoint(ok ? "CHARGING" : "CHARGE_FAULT");
}

void service() {
  if (!initialized || cutoffDone) return;
  transfer.service();
  if (transfer.sourceLossCount() != reportedLossCount) {
    reportedLossCount = transfer.sourceLossCount();
    reportedBackup = reportedReturn = false;
    emit("SOURCE_LOSS_CYCLE session=%08lX cycle=%lu loss_ms=%lu grace_ms=20000",
         static_cast<unsigned long>(session), static_cast<unsigned long>(reportedLossCount),
         static_cast<unsigned long>(transfer.firstLossMs()));
  }
  const uint32_t edges = muxEdges;
  if (edges != lastEdges) {
    lastEdges = edges;
    emit("MUX_EDGE session=%08lX t=%lu level=%u edges=%lu falls=%lu rises=%lu",
         static_cast<unsigned long>(session), static_cast<unsigned long>(muxEdgeAt), muxLevel,
         static_cast<unsigned long>(edges), static_cast<unsigned long>(muxFalls),
         static_cast<unsigned long>(muxRises));
  }
  if (transfer.state() != lastState) {
    lastState = transfer.state();
    emit("STATE session=%08lX t=%lu state=%s VCAP=%.4f loss_ms=%lu cleanup=%u error=%s",
         static_cast<unsigned long>(session), static_cast<unsigned long>(millis()),
         transfer.stateName(), transfer.voltage(), static_cast<unsigned long>(transfer.firstLossMs()),
         transfer.cleanupOk(), transfer.error());
  }
  if (transfer.statusValid() && transfer.active()) {
    const Status &s = transfer.lastStatus();
    const uint32_t signals = s.internalInputs | (uint32_t(s.mainSelected) << 8) |
        (uint32_t(s.internalOutputs) << 9) | (uint32_t(digitalRead(42)) << 17) |
        (uint32_t(digitalRead(4)) << 18);
    if (signals != previousSignals) {
      previousSignals = signals;
      emit("SIGNALS t=%lu MAIN=%u USB=%u DC=%u CAP=%u SS=%u IN=0x%02X OUT=0x%02X EN42=%u LED=%u VCAP=%.4f",
           static_cast<unsigned long>(millis()), s.mainSelected, s.usbPg, s.dcPg, s.backupPg,
           s.ssBuckPg, s.internalInputs, s.internalOutputs, digitalRead(42), digitalRead(4), s.vcapV);
    }
  }
  if (transfer.state() == SupercapTransfer::State::Ready && !reportedReady) {
    reportedReady = true;
    emit("READY_FOR_METER_CHECK confirm_VCAP_near_20.63V_before_removing_J1; keep_J2_connected session=%08lX t=%lu VCAP_ADC=%.4f stable_plateau_s=30 backup_armed=%u",
         static_cast<unsigned long>(session), static_cast<unsigned long>(millis()),
         transfer.voltage(), transfer.backupArmed());
    checkpoint("READY");
  }
  if (transfer.backupConfirmed() && !reportedBackup) {
    reportedBackup = true;
    emit("BACKUP_CONFIRMED session=%08lX t=%lu VCAP=%.4f MAIN=0 CAP_PG=1 SS_PG=1; following uptime for reset evidence",
         static_cast<unsigned long>(session), static_cast<unsigned long>(millis()), transfer.voltage());
  }
  if (transfer.state() == SupercapTransfer::State::Returned && !reportedReturn) {
    reportedReturn = true;
    emit("MAIN_RESTORED t=%lu backup_ms=%lu shutdown_cancelled=1 charging=OFF", static_cast<unsigned long>(millis()),
         static_cast<unsigned long>(transfer.backupDurationMs()));
    checkpoint("MAIN_RESTORED");
  }
  if (transfer.cutoffRequested()) {
    emit("INTENTIONAL_BACKUP_SHUTDOWN reason=%s session=%08lX t=%lu VCAP=%.4f status_valid=%u loss_elapsed_ms=%lu backup_ms=%lu",
         transfer.cutoffReasonName(), static_cast<unsigned long>(session), static_cast<unsigned long>(millis()),
         transfer.voltage(), transfer.statusValid(), static_cast<unsigned long>(transfer.sourceLossElapsedMs()),
         static_cast<unsigned long>(transfer.backupDurationMs()));
    checkpoint(transfer.cutoffReason() == SupercapTransfer::CutoffReason::MainTimeout
               ? "MAIN_TIMEOUT_20S" : "LOW_VOLTAGE_CUTOFF");
    delay(20);  // Let the final USB packet leave while still above buck dropout.
    cutoffDone = true;
    ledHeartbeat.stop();
    if (!board.releaseBackupForShutdown()) emit("SHUTDOWN_RELEASE_FAILED %s", board.error());
    // If main arrived at the instant of cutoff, the MCU can remain alive with
    // backup released. Keep indicating loop liveness; charging stays disabled.
    ledHeartbeat.begin();
  }
}

void serviceStatusHeartbeat() {
  if (!initialized) return;
  const uint32_t before = ledHeartbeat.transitions();
  if (!ledHeartbeat.service()) {
    if (!heartbeatFaultReported) emit("LED_HEARTBEAT FAILED %s", ledHeartbeat.error());
    heartbeatFaultReported = true;
  } else if (before != ledHeartbeat.transitions()) {
    emit("LED_HEARTBEAT t=%lu level=%u transitions=%lu",
         static_cast<unsigned long>(millis()), ledHeartbeat.level(),
         static_cast<unsigned long>(ledHeartbeat.transitions()));
  }
}

#if PDB_USB_RECOVERY_TEST
bool usbEvidenceSafe() {
  if (!initialized || cutoffDone || transfer.active() || digitalRead(42)) return false;
  Status s;
  return board.readStatus(s) && s.mainSelected && s.ssBuckPg && (s.usbPg || s.dcPg) &&
      !s.internalOutputs && !s.externalOutputs;
}

void usbBetweenRows() {
  service();
  serviceStatusHeartbeat();
  usbDiagnostics.service();
}

void serviceUsbEvidence() {
  usbDiagnostics.service();
  const uint32_t now = millis();
  const uint32_t windows[] = {10000, 45000};
  for (unsigned i = 0; i < 2; ++i) {
    const uint8_t mask = uint8_t(1u << i);
    if ((usbCheckpointWindows & mask) || now < windows[i]) continue;
    usbCheckpointWindows |= mask;  // At most two attempts per boot, never deferred into backup.
    if (!storageReady || !usbEvidenceSafe()) continue;
    if (!usbPriorArchived) {
      if (savedUsbBytes &&
          evidence.putBytes("usbprev", savedUsbBlob, savedUsbBytes) != savedUsbBytes) {
        emit("USB_NVS ARCHIVE_FAILED; previous saved boot preserved");
        continue;
      }
      usbPriorArchived = true;
    }
    uint8_t blob[UsbRecoveryDiagnostics::kBlobBytes];
    const size_t n = usbDiagnostics.exportBlob(blob, sizeof(blob));
    const bool ok = n && evidence.putBytes("usbdiag", blob, n) == n;
    emit("USB_NVS %s session=%08lX window_ms=%lu t=%lu bytes=%u",
         ok ? "SAVED" : "FAILED", static_cast<unsigned long>(session),
         static_cast<unsigned long>(windows[i]), static_cast<unsigned long>(millis()), unsigned(n));
  }
}

void reportUsb() {
  if (!usbEvidenceSafe()) {
    emit("USB_REPORT REFUSED requires main power, inactive transfer and all controlled outputs off");
    return;
  }
  usbDiagnostics.dump(Serial, usbBetweenRows);
  if (savedUsbBytes) {
    emit("USB_NVS_AT_BOOT bytes=%u; saved record predates this boot and may be an older test", unsigned(savedUsbBytes));
    if (!UsbRecoveryDiagnostics::dumpBlob(Serial, savedUsbBlob, savedUsbBytes, usbBetweenRows))
      emit("USB_NVS_AT_BOOT INVALID");
  } else emit("USB_NVS_AT_BOOT NONE");
  if (olderUsbBytes) {
    emit("USB_NVS_ARCHIVE_AT_BOOT bytes=%u; older saved boot", unsigned(olderUsbBytes));
    if (!UsbRecoveryDiagnostics::dumpBlob(Serial, olderUsbBlob, olderUsbBytes, usbBetweenRows))
      emit("USB_NVS_ARCHIVE_AT_BOOT INVALID");
  }
}
#endif
}  // namespace

void setupTransferApplication() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(5);
  resetReason = esp_reset_reason();
  session = esp_random();
#if PDB_USB_RECOVERY_TEST
  usbDiagnostics.begin(session);
#endif
  initialized = board.begin(startup);
  if (!initialized) snprintf(initializationError, sizeof(initializationError), "%s", board.error());
  if (initialized && !ledHeartbeat.begin()) {
    heartbeatFaultReported = true;
    emit("LED_HEARTBEAT INIT_FAILED %s", board.error());
  }
  muxLevel = digitalRead(11);
  attachInterrupt(digitalPinToInterrupt(11), muxInterrupt, CHANGE);
  storageReady = evidence.begin("pdb-transfer", false);
  if (storageReady) {
    EvidenceRecord prior = {};
    if (evidence.getBytesLength("last") == sizeof(prior) &&
        evidence.getBytes("last", &prior, sizeof(prior)) == sizeof(prior) &&
        prior.magic == kEvidenceMagic && prior.version == kEvidenceVersion) {
      prior.phase[sizeof(prior.phase) - 1] = '\0';
      previousPhase = prior.phase;
      previousSession = prior.session;
      previousUptime = prior.uptime;
    } else {
      // Preserve evidence written by earlier development builds.
      previousPhase = evidence.getString("phase", "NONE");
      previousSession = evidence.getUInt("session", 0);
      previousUptime = evidence.getUInt("uptime", 0);
    }
  } else previousPhase = "NVS_UNAVAILABLE";
#if PDB_USB_RECOVERY_TEST
  if (storageReady && evidence.getBytesLength("usbdiag") == sizeof(savedUsbBlob))
    savedUsbBytes = evidence.getBytes("usbdiag", savedUsbBlob, sizeof(savedUsbBlob));
  if (storageReady && evidence.getBytesLength("usbprev") == sizeof(olderUsbBlob))
    olderUsbBytes = evidence.getBytes("usbprev", olderUsbBlob, sizeof(olderUsbBlob));
#endif
  bootReport();
}

void loopTransferApplication() {
  service();
  serviceStatusHeartbeat();
#if PDB_USB_RECOVERY_TEST
  serviceUsbEvidence();
#endif
  unsigned received = 0;
  while (Serial.available() > 0 && received++ < 64) {
    const int value = Serial.read();
#if PDB_USB_RECOVERY_TEST
    usbDiagnostics.observeApplicationRx(value, value == '\n');
#endif
    if (value < 0) break;
    const char c = static_cast<char>(value);
    if (c == '\r') continue;
    if (c != '\n') { if (command.length() < 64) command += c; else command = "INVALID"; continue; }
    command.trim();
    emit("COMMAND %s", command.c_str());
    if (command == "transfer-start") start();
    else if (command == "status") { bootReport(); heartbeat(); }
#if PDB_USB_RECOVERY_TEST
    else if (command == "usb-status") reportUsb();
#endif
    else if (command == "transfer-stop") {
      const bool ok = initialized && !cutoffDone && transfer.stop();
      emit("STOP %s backup_supervision=%u error=%s", ok ? "PASS" : "FAIL", transfer.backupArmed(), transfer.error());
    } else if (command == "events") {
      const unsigned count = eventCount;
      for (unsigned i = count > kEvents ? count - kEvents : 0; i < count; ++i) {
        Serial.printf("REPLAY %s\n", events[i % kEvents]);
        service();
      }
    } else emit("Commands: status, transfer-start, transfer-stop, events, help. Keep J2 serial open throughout transfer; no automatic start.");
    command = "";
    service();
    serviceStatusHeartbeat();
  }
  if (millis() - lastHeartbeat >= 1000) { lastHeartbeat = millis(); heartbeat(); }
  delay(2);
}
