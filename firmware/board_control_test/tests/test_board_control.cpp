#include "BoardControl.h"
#include "ChargerHold.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

TwoWire Wire;
static uint32_t now;
static int gpio[49];
static uint32_t adcMv[49];
static unsigned i2cCalls;
static uint64_t forcedGpioHigh;
uint32_t millis() { return now; }
void delay(uint32_t ms) { now += ms; }
void digitalWrite(int pin, int value) { gpio[pin] = value; }
int digitalRead(int pin) { return (forcedGpioHigh & (uint64_t{1} << pin)) ? HIGH : gpio[pin]; }
int gpio_set_level(int pin, unsigned value) { gpio[pin] = value; return 0; }
void pinMode(int, int) {}
void analogReadResolution(int) {}
void analogSetPinAttenuation(int, int) {}
uint32_t analogReadMilliVolts(int pin) { return adcMv[pin]; }

uint8_t TwoWire::endTransmission(bool) {
  ++i2cCalls;
  auto &device = address_ == 0x20 ? internal : external;
  if (!device.present) return 2;
  if (tx_.empty()) return 0;
  pointer_ = tx_[0];
  if (tx_.size() == 2) {
    const uint8_t ignored = pointer_ == 3 ? device.ignoredOutputWrites : 0;
    device.reg[pointer_] = (device.reg[pointer_] & ignored) | (tx_[1] & ~ignored);
    writes.push_back({address_, pointer_, tx_[1], now});
  }
  return 0;
}

size_t TwoWire::requestFrom(uint8_t address, uint8_t count) {
  ++i2cCalls;
  rx_.clear();
  auto &device = address == 0x20 ? internal : external;
  if (!device.present) return 0;
  uint8_t input0 = address == 0x20 ? internalInput : externalInput;
  if (automaticPg) {
    const uint8_t in = internal.physicalOutputs(), ext = external.physicalOutputs();
    if (address == 0x20) {
      // Schematic: USB P02, charger PG P03, backed buck P07.
      input0 = 0x8c;
      if ((in & 0x0a) == 0x0a) input0 |= 0x10;  // STAT with CE + fixed TS.
      if (in & 0x01) input0 |= 0x40;  // boost P06
      if ((in & 0x10) && buckPgAvailable) input0 |= 0x02;  // buck P01
      if (gpio[42]) input0 |= 0x20;
    } else {
      input0 = 0;
      if (ext & 0x02) input0 |= 0x08;  // J7 P03
      if ((ext & 0x01) && (in & 0x10) && buckPgAvailable) input0 |= 0x10;
      if (ext & 0x04) input0 |= 0x04;  // J9 P02
      if (ext & 0x08) input0 |= 0x02;  // J10 P01
    }
  }
  for (uint8_t i = 0; i < count; ++i) {
    const uint8_t reg = (pointer_ & 0xfe) | ((pointer_ + i) & 1);
    uint8_t value = device.reg[reg];
    if (reg == 0) value = input0 ^ device.reg[4];
    if (reg == 1) value = device.physicalOutputs() ^ device.reg[5];
    rx_.push_back(value);
  }
  return count;
}

#define REQUIRE(condition) do { if (!(condition)) { \
  std::cerr << __func__ << ':' << __LINE__ << " failed: " #condition << '\n'; \
  return false; } } while (false)

static void reset() {
  Wire = TwoWire{};
  now = 0;
  i2cCalls = 0;
  forcedGpioHigh = 0;
  std::fill(std::begin(gpio), std::end(gpio), 0);
  std::fill(std::begin(adcMv), std::end(adcMv), 1600);
  adcMv[2] = 1000;
  gpio[11] = HIGH;
}
static void retain(MockExpander &device, uint8_t outputs) {
  device.reg[3] = outputs;
  device.reg[7] = 0xe0;
}

static bool missingExternalDisablesCharger() {
  reset(); Stream log; BoardControl board;
  retain(Wire.internal, 0x1f);
  Wire.external.present = false;
  REQUIRE(!board.begin(log));
  REQUIRE(!(Wire.internal.physicalOutputs() & 0x02));
  REQUIRE(std::string(board.error()).find("0x21") != std::string::npos);
  return true;
}

static bool missingInternalDisablesExternal() {
  reset(); Stream log; BoardControl board;
  Wire.internal.present = false;
  retain(Wire.external, 0x1f);
  REQUIRE(!board.begin(log));
  REQUIRE((Wire.external.physicalOutputs() & 0x1f) == 0);
  return true;
}

static bool stuckCePreservesBoostAndProfile() {
  reset(); Stream log; BoardControl board;
  retain(Wire.internal, 0x0f);
  retain(Wire.external, 0x0f);
  Wire.internal.forcedHigh = 0x02;
  REQUIRE(!board.begin(log));
  // CE cannot be forced off; do not remove boost or change ISET/TS beneath it.
  REQUIRE((Wire.internal.physicalOutputs() & 0x0d) == 0x0d);
  REQUIRE((Wire.external.physicalOutputs() & 0x1f) == 0);
  for (const auto &w : Wire.writes)
    if (w.address == 0x20 && w.reg == 3) REQUIRE((w.value & 0x0d) == 0x0d);
  return true;
}

static bool retainedPolarityIsReconciled() {
  reset(); Stream log; BoardControl board;
  retain(Wire.internal, 0x1f);
  Wire.internal.reg[5] = 0xff;
  REQUIRE(board.begin(log));
  REQUIRE(Wire.internal.physicalOutputs() == 0);
  REQUIRE(Wire.internal.reg[5] == 0);
  return true;
}

static bool inputOneHotMapping() {
  reset(); Stream log; BoardControl board;
  REQUIRE(board.begin(log));
  Wire.automaticPg = false;
  // Physical Port 0 order independently traced in schematic-map.json.
  bool Status::*const internal[] = {&Status::dcPg, &Status::buckPg, &Status::usbPg,
      &Status::chargerPg, &Status::chargerStat, &Status::backupPg,
      &Status::boostPg, &Status::ssBuckPg};
  bool Status::*const external[] = {&Status::jetsonOn, &Status::ext5vSsPg,
      &Status::extSsPg, &Status::extVbusPg, &Status::ext5vVbusPg};
  for (unsigned bit = 0; bit < 8; ++bit) {
    Wire.internalInput = 1u << bit; Wire.externalInput = 0;
    Status s; REQUIRE(board.readStatus(s));
    for (unsigned i = 0; i < 8; ++i) REQUIRE(s.*internal[i] == (i == bit));
    for (auto field : external) REQUIRE(!(s.*field));
  }
  for (unsigned bit = 0; bit < 5; ++bit) {
    Wire.internalInput = 0; Wire.externalInput = 1u << bit;
    Status s; REQUIRE(board.readStatus(s));
    for (unsigned i = 0; i < 5; ++i) REQUIRE(s.*external[i] == (i == bit));
    for (auto field : internal) REQUIRE(!(s.*field));
  }
  return true;
}

static bool j8StartsBuckFirstAndGuardsShutdown() {
  reset(); Stream log; BoardControl board;
  REQUIRE(board.begin(log));
  Wire.writes.clear();
  REQUIRE(board.setPort(Port::FiveVoltVbus, true));
  uint32_t buckAt = 0, portAt = 0;
  for (const auto &w : Wire.writes) {
    if (w.address == 0x20 && w.reg == 3 && (w.value & 0x10)) buckAt = w.at;
    if (w.address == 0x21 && w.reg == 3 && (w.value & 0x01)) portAt = w.at;
  }
  REQUIRE(buckAt > 0 && portAt >= buckAt + 100);
  REQUIRE(!board.setBuck5V(false));
  REQUIRE(Wire.internal.physicalOutputs() & 0x10);
  REQUIRE(board.setPort(Port::FiveVoltVbus, false));
  REQUIRE(board.setBuck5V(false));
  REQUIRE(!(Wire.internal.physicalOutputs() & 0x10));
  return true;
}

static bool missingBuckPgNeverEnablesJ8() {
  reset(); Stream log; BoardControl board;
  REQUIRE(board.begin(log));
  Wire.buckPgAvailable = false;
  Wire.writes.clear();
  REQUIRE(!board.setPort(Port::FiveVoltVbus, true));
  REQUIRE(!(Wire.external.physicalOutputs() & 0x01));
  REQUIRE(!(Wire.internal.physicalOutputs() & 0x10));
  for (const auto &w : Wire.writes)
    if (w.address == 0x21 && w.reg == 3) REQUIRE(!(w.value & 0x01));
  return true;
}

static bool shutdownDisablesCeBeforeBoost() {
  reset(); Stream log; BoardControl board;
  REQUIRE(board.begin(log));
  REQUIRE(board.setBoost(true));
  REQUIRE(board.setCharger(true));
  REQUIRE(!board.setBoost(false));
  Wire.writes.clear();
  REQUIRE(board.allOff());
  uint32_t ceAt = 0, boostAt = 0;
  for (const auto &w : Wire.writes) {
    if (w.address != 0x20 || w.reg != 3) continue;
    if (!(w.value & 0x02) && (w.value & 0x01)) ceAt = w.at;
    if (!(w.value & 0x01)) boostAt = w.at;
  }
  REQUIRE(ceAt > 0 && boostAt >= ceAt + 100);
  return true;
}

static bool intentionalCutoffBypassesOnlyTheBackupProtection() {
  reset(); Stream log; BoardControl board;
  REQUIRE(board.begin(log));
  REQUIRE(board.setBackup(true));
  gpio[11] = LOW;  // The armed bank now supplies the board.
  REQUIRE(!board.setBackup(false));
  REQUIRE(gpio[42] == HIGH);
  REQUIRE(board.allOff());
  REQUIRE(gpio[42] == HIGH);  // Ordinary cleanup must still preserve backup.
  REQUIRE(board.setStatusLed(true));
  Wire.internal.present = false;
  Wire.external.present = false;
  i2cCalls = 0;
  Wire.writes.clear();
  REQUIRE(board.releaseBackupForShutdown());
  REQUIRE(gpio[42] == LOW && gpio[4] == LOW);
  REQUIRE(i2cCalls == 0 && Wire.writes.empty());
  return true;
}

static bool intentionalCutoffWorksWithoutSuccessfulInitialization() {
  for (bool attemptBegin : {false, true}) {
    reset(); Stream log; BoardControl board;
    Wire.internal.present = Wire.external.present = false;
    if (attemptBegin) REQUIRE(!board.begin(log));
    gpio[42] = HIGH;
    gpio[4] = HIGH;
    gpio[11] = LOW;
    i2cCalls = 0;
    REQUIRE(board.releaseBackupForShutdown());
    REQUIRE(gpio[42] == LOW && gpio[4] == LOW);
    REQUIRE(i2cCalls == 0);
  }
  return true;
}

static bool intentionalCutoffReportsAStuckBackupEnable() {
  reset(); BoardControl board;
  gpio[42] = HIGH;
  forcedGpioHigh = uint64_t{1} << 42;
  REQUIRE(!board.releaseBackupForShutdown());
  REQUIRE(gpio[42] == LOW);  // Low was requested, but the physical pin is stuck.
  REQUIRE(std::string(board.error()).find("GPIO42") != std::string::npos);
  REQUIRE(i2cCalls == 0);
  return true;
}

static bool powerLossStopsChargingDespiteMissingExternalExpander() {
  reset(); Stream log; BoardControl board;
  REQUIRE(board.begin(log));
  REQUIRE(board.setBackup(true));
  REQUIRE(board.setPort(Port::FiveVoltVbus, true));
  REQUIRE(board.setBoost(true));
  REQUIRE(board.setChargeHighCurrent(true));
  REQUIRE(board.setThermistorOverride(true));
  REQUIRE(board.setCharger(true));
  gpio[11] = LOW;
  Wire.external.present = false;
  REQUIRE(!board.setCharger(false));  // Ordinary status validation cannot finish.
  REQUIRE(Wire.internal.physicalOutputs() & 0x02);
  Wire.writes.clear();
  REQUIRE(board.disableChargingForPowerLoss());
  REQUIRE(Wire.internal.physicalOutputs() == 0x1c);  // Buck and profile preserved.
  REQUIRE(Wire.external.physicalOutputs() == 0x01);
  REQUIRE(gpio[42] == HIGH);
  uint32_t ceAt = 0, boostAt = 0;
  for (const auto &w : Wire.writes) {
    REQUIRE(w.address == 0x20);
    if (w.reg != 3) continue;
    if (!(w.value & 0x02) && (w.value & 0x01)) ceAt = w.at;
    if (!(w.value & 0x01)) boostAt = w.at;
  }
  REQUIRE(ceAt > 0 && boostAt >= ceAt + 100);
  return true;
}

static bool powerLossUnknownInternalStateNeverWritesEnableBits() {
  reset(); Stream log; BoardControl board;
  REQUIRE(board.begin(log));
  REQUIRE(board.setBackup(true));
  REQUIRE(board.setBoost(true));
  REQUIRE(board.setCharger(true));
  Wire.internal.present = false;
  Wire.writes.clear();
  REQUIRE(!board.disableChargingForPowerLoss());
  REQUIRE(Wire.writes.empty());
  REQUIRE(Wire.internal.physicalOutputs() == 0x03);
  REQUIRE(gpio[42] == HIGH);
  REQUIRE(std::string(board.error()).find("0x20") != std::string::npos);
  return true;
}

static bool powerLossKeepsBoostAndProfileWhenCeCannotBeDisabled() {
  for (bool stuckLatch : {false, true}) {
    reset(); Stream log; BoardControl board;
    REQUIRE(board.begin(log));
    REQUIRE(board.setBackup(true));
    REQUIRE(board.setBoost(true));
    REQUIRE(board.setChargeHighCurrent(true));
    REQUIRE(board.setThermistorOverride(true));
    REQUIRE(board.setCharger(true));
    if (stuckLatch) Wire.internal.ignoredOutputWrites = 0x02;
    else Wire.internal.forcedHigh = 0x02;
    Wire.writes.clear();
    REQUIRE(!board.disableChargingForPowerLoss());
    REQUIRE((Wire.internal.physicalOutputs() & 0x0f) == 0x0f);
    REQUIRE(gpio[42] == HIGH);
    for (const auto &w : Wire.writes)
      if (w.reg == 3) REQUIRE((w.value & 0x0d) == 0x0d);
    REQUIRE(std::string(board.error()).find(stuckLatch ? "latch" : "physical pin") != std::string::npos);
  }
  return true;
}

static bool powerLossCompensatesPolarityAndIgnoresUnrelatedPinFault() {
  reset(); Stream log; BoardControl board;
  REQUIRE(board.begin(log));
  REQUIRE(board.setBoost(true));
  REQUIRE(board.setCharger(true));
  Wire.internal.reg[5] = 0xff;
  Wire.internal.forcedHigh = 0x04;  // Unrelated ISET pin fault cannot block CE off.
  REQUIRE(board.disableChargingForPowerLoss());
  REQUIRE((Wire.internal.physicalOutputs() & 0x03) == 0);
  REQUIRE(Wire.internal.physicalOutputs() & 0x04);
  REQUIRE((Wire.internal.reg[3] & 0x1f) == 0);
  return true;
}

static bool thermistorOverrideSwitchesAndPreservesProfile() {
  reset(); Stream log; BoardControl board;
  REQUIRE(board.begin(log));
  REQUIRE(board.setBoost(true));
  REQUIRE(board.setChargeHighCurrent(true));
  REQUIRE(board.setThermistorOverride(true));
  Status s;
  REQUIRE(board.readStatus(s));
  REQUIRE((s.internalOutputs & 0x1f) == 0x0d);
  REQUIRE((s.internalOutputInputs & 0x1f) == 0x0d);
  REQUIRE(board.setThermistorOverride(false));
  REQUIRE(board.readStatus(s));
  REQUIRE((s.internalOutputs & 0x1f) == 0x05);
  REQUIRE((s.internalOutputInputs & 0x1f) == 0x05);
  return true;
}

static bool thermistorSelectionRefusesEnabledCharger() {
  // Cover both the real-NTC and override charging states, in both directions.
  for (bool retainedOverride : {false, true}) {
    reset(); Stream log; BoardControl board;
    REQUIRE(board.begin(log));
    REQUIRE(board.setThermistorOverride(retainedOverride));
    REQUIRE(board.setBoost(true));
    REQUIRE(board.setCharger(true));
    Wire.writes.clear();
    REQUIRE(!board.setThermistorOverride(true));
    REQUIRE(!board.setThermistorOverride(false));
    REQUIRE(Wire.writes.empty());
    REQUIRE(std::string(board.error()).find("Disable charger") != std::string::npos);
    REQUIRE(bool(Wire.internal.physicalOutputs() & 0x08) == retainedOverride);
  }
  return true;
}

static bool thermistorSelectionWaitsAfterCeDisable() {
  for (bool selectOverride : {false, true}) {
    reset(); Stream log; BoardControl board;
    REQUIRE(board.begin(log));
    REQUIRE(board.setThermistorOverride(!selectOverride));
    REQUIRE(board.setBoost(true));
    REQUIRE(board.setCharger(true));
    REQUIRE(board.setCharger(false));
    const uint32_t ceVerifiedAt = millis();
    Wire.writes.clear();
    REQUIRE(board.setThermistorOverride(selectOverride));
    REQUIRE(Wire.writes.size() == 1);
    const auto &ts = Wire.writes.front();
    REQUIRE(ts.address == 0x20 && ts.reg == 3);
    REQUIRE(ts.at >= ceVerifiedAt + 100);
    REQUIRE(bool(ts.value & 0x08) == selectOverride);
    REQUIRE((ts.value & 0x03) == 0x01);  // Boost retained, CE remains low.
  }
  return true;
}

static bool thermistorSelectionWaitsForPostSwitchDeglitch() {
  for (bool selectOverride : {false, true}) {
    reset(); Stream log; BoardControl board;
    REQUIRE(board.begin(log));
    REQUIRE(board.setThermistorOverride(!selectOverride));
    REQUIRE(board.setBoost(true));
    Wire.writes.clear();
    REQUIRE(board.setThermistorOverride(selectOverride));
    const uint32_t returnedAt = millis();
    REQUIRE(Wire.writes.size() == 1);
    const auto selection = Wire.writes.front();
    REQUIRE(selection.address == 0x20 && selection.reg == 3);
    REQUIRE(bool(selection.value & 0x08) == selectOverride);
    REQUIRE(!(selection.value & 0x02));
    REQUIRE(returnedAt - selection.at >= (selectOverride ? 100u : 500u));
    // A caller may enable immediately on return; the select-to-CE delay must
    // already include the BQ24640 TS-in or TS-out interval and settling margin.
    REQUIRE(board.setCharger(true));
    const auto ce = Wire.writes.back();
    REQUIRE(ce.value & 0x02);
    REQUIRE(ce.at - selection.at >= (selectOverride ? 100u : 500u));
  }
  return true;
}

static bool thermistorSelectionRejectsConfigLatchAndPinFaults() {
  {
    reset(); Stream log; BoardControl board;
    REQUIRE(board.begin(log));
    Wire.internal.reg[7] |= 0x08;  // TS direction unexpectedly changed to input.
    Wire.writes.clear();
    REQUIRE(!board.setThermistorOverride(true));
    REQUIRE(Wire.writes.empty());
    REQUIRE(std::string(board.error()).find("direction mismatch") != std::string::npos);
  }
  {
    reset(); Stream log; BoardControl board;
    REQUIRE(board.begin(log));
    Wire.internal.ignoredOutputWrites = 0x08;
    REQUIRE(!board.setThermistorOverride(true));
    REQUIRE(std::string(board.error()).find("output latch mismatch") != std::string::npos);
  }
  for (bool selectOverride : {false, true}) {
    reset(); Stream log; BoardControl board;
    REQUIRE(board.begin(log));
    REQUIRE(board.setThermistorOverride(!selectOverride));
    // Prior pin level still matches the latch; only the requested change fails.
    Wire.internal.forcedHigh = selectOverride ? 0 : 0x08;
    Wire.internal.forcedLow = selectOverride ? 0x08 : 0;
    REQUIRE(!board.setThermistorOverride(selectOverride));
    REQUIRE(std::string(board.error()).find("enable pin readback") != std::string::npos);
  }
  return true;
}

static bool shutdownRestoresRealNtcAfterDecay() {
  reset(); Stream log; BoardControl board;
  REQUIRE(board.begin(log));
  REQUIRE(board.setThermistorOverride(true));
  REQUIRE(board.setBoost(true));
  REQUIRE(board.setCharger(true));
  Wire.writes.clear();
  REQUIRE(board.allOff());
  uint32_t ceAt = 0, tsAt = 0;
  for (const auto &w : Wire.writes) {
    if (w.address != 0x20 || w.reg != 3) continue;
    if (!(w.value & 0x02) && (w.value & 0x08)) ceAt = w.at;
    if (!(w.value & 0x08)) tsAt = w.at;
  }
  REQUIRE(ceAt > 0 && tsAt >= ceAt + 100);
  Status s;
  REQUIRE(board.readStatus(s));
  REQUIRE((s.internalOutputs & 0x0b) == 0);
  REQUIRE((s.internalOutputInputs & 0x0b) == 0);
  return true;
}

static bool beginClearsRetainedOverrideAfterCeDecay() {
  reset(); Stream log; BoardControl board;
  retain(Wire.internal, 0x0b);  // Boost, charger CE and TS bypass all retained high.
  now = 10;
  REQUIRE(board.begin(log));
  uint32_t ceAt = 0, tsAt = 0;
  for (const auto &w : Wire.writes) {
    if (w.address != 0x20 || w.reg != 3) continue;
    if (!(w.value & 0x02) && (w.value & 0x08)) ceAt = w.at;
    if (!(w.value & 0x08)) tsAt = w.at;
  }
  REQUIRE(ceAt > 0 && tsAt >= ceAt + 100);
  REQUIRE((Wire.internal.physicalOutputs() & 0x0b) == 0);
  return true;
}

static bool startQualifiedHold(ChargerHold &hold) {
  adcMv[2] = 2200;  // Fixture produces 20.24 V at the VCAP divider input.
  REQUIRE(hold.start());
  REQUIRE(hold.state() == ChargerHold::State::Starting);
  for (unsigned i = 0; i < 5; ++i) {
    delay(20);
    REQUIRE(hold.service());
  }
  REQUIRE(hold.state() == ChargerHold::State::Holding);
  return true;
}

static bool holdQualifiesThenRunsWithoutTimeout() {
  reset(); Stream log; BoardControl board;
  REQUIRE(board.begin(log));
  ChargerHold hold(board);
  REQUIRE(hold.start());
  REQUIRE(hold.state() == ChargerHold::State::Starting);
  REQUIRE(Wire.internal.physicalOutputs() == 0x0b);
  REQUIRE(Wire.external.physicalOutputs() == 0);
  REQUIRE(gpio[42] == LOW);
  adcMv[2] = 2200;
  delay(20); REQUIRE(hold.service());
  delay(80); REQUIRE(hold.service());
  REQUIRE(hold.state() == ChargerHold::State::Starting);
  delay(20); REQUIRE(hold.service());
  REQUIRE(hold.state() == ChargerHold::State::Holding);
  // Twelve minutes of regularly serviced virtual time, far beyond the bounded
  // charger characterization test, must leave CE and the bypass asserted.
  for (unsigned i = 0; i < 36000; ++i) {
    adcMv[2] = (i % 2) ? 2195 : 2205;
    delay(20);
    REQUIRE(hold.service());
  }
  REQUIRE(hold.state() == ChargerHold::State::Holding);
  REQUIRE(hold.samples() >= 36000);
  REQUIRE(hold.minVoltage() < 20.2f && hold.maxVoltage() > 20.28f);
  REQUIRE(Wire.internal.physicalOutputs() == 0x0b);
  REQUIRE(Wire.external.physicalOutputs() == 0);
  return true;
}

static bool holdQualificationRequiresContinuousRegulation() {
  reset(); Stream log; BoardControl board;
  REQUIRE(board.begin(log));
  ChargerHold hold(board);
  adcMv[2] = 2200;
  REQUIRE(hold.start());
  delay(80); REQUIRE(hold.service());
  adcMv[2] = 1000;
  delay(20); REQUIRE(hold.service());
  adcMv[2] = 2200;
  delay(20); REQUIRE(hold.service());
  delay(80); REQUIRE(hold.service());
  REQUIRE(hold.state() == ChargerHold::State::Starting);
  delay(20); REQUIRE(hold.service());
  REQUIRE(hold.state() == ChargerHold::State::Holding);
  return true;
}

static bool holdManualStopStaysStopped() {
  reset(); Stream log; BoardControl board;
  REQUIRE(board.begin(log));
  ChargerHold hold(board);
  REQUIRE(startQualifiedHold(hold));
  REQUIRE(hold.stop());
  REQUIRE(hold.state() == ChargerHold::State::Stopped);
  REQUIRE(!hold.active());
  Wire.writes.clear();
  delay(60000);
  REQUIRE(hold.service());
  REQUIRE(hold.state() == ChargerHold::State::Stopped);
  REQUIRE(Wire.writes.empty());
  REQUIRE(Wire.internal.physicalOutputs() == 0);
  REQUIRE(Wire.external.physicalOutputs() == 0);
  return true;
}

static bool holdOvervoltageLatchesOff() {
  reset(); Stream log; BoardControl board;
  REQUIRE(board.begin(log));
  ChargerHold hold(board);
  REQUIRE(startQualifiedHold(hold));
  adcMv[2] = 2400;  // 22.08 V, exceeding the software screening limit.
  delay(20);
  REQUIRE(!hold.service());
  REQUIRE(hold.state() == ChargerHold::State::Fault);
  REQUIRE(hold.cleanupOk());
  REQUIRE(std::string(hold.error()).find("overvoltage") != std::string::npos);
  REQUIRE(Wire.internal.physicalOutputs() == 0);
  adcMv[2] = 2200;
  Wire.writes.clear();
  delay(60000);
  REQUIRE(!hold.service());
  REQUIRE(Wire.writes.empty());  // A recovered reading must not restart charging.
  return true;
}

static bool holdSupplyAndStatusSignalsTrip() {
  // Independently lose USB input PG, charger PG, boost PG, backed 5 V PG,
  // charger STAT, or the selected main source after successful qualification.
  for (uint8_t lost : {0x04, 0x08, 0x40, 0x80, 0x10, 0x00}) {
    reset(); Stream log; BoardControl board;
    REQUIRE(board.begin(log));
    ChargerHold hold(board);
    REQUIRE(startQualifiedHold(hold));
    Wire.automaticPg = false;
    Wire.internalInput = 0xdc & ~lost;
    if (!lost) gpio[11] = LOW;
    delay(20);
    REQUIRE(!hold.service());
    REQUIRE(hold.state() == ChargerHold::State::Fault);
    REQUIRE(hold.cleanupOk());
    REQUIRE(Wire.internal.physicalOutputs() == 0);
    REQUIRE(Wire.external.physicalOutputs() == 0);
  }
  return true;
}

static bool holdReadFailureStillDisablesCe() {
  reset(); Stream log; BoardControl board;
  REQUIRE(board.begin(log));
  ChargerHold hold(board);
  REQUIRE(startQualifiedHold(hold));
  Wire.external.present = false;
  delay(20);
  REQUIRE(!hold.service());
  REQUIRE(hold.state() == ChargerHold::State::Fault);
  REQUIRE(!hold.cleanupOk());
  REQUIRE(std::string(hold.error()).find("0x21") != std::string::npos);
  REQUIRE(!(Wire.internal.physicalOutputs() & 0x02));
  Wire.external.present = true;
  delay(20);
  REQUIRE(!hold.service());
  REQUIRE(!(Wire.internal.physicalOutputs() & 0x02));
  REQUIRE(hold.stop());  // Explicit recovery completes the remaining cleanup.
  REQUIRE(Wire.internal.physicalOutputs() == 0);
  return true;
}

static bool holdAllowsOpenRealThermistorWhileOverridden() {
  reset(); Stream log; BoardControl board;
  REQUIRE(board.begin(log));
  adcMv[1] = 2909;  // GPIO1 observes the open physical sensor, not U14's output.
  ChargerHold hold(board);
  REQUIRE(startQualifiedHold(hold));
  Status s;
  REQUIRE(board.readStatus(s));
  REQUIRE(s.tsMv == 2909);
  REQUIRE(s.internalOutputs == 0x0b);
  REQUIRE(s.chargerStat);
  delay(1000);
  REQUIRE(hold.service());
  REQUIRE(hold.state() == ChargerHold::State::Holding);
  return true;
}

static bool holdStartupUndervoltageTimesOut() {
  reset(); Stream log; BoardControl board;
  REQUIRE(board.begin(log));
  adcMv[2] = 67;  // Approximately 0.62 V, matching inhibited-rail observation.
  ChargerHold hold(board);
  REQUIRE(hold.start());
  REQUIRE(hold.state() == ChargerHold::State::Starting);
  delay(1980);
  REQUIRE(hold.service());
  REQUIRE(hold.state() == ChargerHold::State::Starting);
  delay(20);
  REQUIRE(!hold.service());
  REQUIRE(hold.state() == ChargerHold::State::Fault);
  REQUIRE(hold.cleanupOk());
  REQUIRE(std::string(hold.error()).find("timeout") != std::string::npos);
  REQUIRE(Wire.internal.physicalOutputs() == 0);
  return true;
}

int main() {
  const std::pair<const char *, bool (*)()> cases[] = {
    {"missing U43 disables retained CE", missingExternalDisablesCharger},
    {"missing U26 disables retained exterior outputs", missingInternalDisablesExternal},
    {"stuck CE preserves boost/profile", stuckCePreservesBoostAndProfile},
    {"retained input polarity reconciliation", retainedPolarityIsReconciled},
    {"schematic input one-hot mapping", inputOneHotMapping},
    {"J8 buck prerequisite and shutdown guard", j8StartsBuckFirstAndGuardsShutdown},
    {"missing buck PG prevents J8 activation", missingBuckPgNeverEnablesJ8},
    {"CE-before-boost shutdown with decay delay", shutdownDisablesCeBeforeBoost},
    {"intentional cutoff releases backup while ordinary cleanup preserves it", intentionalCutoffBypassesOnlyTheBackupProtection},
    {"intentional cutoff works without begin or after failed initialization", intentionalCutoffWorksWithoutSuccessfulInitialization},
    {"intentional cutoff detects a stuck physical backup enable", intentionalCutoffReportsAStuckBackupEnable},
    {"power-loss charging shutdown works despite missing U43 and preserves backup/loads", powerLossStopsChargingDespiteMissingExternalExpander},
    {"unknown U26 state causes no enable writes during power-loss cleanup", powerLossUnknownInternalStateNeverWritesEnableBits},
    {"power-loss cleanup preserves boost/profile if CE latch or pin remains high", powerLossKeepsBoostAndProfileWhenCeCannotBeDisabled},
    {"power-loss cleanup handles polarity and unrelated internal pin faults", powerLossCompensatesPolarityAndIgnoresUnrelatedPinFault},
    {"thermistor override on/off readback preserves other outputs", thermistorOverrideSwitchesAndPreservesProfile},
    {"thermistor selection refused while CE high", thermistorSelectionRefusesEnabledCharger},
    {"thermistor selection waits for CE decay", thermistorSelectionWaitsAfterCeDisable},
    {"thermistor selection waits for TS-in/TS-out deglitch before CE", thermistorSelectionWaitsForPostSwitchDeglitch},
    {"thermistor config/latch/physical pin fault detection", thermistorSelectionRejectsConfigLatchAndPinFaults},
    {"allOff restores real NTC after CE decay", shutdownRestoresRealNtcAfterDecay},
    {"begin clears retained bypass after CE decay", beginClearsRetainedOverrideAfterCeDecay},
    {"hold qualifies regulation and remains on without a duration limit", holdQualifiesThenRunsWithoutTimeout},
    {"hold qualification requires continuously valid regulation", holdQualificationRequiresContinuousRegulation},
    {"hold manual stop remains stopped", holdManualStopStaysStopped},
    {"hold overvoltage shutdown remains latched", holdOvervoltageLatchesOff},
    {"hold source/PG/STAT failures shut down", holdSupplyAndStatusSignalsTrip},
    {"hold read failure disables CE and reports incomplete cleanup", holdReadFailureStillDisablesCe},
    {"hold permits open real thermistor with fixed TS selected", holdAllowsOpenRealThermistorWhileOverridden},
    {"hold startup undervoltage times out and disables outputs", holdStartupUndervoltageTimesOut},
  };
  unsigned failures = 0;
  for (const auto &test : cases) {
    const bool ok = test.second();
    std::cout << (ok ? "PASS " : "FAIL ") << test.first << '\n';
    failures += !ok;
  }
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
