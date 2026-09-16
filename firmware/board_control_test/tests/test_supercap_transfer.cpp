#include "SupercapTransfer.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Link the real supervisor against a BoardControl test double. Driver register
// transport is tested separately; here status/PG propagation and time advance
// independently of control requests to test supervisory decisions.
namespace {
struct Action { std::string name; bool enabled; uint32_t at; };
Status fixture;
uint32_t clockMs;
bool backupPin, ledPin, readFails, normalCeFails, emergencyFails, armFails;
bool armPreservesPin, ledFails;
std::vector<Action> actions;

void record(const char *name, bool enabled) { actions.push_back({name, enabled, clockMs}); }
void internalBit(unsigned bit, bool enabled) {
  const uint8_t mask = uint8_t(1U << bit);
  if (enabled) fixture.internalOutputs |= mask;
  else fixture.internalOutputs &= ~mask;
  fixture.internalOutputInputs = fixture.internalOutputs;
}
void resetFixture(float voltage = 5) {
  fixture = Status{};
  fixture.usbPg = fixture.mainSelected = fixture.ssBuckPg = fixture.chargerPg = true;
  fixture.chargerStat = true;
  fixture.tsMv = 1900;
  fixture.vcapV = voltage;
  clockMs = 1000;
  backupPin = ledPin = readFails = normalCeFails = emergencyFails = armFails = false;
  armPreservesPin = ledFails = false;
  actions.clear();
}
bool step(SupercapTransfer &test, uint32_t duration = 20) {
  clockMs += duration;
  return test.service();
}
size_t count(const char *name, bool enabled) {
  size_t total = 0;
  for (const auto &action : actions)
    if (action.name == name && action.enabled == enabled) ++total;
  return total;
}
void loseMain(bool muxChanged = true) {
  fixture.usbPg = fixture.dcPg = false;
  fixture.mainSelected = !muxChanged;
  fixture.boostPg = fixture.chargerPg = fixture.chargerStat = false;
  fixture.tsMv = 0;  // Unbacked charger reference normally collapses.
}
void arm(SupercapTransfer &test) {
  fixture.vcapV = 12;
  assert(step(test));
  assert(test.backupArmed() && backupPin && fixture.backupPg);
}
void assertNeverCutsBackup() {
  assert(count("backup", false) == 0);
  assert(count("release", false) == 0);
  assert(count("allOff", false) == 0);
}
}

uint32_t millis() { return clockMs; }
void delay(uint32_t duration) { clockMs += duration; }
int digitalRead(int pin) { assert(pin == 42 || pin == 4); return pin == 42 ? backupPin : ledPin; }

bool BoardControl::readStatus(Status &status) {
  if (readFails) { std::snprintf(error_, sizeof(error_), "injected status failure"); return false; }
  status = fixture;
  return true;
}
bool BoardControl::setCharger(bool enabled) {
  record("charger", enabled);
  if (readFails || (!enabled && normalCeFails)) {
    std::snprintf(error_, sizeof(error_), "injected CE verification failure");
    return false;
  }
  internalBit(1, enabled);
  return true;
}
bool BoardControl::setBoost(bool enabled) {
  record("boost", enabled);
  if (readFails) return false;
  internalBit(0, enabled);
  fixture.boostPg = enabled;
  return true;
}
bool BoardControl::setBuck5V(bool enabled) {
  record("buck", enabled);
  if (readFails) return false;
  internalBit(4, enabled);
  return true;
}
bool BoardControl::setChargeHighCurrent(bool enabled) {
  record("highCurrent", enabled);
  internalBit(2, enabled);
  return !readFails;
}
bool BoardControl::setThermistorOverride(bool enabled) {
  record("override", enabled);
  internalBit(3, enabled);
  delay(enabled ? 100 : 500);
  return !readFails;
}
bool BoardControl::setPort(Port port, bool enabled) {
  record("port", enabled);
  if (readFails) return false;
  const uint8_t mask = uint8_t(1U << unsigned(port));
  if (enabled) fixture.externalOutputs |= mask;
  else fixture.externalOutputs &= ~mask;
  fixture.externalOutputInputs = fixture.externalOutputs;
  return true;
}
bool BoardControl::setJetsonButton(bool enabled) {
  record("jetson", enabled);
  if (readFails) return false;
  if (enabled) fixture.externalOutputs |= 0x10;
  else fixture.externalOutputs &= ~0x10;
  fixture.externalOutputInputs = fixture.externalOutputs;
  return true;
}
bool BoardControl::setBackup(bool enabled) {
  record("backup", enabled);
  if (armFails && enabled) {
    backupPin = armPreservesPin;
    fixture.backupPg = armPreservesPin;
    std::snprintf(error_, sizeof(error_), "injected arm failure");
    return false;
  }
  backupPin = enabled;
  fixture.backupPg = enabled;
  return true;
}
bool BoardControl::setStatusLed(bool enabled) {
  assert(false && "SupercapTransfer must not own the application heartbeat LED");
  record("led", enabled);
  if (ledFails) { std::snprintf(error_, sizeof(error_), "injected LED failure"); return false; }
  ledPin = enabled;
  return true;
}
bool BoardControl::disableChargingForPowerLoss() {
  record("emergency", false);
  if (emergencyFails) return false;
  internalBit(1, false);
  delay(100);
  internalBit(0, false);
  return true;
}
bool BoardControl::releaseBackupForShutdown() {
  record("release", false);
  backupPin = false;
  return true;
}
bool BoardControl::allOff() { record("allOff", false); return false; }

int main() {
  using State = SupercapTransfer::State;
  unsigned passed = 0;
  {
    resetFixture(); BoardControl board; SupercapTransfer test(board);
    assert(!test.start(false));
    assert(test.state() == State::Fault && count("charger", true) == 0);
    assert(count("boost", true) == 0 && count("override", true) == 0);
    ++passed;
  }
  for (uint32_t badTs : {0U, 1299U, 2351U, 2909U}) {
    resetFixture(); fixture.tsMv = badTs;
    BoardControl board; SupercapTransfer test(board);
    assert(!test.start(true));
    assert(test.state() == State::Fault && count("charger", true) == 0);
    assert(count("override", true) == 0);
    assert(step(test, 100) == false && (fixture.internalOutputs & 3) == 0);
    ++passed;
  }
  {
    resetFixture(); BoardControl board; SupercapTransfer test(board);
    assert(test.start(true));
    assert(test.state() == State::Charging && fixture.internalOutputs == 3);
    assert(count("override", false) == 1 && count("override", true) == 0);
    assert(count("highCurrent", true) == 0 && !backupPin);
    ledPin = true;  // Independently controlled by the application heartbeat.
    fixture.vcapV = 10.0f;
    assert(step(test) && backupPin);  // Bank arms at 10 V, independently of LED.
    fixture.vcapV = 10.001f;
    assert(step(test) && ledPin);
    fixture.vcapV = 9.999f;
    assert(step(test) && ledPin && backupPin);
    assert(count("led", true) == 0 && count("led", false) == 0);
    ++passed;
  }
  {
    resetFixture(); BoardControl board; SupercapTransfer test(board);
    assert(test.start(true)); arm(test);
    fixture.vcapV = 20.23f;
    assert(step(test));
    assert(step(test, 29980));
    assert(test.state() == State::Charging && !test.reachedReady());
    assert(step(test));
    assert(test.state() == State::Ready && test.reachedReady());
    fixture.vcapV = 19.99f;
    assert(step(test) && test.state() == State::Charging);
    ++passed;
  }
  {
    resetFixture(); BoardControl board; SupercapTransfer test(board);
    assert(test.start(true)); arm(test);
    fixture.vcapV = 20.20f; assert(step(test));
    assert(step(test, 29980));
    fixture.vcapV = 20.43f; assert(step(test));  // Rising voltage restarts plateau.
    assert(test.state() == State::Charging);
    assert(step(test, 29980) && test.state() == State::Charging);
    assert(step(test) && test.state() == State::Ready);
    ++passed;
  }
  {
    resetFixture(); BoardControl board; SupercapTransfer test(board);
    assert(test.start(true)); arm(test); actions.clear();
    loseMain(false);  // Input PG falls before mux status follows.
    const uint32_t lost = clockMs + 20;
    assert(step(test));
    assert(test.state() == State::Backup && test.firstLossMs() == lost);
    assert(!test.backupConfirmed() && (fixture.internalOutputs & 2) == 0);
    assert((fixture.internalOutputs & 1) != 0 && count("boost", false) == 0);
    assert(count("charger", false) == 1 && count("port", false) == 4);
    assert(step(test, 80) && count("boost", false) == 0);
    fixture.mainSelected = false;
    assert(step(test));
    assert(test.backupConfirmed() && fixture.internalOutputs == 0);
    assert(test.cleanupOk() && backupPin && count("boost", false) == 1);
    assertNeverCutsBackup();
    assert(step(test, 200) && test.backupDurationMs() == 200);
    // Restoring main power never automatically re-enables the charger.
    fixture.usbPg = fixture.mainSelected = true;
    assert(step(test) && test.state() == State::Returned);
    const uint32_t duration = test.backupDurationMs();
    assert(step(test, 1000) && test.backupDurationMs() == duration);
    assert(count("charger", true) == 0 && count("boost", true) == 0);
    assertNeverCutsBackup();
    ++passed;
  }
  {
    resetFixture(); BoardControl board; SupercapTransfer test(board);
    assert(test.start(true)); arm(test); actions.clear(); loseMain();
    assert(step(test)); assert(step(test, 100));
    fixture.vcapV = 7.0f;
    assert(step(test) && test.cutoffRequested());
    assert(!test.active() && backupPin);
    assert(test.cutoffReason() == SupercapTransfer::CutoffReason::LowVoltage);
    assertNeverCutsBackup();  // Application gets a chance to persist its result.
    ++passed;
  }
  {
    resetFixture(); BoardControl board; SupercapTransfer test(board);
    assert(test.start(true)); arm(test); actions.clear();
    fixture.tsMv = 2909;
    assert(!step(test) && test.state() == State::Fault);
    assert(backupPin && (fixture.internalOutputs & 2) == 0);
    assert(!step(test, 100) && fixture.internalOutputs == 0);
    assertNeverCutsBackup();
    ++passed;
  }
  {
    resetFixture(); BoardControl board; SupercapTransfer test(board);
    assert(test.start(true)); arm(test);
    fixture.chargerStat = false;
    assert(step(test)); assert(step(test, 80));
    fixture.chargerStat = true;
    assert(step(test) && test.state() == State::Charging);  // Brief low recovers.
    fixture.chargerStat = false;
    assert(step(test));
    assert(!step(test, 100) && test.state() == State::Fault);
    assert(std::strstr(test.error(), "STAT") && backupPin);
    ++passed;
  }
  {
    resetFixture(); BoardControl board; SupercapTransfer test(board);
    assert(test.start(true)); arm(test); actions.clear();
    readFails = true;
    assert(!step(test) && test.state() == State::Fault);
    assert(!test.statusValid() && count("emergency", false) == 1);
    assert(fixture.internalOutputs == 0 && backupPin);
    readFails = false;
    assert(!step(test, 100));
    assert(test.statusValid() && test.cleanupOk() && backupPin);
    assertNeverCutsBackup();
    ++passed;
  }
  {
    resetFixture(); BoardControl board; SupercapTransfer test(board);
    assert(test.start(true)); arm(test); actions.clear();
    normalCeFails = emergencyFails = true;
    loseMain();
    assert(!step(test) && test.state() == State::Fault && !test.cleanupOk());
    assert((fixture.internalOutputs & 3) == 3 && count("boost", false) == 0);
    assert(!step(test, 100) && count("boost", false) == 0);
    normalCeFails = emergencyFails = false;
    assert(!step(test)); assert(!step(test, 100));
    assert(fixture.internalOutputs == 0 && test.cleanupOk() && backupPin);
    assertNeverCutsBackup();
    ++passed;
  }
  {
    resetFixture(); BoardControl board; SupercapTransfer test(board);
    assert(test.start(true)); arm(test); actions.clear(); loseMain();
    assert(step(test));
    assert(test.stop() && test.active() && backupPin);
    assert(step(test, 100) && fixture.internalOutputs == 0);
    assertNeverCutsBackup();
    fixture.vcapV = 6.9f;
    assert(step(test) && test.cutoffRequested());
    ++passed;
  }
  {
    resetFixture(); BoardControl board; SupercapTransfer test(board);
    assert(test.start(true)); arm(test); actions.clear();
    assert(test.stop()); assert(!backupPin && count("backup", false) == 1);
    assert(step(test, 100) && !test.active() && test.state() == State::Stopped);
    assert(fixture.internalOutputs == 0);
    ++passed;
  }
  {
    resetFixture(); BoardControl board; SupercapTransfer test(board);
    assert(test.start(true)); actions.clear(); loseMain();
    assert(!step(test) && test.state() == State::Fault);
    assert(std::strstr(test.error(), "before backup was armed"));
    assertNeverCutsBackup();
    ++passed;
  }
  for (float invalidVoltage : {21.0f, NAN, -1.0f}) {
    resetFixture(); BoardControl board; SupercapTransfer test(board);
    assert(test.start(true)); arm(test); actions.clear();
    fixture.vcapV = invalidVoltage;
    assert(!step(test) && test.state() == State::Fault && backupPin);
    assertNeverCutsBackup();
    ++passed;
  }
  {
    resetFixture(); BoardControl board; SupercapTransfer test(board);
    assert(test.start(true)); arm(test); actions.clear(); fixture.backupPg = false;
    assert(!step(test) && test.state() == State::Fault);
    assert(std::strstr(test.error(), "backup switch lost PG") && backupPin);
    assertNeverCutsBackup();
    ++passed;
  }
  {
    resetFixture(); BoardControl board; SupercapTransfer test(board);
    assert(test.start(true)); armFails = armPreservesPin = true;
    fixture.vcapV = 12;
    assert(!step(test) && test.state() == State::Fault && test.backupArmed());
    assert(backupPin);
    ++passed;
  }
  {
    // Reproduce the bench sequence: source returns after one successful backup
    // interval, then is removed again without an explicit new charge command.
    resetFixture(); BoardControl board; SupercapTransfer test(board);
    assert(test.start(true) && test.sourceLossCount() == 0);
    arm(test); actions.clear(); loseMain();
    assert(step(test) && test.state() == State::Backup);
    assert(test.sourceLossCount() == 1);
    const uint32_t firstLoss = test.firstLossMs();
    assert(step(test, 17980));
    fixture.usbPg = fixture.mainSelected = true;
    assert(step(test) && test.state() == State::Returned);
    const uint32_t firstDuration = test.backupDurationMs();
    assert(firstDuration == 18000);
    assert(step(test, 5000) && test.backupDurationMs() == firstDuration);
    assert(test.sourceLossCount() == 1 && test.firstLossMs() == firstLoss);
    loseMain(false);  // A fresh input-PG loss can precede the second mux change.
    const uint32_t secondLoss = clockMs + 20;
    assert(step(test) && test.state() == State::Backup);
    assert(test.sourceLossCount() == 2 && test.firstLossMs() == secondLoss);
    assert(!test.backupConfirmed() && test.backupDurationMs() == 0);
    fixture.mainSelected = false;
    assert(step(test) && test.backupConfirmed());
    assert(test.backupDurationMs() == 0);
    assert(step(test, 15000) && test.state() == State::Backup);
    assert(test.sourceLossCount() == 2 && test.firstLossMs() == secondLoss);
    assert(test.backupDurationMs() == 15000 && test.cleanupOk());
    assert(fixture.internalOutputs == 0 && backupPin && test.backupArmed());
    assert(count("charger", true) == 0 && count("boost", true) == 0);
    assertNeverCutsBackup();
    fixture.usbPg = fixture.mainSelected = true;
    assert(step(test) && test.state() == State::Returned);
    assert(test.backupDurationMs() == 15020);
    assert(step(test, 1000) && test.backupDurationMs() == 15020);
    assert(test.sourceLossCount() == 2 && test.firstLossMs() == secondLoss);
    assert(count("charger", true) == 0 && count("boost", true) == 0);
    assertNeverCutsBackup();
    ++passed;
  }
  {
    // Stop while on backup intentionally keeps bank supervision active. Neither
    // a return nor a subsequent loss may resurrect CHARGING/BACKUP state.
    resetFixture(); BoardControl board; SupercapTransfer test(board);
    assert(test.start(true)); arm(test); loseMain(); assert(step(test));
    assert(test.stop()); assert(step(test, 100)); actions.clear();
    fixture.usbPg = fixture.mainSelected = true;
    assert(step(test) && test.state() == State::Stopped);
    assert(test.active() && backupPin);
    loseMain();
    assert(step(test) && test.state() == State::Stopped);
    assert(step(test, 19999) && test.state() == State::Stopped);
    assert(test.sourceLossCount() == 2);
    assert(test.backupConfirmed() && test.backupDurationMs() == 19999);
    assert(step(test, 1) && test.cutoffRequested());
    assert(test.cutoffReason() == SupercapTransfer::CutoffReason::MainTimeout);
    assert(fixture.internalOutputs == 0 && backupPin);
    assert(count("charger", true) == 0 && count("boost", true) == 0);
    assertNeverCutsBackup();
    ++passed;
  }
  {
    // A fault remains latched across multiple physical handovers; only evidence
    // timing is refreshed. Main return never clears it or restarts charging.
    resetFixture(); BoardControl board; SupercapTransfer test(board);
    assert(test.start(true)); arm(test); fixture.tsMv = 2909;
    assert(!step(test) && test.state() == State::Fault);
    const std::string faultMessage = test.error();
    assert(!step(test, 100)); actions.clear();
    loseMain(); assert(!step(test) && test.state() == State::Fault);
    assert(!step(test, 10000));
    fixture.usbPg = fixture.mainSelected = true;
    assert(!step(test) && test.state() == State::Fault);
    loseMain(); assert(!step(test) && test.state() == State::Fault);
    assert(!step(test, 19999) && test.state() == State::Fault);
    assert(test.sourceLossCount() == 2);
    assert(test.backupConfirmed() && test.backupDurationMs() == 19999);
    assert(step(test, 1) && test.cutoffRequested());
    assert(test.cutoffReason() == SupercapTransfer::CutoffReason::MainTimeout);
    assert(test.error() == faultMessage && fixture.internalOutputs == 0 && backupPin);
    assert(count("charger", true) == 0 && count("boost", true) == 0);
    assertNeverCutsBackup();
    ++passed;
  }
  {
    // Deadline is measured from detected source loss, independently of voltage
    // and independently of the normal 20 ms sampling cadence.
    resetFixture(); BoardControl board; SupercapTransfer test(board);
    assert(test.start(true)); arm(test); actions.clear(); loseMain();
    assert(step(test) && test.sourceLossActive());
    assert(test.sourceLossElapsedMs() == 0 && test.sourceLossRemainingMs() == 20000);
    assert(step(test, 19999) && !test.cutoffRequested());
    assert(test.sourceLossElapsedMs() == 19999 && test.sourceLossRemainingMs() == 1);
    assert(step(test, 1) && test.cutoffRequested());
    assert(test.cutoffReason() == SupercapTransfer::CutoffReason::MainTimeout);
    assert(!test.active() && backupPin && fixture.vcapV == 12);
    assert(test.sourceLossRemainingMs() == 0);
    assertNeverCutsBackup();
    // Only the application may commit the result and physically remove power.
    // Neither stop nor a charge command cancels an already requested cutoff.
    fixture.usbPg = fixture.mainSelected = true;
    assert(!test.stop() && !test.start(true) && test.cutoffRequested());
    assertNeverCutsBackup();
    ++passed;
  }
  {
    // A return at 19,999 ms cancels this episode's deadline. The next episode
    // gets a fresh 20 seconds even when the previous deadline passes meanwhile.
    resetFixture(); BoardControl board; SupercapTransfer test(board);
    assert(test.start(true)); arm(test); actions.clear(); loseMain();
    assert(step(test));
    fixture.usbPg = fixture.mainSelected = true;
    assert(step(test, 19999) && test.state() == State::Returned);
    assert(!test.sourceLossActive() && !test.cutoffRequested());
    assert(step(test, 40000) && !test.cutoffRequested());
    loseMain(); assert(step(test));
    assert(test.sourceLossCount() == 2 && test.sourceLossRemainingMs() == 20000);
    assert(step(test, 19999) && !test.cutoffRequested());
    assert(step(test, 1) && test.cutoffRequested());
    assert(test.cutoffReason() == SupercapTransfer::CutoffReason::MainTimeout);
    assert(count("charger", true) == 0 && count("boost", true) == 0);
    assertNeverCutsBackup();
    ++passed;
  }
  {
    // The controller must obtain fresh main status at the deadline before
    // cutting power, even when that falls inside the normal sample interval.
    resetFixture(); BoardControl board; SupercapTransfer test(board);
    assert(test.start(true)); arm(test); actions.clear(); loseMain();
    assert(step(test)); assert(step(test, 19999));
    fixture.usbPg = fixture.mainSelected = true;
    assert(step(test, 1) && test.state() == State::Returned);
    assert(!test.sourceLossActive() && !test.cutoffRequested());
    assert(step(test, 20000) && !test.cutoffRequested());
    assertNeverCutsBackup();
    ++passed;
  }
  {
    // A failed PG/mux confirmation must not suppress the source-loss timeout.
    resetFixture(); BoardControl board; SupercapTransfer test(board);
    assert(test.start(true)); arm(test); actions.clear(); loseMain(false);
    fixture.backupPg = false;
    assert(!step(test) && test.sourceLossActive() && !test.backupConfirmed());
    assert(!step(test, 19999) && !test.cutoffRequested());
    assert(step(test, 1) && test.cutoffRequested());
    assert(test.cutoffReason() == SupercapTransfer::CutoffReason::MainTimeout);
    assert(!test.backupConfirmed() && backupPin);
    assertNeverCutsBackup();
    ++passed;
  }
  {
    // Once a loss is known, an I2C/status failure cannot erase its deadline.
    resetFixture(); BoardControl board; SupercapTransfer test(board);
    assert(test.start(true)); arm(test); actions.clear(); loseMain();
    assert(step(test));
    const uint32_t lost = test.firstLossMs();
    assert(step(test, 100));
    readFails = true;
    assert(!step(test) && test.state() == State::Fault);
    clockMs = lost + 19999;
    assert(!test.service() && !test.cutoffRequested());
    clockMs = lost + 20000;
    test.service();
    assert(test.cutoffRequested() && !test.statusValid());
    assert(test.cutoffReason() == SupercapTransfer::CutoffReason::MainTimeout);
    assert(backupPin);
    assertNeverCutsBackup();
    ++passed;
  }
  {
    // Normal unsigned subtraction must work with first-loss and deadline on
    // opposite sides of the millis() rollover.
    resetFixture(); clockMs = UINT32_MAX - 10000;
    BoardControl board; SupercapTransfer test(board);
    assert(test.start(true)); arm(test); actions.clear(); loseMain();
    assert(step(test));
    const uint32_t lost = test.firstLossMs();
    assert(step(test, 19999) && clockMs < lost && !test.cutoffRequested());
    assert(test.sourceLossElapsedMs() == 19999 && test.sourceLossRemainingMs() == 1);
    assert(step(test, 1) && test.cutoffRequested());
    assert(test.cutoffReason() == SupercapTransfer::CutoffReason::MainTimeout);
    assertNeverCutsBackup();
    ++passed;
  }
  {
    // A zero-valued timestamp after rollover is an ordinary event timestamp,
    // not an indication that no main loss has been observed.
    resetFixture(); clockMs = UINT32_MAX - 539;
    BoardControl board; SupercapTransfer test(board);
    assert(test.start(true)); arm(test); actions.clear(); loseMain();
    assert(step(test) && test.firstLossMs() == 0 && test.sourceLossActive());
    assert(step(test, 19999) && !test.cutoffRequested());
    assert(step(test, 1) && test.cutoffRequested());
    assert(test.cutoffReason() == SupercapTransfer::CutoffReason::MainTimeout);
    assertNeverCutsBackup();
    ++passed;
  }
  {
    // Failed charger disable must not delay the application's final power cut.
    resetFixture(); BoardControl board; SupercapTransfer test(board);
    assert(test.start(true)); arm(test); actions.clear(); loseMain();
    normalCeFails = emergencyFails = true;
    assert(!step(test) && test.state() == State::Fault);
    assert(!step(test, 19999) && !test.cutoffRequested());
    assert(step(test, 1) && test.cutoffRequested() && !test.cleanupOk());
    assert(count("boost", false) == 0 && backupPin);
    assertNeverCutsBackup();
    ++passed;
  }
  {
    // Return exactly at millis()==0 must still freeze the measured duration;
    // zero is not a usable sentinel for an absent return timestamp.
    resetFixture(); clockMs = UINT32_MAX - 2000;
    BoardControl board; SupercapTransfer test(board);
    assert(test.start(true)); arm(test); actions.clear(); loseMain();
    assert(step(test));
    const uint32_t expectedDuration = uint32_t(0 - test.firstLossMs());
    assert(step(test, 100));
    fixture.usbPg = fixture.mainSelected = true;
    clockMs = 0;
    assert(test.service() && test.state() == State::Returned);
    assert(test.backupDurationMs() == expectedDuration);
    assert(step(test, 20000) && test.backupDurationMs() == expectedDuration);
    assert(!test.cutoffRequested() && !test.sourceLossActive());
    assertNeverCutsBackup();
    ++passed;
  }
  {
    // A bounded emergency shutdown may cross the deadline inside service();
    // the method must request cutoff before returning from that same call.
    resetFixture(); BoardControl board; SupercapTransfer test(board);
    assert(test.start(true)); arm(test); actions.clear(); loseMain();
    assert(step(test)); assert(step(test, 100));
    const uint32_t lost = test.firstLossMs();
    clockMs = lost + 19950;
    readFails = true;
    assert(test.service() && test.cutoffRequested());
    assert(clockMs - lost >= 20000);
    assert(test.cutoffReason() == SupercapTransfer::CutoffReason::MainTimeout);
    assertNeverCutsBackup();
    ++passed;
  }
  std::printf("PASS %u transfer supervisor scenarios: real NTC/budget guards, heartbeat independence, plateau, PG/mux propagation, CE-before-boost shutdown, backup preservation, repeated returns, 20 s deadline/cancellation/rollover, faults and caller-owned cutoff\n", passed);
}
