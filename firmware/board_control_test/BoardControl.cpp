#include "BoardControl.h"

#include <initializer_list>
#include <stdarg.h>
#include <stdio.h>
#include <driver/gpio.h>

namespace {
constexpr uint8_t kInternal = 0x20, kExternal = 0x21;
constexpr uint8_t kDefinedOutputs = 0x1f, kUnusedInputs = 0xe0;
constexpr uint8_t kBoost = 1u << 0, kCharger = 1u << 1;
constexpr uint8_t kHighCurrent = 1u << 2, kTsOverride = 1u << 3, kBuck = 1u << 4;
constexpr int kBackupPin = 42, kLedPin = 4, kMuxPin = 11;
constexpr uint32_t kChargerDecayMs = 100, kStablePgMs = 100;
constexpr uint32_t kTsOverrideSettleMs = 100, kTsNtcSettleMs = 500;
bool isBitSet(uint8_t value, uint8_t position) { return (value & (1u << position)) != 0; }
}

bool BoardControl::fail(const char *format, ...) {
  // Keep the originating error if a subsequent best-effort rollback also fails.
  if (!error_[0]) {
    va_list args;
    va_start(args, format);
    vsnprintf(error_, sizeof(error_), format, args);
    va_end(args);
  }
  return false;
}

bool BoardControl::requireReady() {
  return ready_ || fail("BoardControl.begin() has not completed successfully");
}

bool BoardControl::readPair(uint8_t address, uint8_t reg, uint8_t *values) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  const uint8_t result = Wire.endTransmission(false);
  if (result) return fail("I2C 0x%02X register 0x%02X select failed (%u)", address, reg, result);
  const size_t count = Wire.requestFrom(address, static_cast<uint8_t>(2));
  if (count != 2 || Wire.available() != 2) {
    while (Wire.available()) Wire.read();
    return fail("I2C 0x%02X register 0x%02X short read (%u/2)", address, reg, unsigned(count));
  }
  values[0] = Wire.read();
  values[1] = Wire.read();
  return true;
}

bool BoardControl::writeRegister(uint8_t address, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(value);
  const uint8_t result = Wire.endTransmission();
  return result == 0 || fail("I2C 0x%02X register 0x%02X write failed (%u)", address, reg, result);
}

bool BoardControl::readRegisters(uint8_t address, Registers &r) {
  // TCA9535 increments only within each two-register pair, not through 0..7.
  return readPair(address, 0, r.input) && readPair(address, 2, r.output) &&
         readPair(address, 4, r.polarity) && readPair(address, 6, r.config);
}

bool BoardControl::validate(uint8_t address, const Registers &r, uint8_t expected) {
  if (r.config[0] != 0xff || r.config[1] != kUnusedInputs)
    return fail("0x%02X direction mismatch: %02X/%02X expected FF/E0", address, r.config[0], r.config[1]);
  if (r.polarity[0] != 0 || r.polarity[1] != 0)
    return fail("0x%02X unexpected input polarity: %02X/%02X", address, r.polarity[0], r.polarity[1]);
  if (r.output[0] != 0 || r.output[1] != expected)
    return fail("0x%02X output latch mismatch: %02X/%02X expected 00/%02X", address, r.output[0], r.output[1], expected);
  if ((r.input[1] & kDefinedOutputs) != (expected & kDefinedOutputs))
    return fail("0x%02X enable pin readback %02X differs from latch %02X (mask 1F)", address, r.input[1], expected);
  return true;
}

bool BoardControl::failInitialization() {
  char originalError[sizeof(error_)];
  snprintf(originalError, sizeof(originalError), "%s", error_);
  clearError();
  ready_ = false;
  uint8_t outputs[2], config[2], polarity[2], inputs[2];
  const bool internalKnown = readPair(kInternal, 2, outputs);
  bool ceLow = false;
  if (internalKnown) {
    // Read before writing: never enable boost merely to establish a shutdown
    // sequence when the actual retained output state is unknown.
    internalOutputs_ = outputs[1];
    if (writeRegister(kInternal, 3, outputs[1] & ~kCharger)) {
      internalOutputs_ &= ~kCharger;
      ceLow = readPair(kInternal, 6, config) && readPair(kInternal, 4, polarity) &&
              writeRegister(kInternal, 7, (config[1] | kUnusedInputs) & ~kCharger) &&
              readPair(kInternal, 0, inputs) &&
              (((inputs[1] ^ polarity[1]) & kCharger) == 0);
      if (ceLow) chargerDisabledAt_ = millis();
    }
  }

  // Recover the external expander independently even if U26 is unreachable.
  // Every write is bounded by the Wire timeout; there are no retries or loops.
  bool externalOff = writeRegister(kExternal, 3, 0) && writeRegister(kExternal, 2, 0) &&
                     writeRegister(kExternal, 6, 0xff) && writeRegister(kExternal, 7, kUnusedInputs) &&
                     writeRegister(kExternal, 4, 0) && writeRegister(kExternal, 5, 0);
  Registers verified;
  if (externalOff) {
    externalOutputs_ = 0;
    externalOff = readRegisters(kExternal, verified) && validate(kExternal, verified, 0);
  }
  bool internalOff = false;
  if (ceLow) {
    waitForChargerDecay();
    internalOff = writeRegister(kInternal, 3, 0) && writeRegister(kInternal, 2, 0) &&
                  writeRegister(kInternal, 6, 0xff) && writeRegister(kInternal, 7, kUnusedInputs) &&
                  writeRegister(kInternal, 4, 0) && writeRegister(kInternal, 5, 0);
    if (internalOff) {
      internalOutputs_ = 0;
      internalOff = readRegisters(kInternal, verified) && validate(kInternal, verified, 0);
    }
  }
  const char *internalResult = internalOff ? "OFF verified" : internalKnown ? "shutdown incomplete" : "UNKNOWN, unchanged";
  const char *externalResult = externalOff ? "OFF verified" : "shutdown incomplete";
  snprintf(error_, sizeof(error_), "%.65s; cleanup U26=%s U43=%s", originalError, internalResult, externalResult);
  if (log_) log_->println(error_);
  return false;
}

bool BoardControl::begin(Stream &log) {
  clearError();
  log_ = &log;
  ready_ = false;
  backupEnabled_ = false;
  // Arduino-ESP32 3.x ignores digitalWrite until pinMode has attached the GPIO.
  // Set the hardware latch directly first so output-direction changes stay low.
  gpio_set_level(static_cast<gpio_num_t>(kBackupPin), 0);
  pinMode(kBackupPin, OUTPUT);
  digitalWrite(kBackupPin, LOW);
  gpio_set_level(static_cast<gpio_num_t>(kLedPin), 0);
  pinMode(kLedPin, OUTPUT);
  digitalWrite(kLedPin, LOW);
  pinMode(kMuxPin, INPUT);
  pinMode(15, INPUT);
  pinMode(16, INPUT);
  analogReadResolution(12);
  const int adcPins[] = {1, 2, 3, 5, 6, 7, 8, 9, 10};
  for (int pin : adcPins) analogSetPinAttenuation(pin, ADC_11db);
  if (!Wire.begin(17, 18, 100000)) return fail("Control I2C initialization failed; retained expander outputs UNKNOWN");
  Wire.setTimeOut(50);

  Registers internal, external;
  if (!readRegisters(kInternal, internal) || !readRegisters(kExternal, external)) return failInitialization();
  for (uint8_t address : {kInternal, kExternal}) {
    const Registers &r = address == kInternal ? internal : external;
    char line[160];
    snprintf(line, sizeof(line),
             "RETAINED 0x%02X input=%02X/%02X output=%02X/%02X polarity=%02X/%02X config=%02X/%02X",
             address, r.input[0], r.input[1], r.output[0], r.output[1],
             r.polarity[0], r.polarity[1], r.config[0], r.config[1]);
    log_->println(line);
  }
  internalOutputs_ = internal.output[1];
  externalOutputs_ = external.output[1];

  // Reset can retain live expander outputs. Assert CE low before changing boost
  // or charger profile. Write the CE latch low before configuring its direction.
  if (!writeRegister(kInternal, 6, 0xff) || !writeRegister(kExternal, 6, 0xff)) return failInitialization();
  if (!writeRegister(kInternal, 3, internalOutputs_ & ~kCharger)) return failInitialization();
  internalOutputs_ &= ~kCharger;
  if (!writeRegister(kInternal, 7, (internal.config[1] | kUnusedInputs) & ~kCharger)) return failInitialization();
  uint8_t cePins[2];
  if (!readPair(kInternal, 0, cePins)) return failInitialization();
  if ((cePins[1] ^ internal.polarity[1]) & kCharger) {
    fail("Retained charger CE pin did not go low; boost left unchanged");
    return failInitialization();
  }
  chargerDisabledAt_ = millis();
  waitForChargerDecay();

  // Every output latch is safe before converting any remaining pins to outputs.
  for (uint8_t address : {kExternal, kInternal}) {
    if (!writeRegister(address, 2, 0) || !writeRegister(address, 3, 0) ||
        !writeRegister(address, 6, 0xff) || !writeRegister(address, 7, kUnusedInputs) ||
        !writeRegister(address, 4, 0) || !writeRegister(address, 5, 0)) return failInitialization();
  }
  internalOutputs_ = externalOutputs_ = 0;
  delay(2);
  if (!readRegisters(kInternal, internal) || !readRegisters(kExternal, external) ||
      !validate(kInternal, internal, 0) || !validate(kExternal, external, 0)) return failInitialization();
  ready_ = true;
  log_->println("SAFE baseline: CE, boost, unbacked buck, external switches, backup, button and status LED off; real NTC, low charge current.");
  return true;
}

bool BoardControl::readStatus(Status &status) {
  clearError();
  return requireReady() && readStatusImpl(status);
}

bool BoardControl::readStatusImpl(Status &status) {
  Registers internal, external;
  if (!readRegisters(kInternal, internal) || !readRegisters(kExternal, external) ||
      !validate(kInternal, internal, internalOutputs_) ||
      !validate(kExternal, external, externalOutputs_)) return false;
  Status next;
  next.internalInputs = internal.input[0];
  next.internalOutputInputs = internal.input[1];
  next.internalOutputs = internal.output[1];
  next.externalInputs = external.input[0];
  next.externalOutputInputs = external.input[1];
  next.externalOutputs = external.output[1];
  // These mappings were traced through the input divider resistors in the fresh
  // KiCad netlist; the previous prose pin tables had several PG inputs swapped.
  next.dcPg = isBitSet(internal.input[0], 0);
  next.buckPg = isBitSet(internal.input[0], 1);
  next.usbPg = isBitSet(internal.input[0], 2);
  next.chargerPg = isBitSet(internal.input[0], 3);
  next.chargerStat = isBitSet(internal.input[0], 4);
  next.backupPg = isBitSet(internal.input[0], 5);
  next.boostPg = isBitSet(internal.input[0], 6);
  next.ssBuckPg = isBitSet(internal.input[0], 7);
  next.jetsonOn = isBitSet(external.input[0], 0);
  next.ext5vSsPg = isBitSet(external.input[0], 1);
  next.extSsPg = isBitSet(external.input[0], 2);
  next.extVbusPg = isBitSet(external.input[0], 3);
  next.ext5vVbusPg = isBitSet(external.input[0], 4);
  next.mainSelected = digitalRead(kMuxPin) == HIGH;
  next.vcapV = readAdcMv(2) * (9.2f / 1000.0f);
  next.tsMv = readAdcMv(1);
  status = next;
  return true;
}

uint32_t BoardControl::readAdcMv(int pin) {
  if (!(pin == 1 || pin == 2 || pin == 3 || (pin >= 5 && pin <= 10))) {
    fail("GPIO%d is not one of the board's analog input pins", pin);
    return UINT32_MAX;
  }
  uint32_t sum = 0;
  for (unsigned sample = 0; sample < 16; ++sample) sum += analogReadMilliVolts(pin);
  return (sum + 8) / 16;
}

bool BoardControl::mainPresent(const Status &s) const {
  return s.mainSelected && (s.usbPg || s.dcPg);
}

bool BoardControl::writeOutputs(uint8_t address, uint8_t value) {
  if (!writeRegister(address, 3, value)) return false;
  if (address == kInternal) internalOutputs_ = value;
  else externalOutputs_ = value;
  delay(1);
  Registers r;
  return readRegisters(address, r) && validate(address, r, value);
}

void BoardControl::waitForChargerDecay() {
  const uint32_t elapsed = millis() - chargerDisabledAt_;
  if (elapsed < kChargerDecayMs) delay(kChargerDecayMs - elapsed);
}

bool BoardControl::waitPg(Pg pg, bool requireMain, uint32_t timeoutMs) {
  const uint32_t started = millis();
  uint32_t goodSince = 0;
  bool wasGood = false;
  while (millis() - started < timeoutMs) {
    Status s;
    if (!readStatusImpl(s)) return false;
    if (requireMain && !mainPresent(s)) return fail("Main source lost while awaiting PG");
    bool good = false;
    switch (pg) {
      case Pg::Boost: good = s.boostPg; break;
      case Pg::Buck: good = s.buckPg; break;
      case Pg::Vbus: good = s.extVbusPg; break;
      case Pg::FiveVoltVbus: good = s.ext5vVbusPg && s.buckPg; break;
      case Pg::Supervised: good = s.extSsPg; break;
      case Pg::FiveVoltSupervised: good = s.ext5vSsPg && s.ssBuckPg; break;
      case Pg::Backup: good = s.backupPg; break;
    }
    if (good) {
      if (!wasGood) goodSince = millis();
      if (millis() - goodSince >= kStablePgMs) return true;
    }
    wasGood = good;
    delay(10);
  }
  return fail("PG %u did not remain high for %lu ms within %lu ms", unsigned(pg),
              static_cast<unsigned long>(kStablePgMs), static_cast<unsigned long>(timeoutMs));
}

bool BoardControl::setBoost(bool enabled) {
  clearError();
  Status s;
  if (!requireReady() || !readStatusImpl(s)) return false;
  if (!enabled) {
    if (internalOutputs_ & kCharger) return fail("Disable charger before disabling boost");
    waitForChargerDecay();
    return writeOutputs(kInternal, internalOutputs_ & ~kBoost);
  }
  if (!mainPresent(s)) return fail("Boost requires a valid main source and main mux selection");
  if (!(internalOutputs_ & kBoost) && (internalOutputs_ & kCharger))
    return fail("Disable charger before starting boost");
  if (!writeOutputs(kInternal, internalOutputs_ | kBoost)) return false;
  if (waitPg(Pg::Boost, true)) return true;
  // A failed re-validation of an already charging boost must shut CE down first.
  if (writeOutputs(kInternal, internalOutputs_ & ~kCharger)) {
    chargerDisabledAt_ = millis();
    waitForChargerDecay();
    writeOutputs(kInternal, internalOutputs_ & ~kBoost);
  }
  return false;
}

bool BoardControl::setBuck5V(bool enabled) {
  clearError();
  return requireReady() && setBuckImpl(enabled);
}

bool BoardControl::setBuckImpl(bool enabled) {
  Status s;
  if (!readStatusImpl(s)) return false;
  if (!enabled) {
    if (externalOutputs_ & 1u) return fail("Disable J8 5V VBUS port before disabling its buck");
    return writeOutputs(kInternal, internalOutputs_ & ~kBuck);
  }
  if (!mainPresent(s)) return fail("5V VBUS buck requires a valid main source");
  if (!writeOutputs(kInternal, internalOutputs_ | kBuck)) return false;
  if (waitPg(Pg::Buck, true)) return true;
  // If this buck was already supplying J8, disconnect that port before rollback.
  if (writeOutputs(kExternal, externalOutputs_ & ~1u))
    writeOutputs(kInternal, internalOutputs_ & ~kBuck);
  return false;
}

bool BoardControl::setCharger(bool enabled) {
  clearError();
  return requireReady() && setChargerImpl(enabled);
}

bool BoardControl::setChargerImpl(bool enabled) {
  Status s;
  if (!readStatusImpl(s)) return false;
  if (enabled) {
    if (!mainPresent(s) || !(internalOutputs_ & kBoost) || !s.boostPg)
      return fail("Charger requires enabled boost, boost PG and a valid main source");
    if (s.vcapV > 21.0f) return fail("Charger inhibited: measured VCAP exceeds 21 V");
  }
  if (!writeOutputs(kInternal, enabled ? internalOutputs_ | kCharger : internalOutputs_ & ~kCharger)) return false;
  if (!enabled) chargerDisabledAt_ = millis();
  // SCC_PG describes valid charger input, not CE/charging; STAT also depends on TS
  // and bank state. The caller must interpret these separately from CE readback.
  return true;
}

bool BoardControl::setChargeHighCurrent(bool enabled) {
  clearError();
  Status s;
  if (!requireReady() || !readStatusImpl(s)) return false;
  if (internalOutputs_ & kCharger) return fail("Disable charger before changing charge current");
  waitForChargerDecay();
  return writeOutputs(kInternal, enabled ? internalOutputs_ | kHighCurrent : internalOutputs_ & ~kHighCurrent);
}

bool BoardControl::setThermistorOverride(bool enabled) {
  clearError();
  Status s;
  if (!requireReady() || !readStatusImpl(s)) return false;
  if (internalOutputs_ & kCharger) return fail("Disable charger before changing thermistor selection");
  waitForChargerDecay();
  if (!writeOutputs(kInternal, enabled ? internalOutputs_ | kTsOverride : internalOutputs_ & ~kTsOverride))
    return false;
  // BQ24640 TS-in deglitch is 20 ms; TS-out is 400 ms. Include RC/margin
  // before returning, so an immediate CE command cannot use the old TS state.
  // GPIO1 senses the real NTC branch and cannot verify the selected TS voltage.
  delay(enabled ? kTsOverrideSettleMs : kTsNtcSettleMs);
  return true;
}

bool BoardControl::setPort(Port port, bool enabled) {
  clearError();
  uint8_t mask;
  Pg pg;
  switch (port) {
    case Port::Vbus: mask = 1u << 1; pg = Pg::Vbus; break;
    case Port::FiveVoltVbus: mask = 1u << 0; pg = Pg::FiveVoltVbus; break;
    case Port::Supervised: mask = 1u << 2; pg = Pg::Supervised; break;
    case Port::FiveVoltSupervised: mask = 1u << 3; pg = Pg::FiveVoltSupervised; break;
    default: return fail("Unknown external port");
  }
  Status s;
  if (!requireReady() || !readStatusImpl(s)) return false;
  if (!enabled) return writeOutputs(kExternal, externalOutputs_ & ~mask);
  const bool unbacked = port == Port::Vbus || port == Port::FiveVoltVbus;
  if (unbacked && !mainPresent(s)) return fail("Unbacked port requires a valid main source");
  if (!unbacked && (!s.ssBuckPg || (!mainPresent(s) && !(!s.mainSelected && s.backupPg))))
    return fail("Backed port requires backed buck PG and a valid selected source");
  const bool startBuck = port == Port::FiveVoltVbus && !(internalOutputs_ & kBuck);
  if (port == Port::FiveVoltVbus && !setBuckImpl(true)) return false;
  if (!writeOutputs(kExternal, externalOutputs_ | mask)) return false;
  if (waitPg(pg, unbacked)) return true;
  if (writeOutputs(kExternal, externalOutputs_ & ~mask) && startBuck)
    writeOutputs(kInternal, internalOutputs_ & ~kBuck);
  return false;
}

bool BoardControl::setBackup(bool enabled) {
  clearError();
  Status s;
  if (!requireReady() || !readStatusImpl(s)) return false;
  if (!enabled) {
    if (backupEnabled_ && !mainPresent(s)) return fail("Refusing to release backup without validated main power");
  } else {
    if (!mainPresent(s)) return fail("Arm backup only while main power is valid");
    if (s.vcapV < 6.0f || s.vcapV > 21.0f) return fail("Backup requires measured VCAP within 6..21 V (%.3f V)", s.vcapV);
  }
  digitalWrite(kBackupPin, enabled ? HIGH : LOW);
  backupEnabled_ = enabled;
  if (digitalRead(kBackupPin) != (enabled ? HIGH : LOW)) return fail("GPIO42 backup enable readback mismatch");
  if (!enabled || waitPg(Pg::Backup, true)) return true;
  // Preserve the arm if main disappeared during the wait, since it may now be the
  // only power path. A status error is also uncertainty, not permission to cut it.
  if (readStatusImpl(s) && mainPresent(s)) {
    digitalWrite(kBackupPin, LOW);
    backupEnabled_ = false;
  }
  return false;
}

bool BoardControl::releaseBackupForShutdown() {
  clearError();
  // No I2C and no source/status reads: this deliberately releases even the last
  // power path. The caller must persist its reason before entering this method.
  gpio_set_level(static_cast<gpio_num_t>(kLedPin), 0);
  pinMode(kLedPin, OUTPUT);
  digitalWrite(kLedPin, LOW);
  backupEnabled_ = false;
  gpio_set_level(static_cast<gpio_num_t>(kBackupPin), 0);
  pinMode(kBackupPin, OUTPUT);
  digitalWrite(kBackupPin, LOW);
  return digitalRead(kBackupPin) == LOW || fail("Final shutdown: GPIO42 did not release backup");
}

bool BoardControl::disableChargingForPowerLoss() {
  clearError();
  uint8_t outputs[2], config[2];
  // Read the actual latch even if normal begin/status validation failed. Never
  // reconstruct unknown retained outputs in a way that could turn boost on.
  if (!readPair(kInternal, 2, outputs)) return false;
  if (!writeRegister(kInternal, 3, outputs[1] & ~kCharger)) return false;
  internalOutputs_ = outputs[1] & ~kCharger;
  if (!readPair(kInternal, 6, config) ||
      !writeRegister(kInternal, 7, (config[1] | kUnusedInputs) & ~kCharger)) return false;

  // Check only the controls being made safe. An unavailable U43 or an unrelated
  // ISET/TS pin fault must not prevent responsive U26 from disabling charging.
  auto verifyLow = [&](uint8_t mask) {
    uint8_t actual[2], inputs[2], polarity[2];
    if (!readPair(kInternal, 2, actual) || !readPair(kInternal, 4, polarity) ||
        !readPair(kInternal, 0, inputs)) return false;
    internalOutputs_ = actual[1];
    if (actual[1] & mask)
      return fail("Power-loss shutdown: U26 latch %02X remains high (mask %02X)", actual[1], mask);
    if ((inputs[1] ^ polarity[1]) & mask)
      return fail("Power-loss shutdown: U26 physical pin remains high (mask %02X)", mask);
    return true;
  };
  if (!verifyLow(kCharger)) return false;
  chargerDisabledAt_ = millis();
  waitForChargerDecay();
  // Recheck CE after decay and preserve the current profile/buck latch values.
  if (!verifyLow(kCharger)) return false;
  if (!writeRegister(kInternal, 3, internalOutputs_ & ~(kCharger | kBoost))) return false;
  internalOutputs_ &= ~(kCharger | kBoost);
  if (!readPair(kInternal, 6, config) ||
      !writeRegister(kInternal, 7, (config[1] | kUnusedInputs) & ~(kCharger | kBoost))) return false;
  return verifyLow(kCharger | kBoost);
}

bool BoardControl::setStatusLed(bool enabled) {
  clearError();
  if (!requireReady()) return false;
  digitalWrite(kLedPin, enabled ? HIGH : LOW);
  return digitalRead(kLedPin) == (enabled ? HIGH : LOW) || fail("GPIO4 status LED drive readback mismatch");
}

bool BoardControl::setJetsonButton(bool pressed) {
  clearError();
  Status s;
  if (!requireReady() || !readStatusImpl(s)) return false;
  return writeOutputs(kExternal, pressed ? externalOutputs_ | (1u << 4) : externalOutputs_ & ~(1u << 4));
}

bool BoardControl::allOff() {
  clearError();
  if (!requireReady()) return false;
  // This recovery path deliberately attempts safe writes even if a prior status
  // check failed. Never drop boost unless the CE-low write/readback succeeded.
  const bool chargerOff = writeOutputs(kInternal, internalOutputs_ & ~kCharger);
  if (chargerOff) chargerDisabledAt_ = millis();
  const bool portsOff = writeOutputs(kExternal, 0);
  bool convertersOff = false;
  if (chargerOff && portsOff) {
    waitForChargerDecay();
    convertersOff = writeOutputs(kInternal, 0);
  }
  digitalWrite(kLedPin, LOW);
  Status s;
  const bool statusValid = readStatusImpl(s);
  if (statusValid && mainPresent(s)) {
    digitalWrite(kBackupPin, LOW);
    backupEnabled_ = false;
  } else if (backupEnabled_ && log_) {
    log_->println("SAFE shutdown preserved armed backup: main power is absent or status is uncertain.");
  }
  return chargerOff && portsOff && convertersOff && statusValid;
}
