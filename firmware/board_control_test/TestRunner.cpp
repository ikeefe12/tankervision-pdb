#include "BoardControl.h"
#include "ChargerCharacterization.h"
#include "ChargerHold.h"
#include "ControlDiagnostics.h"
#include "PdDiagnostics.h"
#include "TransferApplication.h"
#include <Arduino.h>

#ifndef PDB_CHARGER_HOLD_ON_BOOT
#define PDB_CHARGER_HOLD_ON_BOOT 0
#endif

namespace {
// Retain startup evidence even when USB enumeration finishes after initialization.
class BootLog : public Stream {
 public:
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  size_t write(uint8_t byte) override {
    if (used_ + 1 < sizeof(buffer_)) { buffer_[used_++] = byte; buffer_[used_] = '\0'; }
    if (Serial) Serial.write(byte);
    return 1;
  }
  const char *text() const { return buffer_; }
 private:
  char buffer_[1024] = {};
  size_t used_ = 0;
};
BootLog bootLog;
BoardControl board;
ChargerHold chargerHold(board);
bool ready = false;
bool watching = false;
unsigned passed = 0, failed = 0;
uint32_t lastWatch = 0;
String command;

void holdStatus() {
  Serial.printf("CHARGER_HOLD state=%s auto_boot=%u VCAP=%.4f min=%.4f max=%.4f samples=%lu max_gap_ms=%lu cleanup=%u error=%s\n",
                chargerHold.stateName(), unsigned(PDB_CHARGER_HOLD_ON_BOOT), chargerHold.voltage(),
                chargerHold.minVoltage(), chargerHold.maxVoltage(),
                static_cast<unsigned long>(chargerHold.samples()),
                static_cast<unsigned long>(chargerHold.maxServiceGapMs()),
                chargerHold.cleanupOk(), chargerHold.error());
}

bool snapshot(const char *label) {
  Status s;
  if (!board.readStatus(s)) {
    Serial.printf("ERROR %s: %s\n", label, board.error());
    return false;
  }
  Serial.printf("STATE t=%lu %s IN=0x%02X OUT=0x%02X EXT=0x%02X EOUT=0x%02X "
                "USB=%u DC=%u CAP=%u SCC_PG=%u STAT=%u BOOST=%u BUCK=%u SS=%u "
                "J7=%u J8=%u J9=%u J10=%u MAIN=%u VCAP=%.3f TS_mV=%lu\n",
                static_cast<unsigned long>(millis()), label,
                s.internalInputs, s.internalOutputs, s.externalInputs, s.externalOutputs,
                s.usbPg, s.dcPg, s.backupPg, s.chargerPg, s.chargerStat,
                s.boostPg, s.buckPg, s.ssBuckPg, s.extVbusPg, s.ext5vVbusPg,
                s.extSsPg, s.ext5vSsPg, s.mainSelected, s.vcapV,
                static_cast<unsigned long>(s.tsMv));
  return true;
}

bool check(bool ok, const char *label) {
  if (ok) { ++passed; Serial.printf("PASS %s\n", label); }
  else { ++failed; Serial.printf("FAIL %s (%s)\n", label, board.error()); }
  return ok;
}

// PG must hold its requested level for 100 ms. Each transition has a 2 s
// deadline, comfortably above the 17/22 ms converters and 47/84 ms switches.
bool expect(bool Status::*field, bool expected, const char *label) {
  uint32_t start = millis(), stable = 0;
  bool timing = false;
  while (millis() - start < 2000) {
    Status s;
    if (!board.readStatus(s)) return check(false, label);
    if (!s.mainSelected || !(s.usbPg || s.dcPg) || !s.ssBuckPg) {
      Serial.println("ABORT input source or backed 5 V lost");
      return check(false, label);
    }
    if ((s.*field) == expected) {
      if (!timing) { stable = millis(); timing = true; }
      if (millis() - stable >= 100) {
        Serial.printf("PG_CHECK %s expected=%u observation_ms=%lu\n", label, expected,
                      static_cast<unsigned long>(millis() - start));
        if (!snapshot(label)) return check(false, label);
        return check(true, label);
      }
    } else timing = false;
    delay(10);
  }
  snapshot(label);
  Serial.printf("PG TIMEOUT expected=%u\n", expected);
  return check(false, label);
}

bool holdGood(bool Status::*field, uint32_t duration, const char *label) {
  const uint32_t start = millis();
  while (millis() - start < duration) {
    Status s;
    if (!board.readStatus(s) || !(s.*field) || !s.mainSelected ||
        !(s.usbPg || s.dcPg) || !s.ssBuckPg) return check(false, label);
    delay(20);
  }
  return check(true, label);
}

bool allPortsExcept(Port active, bool enabled) {
  Status s;
  if (!board.readStatus(s)) return false;
  return s.extVbusPg == (enabled && active == Port::Vbus) &&
         s.ext5vVbusPg == (enabled && active == Port::FiveVoltVbus) &&
         s.extSsPg == (enabled && active == Port::Supervised) &&
         s.ext5vSsPg == (enabled && active == Port::FiveVoltSupervised);
}

bool portCycle(Port port, bool Status::*pg, const char *name) {
  Serial.printf("TEST %s OFF -> ON -> OFF (1.5 s visible dwell)\n", name);
  if (!expect(pg, false, "port initial PG low")) return false;
  if (!check(board.setPort(port, true), name)) return false;
  if (!expect(pg, true, "port PG rises")) return false;
  if (!check(allPortsExcept(port, true), "only selected port PG high")) return false;
  if (port == Port::FiveVoltVbus && !expect(&Status::buckPg, true, "J8 prerequisite buck PG")) return false;
  if (!holdGood(pg, 1500, "port PG held throughout dwell")) return false;
  if (!check(board.setPort(port, false), "port disable accepted")) return false;
  if (!expect(pg, false, "port PG falls")) return false;
  if (!check(allPortsExcept(port, false), "all port PG low")) return false;
  delay(500);
  return true;
}

bool basicSequence() {
  Status s;
  if (!board.readStatus(s)) return check(false, "read baseline");
  if (!check(s.mainSelected && (s.usbPg != s.dcPg) && s.ssBuckPg,
             "one input source and backed 5 V valid")) return false;
  if (!check(board.allOff(), "reconcile all controlled outputs off")) return false;
  if (!expect(&Status::boostPg, false, "boost initial PG low") ||
      !expect(&Status::buckPg, false, "buck initial PG low")) return false;
  if (!check(board.setBoost(true), "boost enable")) return false;
  if (!expect(&Status::boostPg, true, "boost PG rises")) return false;
  if (!expect(&Status::chargerPg, true, "charger input PG with boost on, CE off")) return false;
  if (!holdGood(&Status::boostPg, 1500, "boost PG held throughout dwell")) return false;
  if (!check(board.setBoost(false), "boost disable")) return false;
  if (!expect(&Status::boostPg, false, "boost PG falls")) return false;
  Serial.println("INFO SCC_PG may remain high: boost diode pass-through and PG independent of CE");
  if (!check(board.setBuck5V(true), "5 V VBUS buck enable")) return false;
  if (!expect(&Status::buckPg, true, "buck PG rises")) return false;
  if (!holdGood(&Status::buckPg, 1500, "buck PG held throughout dwell")) return false;
  if (!check(board.setBuck5V(false), "5 V VBUS buck disable")) return false;
  if (!expect(&Status::buckPg, false, "buck PG falls")) return false;
  if (!portCycle(Port::Vbus, &Status::extVbusPg, "J7 VBUS")) return false;
  // setPort supplies J8's converter dependency itself and waits for buck PG.
  if (!portCycle(Port::FiveVoltVbus, &Status::ext5vVbusPg, "J8 5V VBUS")) return false;
  if (!check(board.setBuck5V(false), "J8 upstream buck off after port off")) return false;
  if (!expect(&Status::buckPg, false, "J8 upstream buck PG falls")) return false;
  if (!portCycle(Port::Supervised, &Status::extSsPg, "J9 VBUS SS")) return false;
  if (!portCycle(Port::FiveVoltSupervised, &Status::ext5vSsPg, "J10 5V SS")) return false;
  return true;
}

void basicTest() {
  passed = failed = 0;
  Serial.println("BEGIN BASIC TEST: converters + four exterior switches");
  board.setStatusLed(true);
  const bool completed = basicSequence();
  // Capture the failing operation above before cleanup changes error().
  check(board.allOff(), "final all-off writes/readback");
  check(allPortsExcept(Port::Vbus, false), "final exterior PG low");
  board.setStatusLed(false);
  check(snapshot("final"), "final status readable");
  Serial.printf("RESULT BASIC %s passed=%u failed=%u\n",
                completed && failed == 0 ? "PASS" : "FAIL", passed, failed);
  Serial.println("NOT TESTED: charging current, backup transfer, external load capacity, PD contract budget");
}

// Explicit bench command. Only run with the connector setup documented in the
// README: valid bank + real NTC, or electrically open NTC for inhibit-only evidence.
void chargerTest(bool inhibited) {
  passed = failed = 0;
  Serial.printf("BEGIN CHARGER %s\n", inhibited ? "INHIBITED" : "REAL NTC LOW CURRENT");
  bool ok = board.allOff() && board.setChargeHighCurrent(false);
  if (ok) ok = board.setBoost(true);
  if (ok) ok = expect(&Status::boostPg, true, "charger prerequisite boost PG");
  Status s;
  if (ok) ok = board.readStatus(s);
  if (ok && (inhibited ? s.tsMv < 2700 : (s.tsMv < 1100 || s.tsMv > 2400))) {
    Serial.printf("FAIL thermistor test precondition TS=%lu mV\n", static_cast<unsigned long>(s.tsMv));
    ok = false;
  }
  if (ok) ok = check(board.setCharger(true), "charger CE high register and pin readback");
  if (ok) ok = expect(&Status::chargerPg, true, "charger input PG with CE high");
  for (int i = 0; ok && i < 20; ++i) {
    if (!board.readStatus(s) || !s.mainSelected || !(s.usbPg || s.dcPg) ||
        !s.boostPg || !s.chargerPg || s.vcapV > 21.0f ||
        (inhibited ? s.tsMv < 2700 : (s.tsMv < 1100 || s.tsMv > 2400))) {
      Serial.println("FAIL charger guard input/boost/PG/VCAP/TS"); ok = false; break;
    }
    if (!snapshot("charger observing STAT (not a current measurement)")) { ok = false; break; }
    delay(100);
  }
  check(board.setCharger(false), "charger CE low register and pin readback");
  check(snapshot("CE low; charger PG can stay high and STAT blink"), "charger off status readable");
  check(board.allOff(), "charger cleanup");
  Serial.printf("RESULT CHARGER %s; %s\n", ok && failed == 0 ? "PASS" : "FAIL",
                inhibited ? "enable-pin control only; open TS condition, actual current unmeasured" :
                            "enable-pin and input PG evidence; verify charge current externally");
}

void backupTest() {
  passed = failed = 0;
  bool ok = check(board.setBackup(false), "backup initially off");
  if (ok) ok = expect(&Status::backupPg, false, "backup initial PG low");
  if (ok) ok = check(board.setBackup(true), "GPIO42 backup arm (valid charged bank required)");
  if (ok) ok = expect(&Status::backupPg, true, "backup PG rises");
  if (ok) ok = holdGood(&Status::backupPg, 1500, "backup PG held throughout dwell");
  check(board.setBackup(false), "backup disarm");
  if (ok) ok = expect(&Status::backupPg, false, "backup PG falls");
  Serial.printf("RESULT BACKUP %s; source transfer not tested\n", ok && failed == 0 ? "PASS" : "FAIL");
}

void help() {
  Serial.println("Commands: status, test, control-next, pd, charger-inhibit, charger, charger-deep, charger-hold, backup, adc, watch, off, led-on, led-off, help");
  Serial.println("test: boost/buck/four ports; charger commands and backup require README bench preconditions");
}
} // namespace

void setup() {
#if PDB_SUPERCAP_TRANSFER_TEST
  setupTransferApplication();
  return;
#endif
  Serial.begin(115200);
  Serial.setTxTimeoutMs(5);  // An absent/slow serial reader must not stall hold monitoring.
  // Reconcile hardware immediately; USB connection must not gate safe startup.
  ready = board.begin(bootLog);
  Serial.println("TankerVision PDB board control test v4; schematic verified 2026-09-14");
  Serial.printf("INIT %s %s\n", ready ? "PASS" : "FAIL", ready ? "" : board.error());
  help();
  if (ready) snapshot("boot");
  if (ready && PDB_CHARGER_HOLD_ON_BOOT) {
    chargerHold.start();
    holdStatus();
  }
}

void loop() {
#if PDB_SUPERCAP_TRANSFER_TEST
  loopTransferApplication();
  return;
#endif
  if (ready) chargerHold.service();
  unsigned received = 0;
  while (Serial.available() && received++ < 64) {
    char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
    if (c != '\n') {
      if (command.length() < 64) command += c;
      else command = "INVALID";
      continue;
    }
    command.trim();
    Serial.printf("COMMAND %s\n", command.c_str());
    if (command == "help") help();
    else if (!ready) Serial.printf("INIT FAILED: reset after fixing hardware: %s\n", board.error());
    else if (command == "status") { Serial.print(bootLog.text()); snapshot("requested"); holdStatus(); }
    else if (command == "charger-hold") { chargerHold.start(); holdStatus(); }
    else if (command == "off") { check(chargerHold.stop(), "manual all off"); snapshot("off"); holdStatus(); }
    else if (chargerHold.active() && (command == "test" || command == "charger-inhibit" ||
             command == "charger" || command == "charger-deep" || command == "backup" ||
             command == "control-next" || command == "pd"))
      Serial.println("Stop charger-hold with off before running another power test");
    else if (command == "test") basicTest();
    else if (command == "control-next") runControlDiagnostics(board, Serial);
    else if (command == "pd") runPdDiagnostics(Serial);
    else if (command == "charger-inhibit") chargerTest(true);
    else if (command == "charger") chargerTest(false);
    else if (command == "charger-deep") runChargerCharacterization(board, Serial);
    else if (command == "backup") backupTest();
    else if (command == "watch") { watching = !watching; Serial.printf("WATCH %u\n", watching); }
    else if (command == "led-on") board.setStatusLed(true);
    else if (command == "led-off") board.setStatusLed(false);
    else if (command == "adc") {
      const uint8_t pins[] = {1,2,3,5,6,7,8,9,10};
      for (auto pin : pins) Serial.printf("ADC GPIO%u=%lu mV\n", pin, static_cast<unsigned long>(board.readAdcMv(pin)));
    }
    else Serial.println("Unknown command; use help");
    command = "";
    if (ready) chargerHold.service();
  }
  if (ready && watching && millis() - lastWatch >= 500) {
    lastWatch = millis(); snapshot("watch");
    if (PDB_CHARGER_HOLD_ON_BOOT || chargerHold.state() != ChargerHold::State::Stopped) holdStatus();
  }
  delay(2);
}
