#include "BoardHardware.h"

#include <driver/gpio.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>

namespace {
constexpr uint8_t kInternal = 0x20, kExternal = 0x21;
constexpr uint8_t kBoost = 0x01, kCe = 0x02, kHighCurrent = 0x04, kTsOverride = 0x08, kBuck = 0x10;
constexpr uint8_t kDefined = 0x1f, kUnusedInputs = 0xe0;
constexpr int kLed = 4, kBackup = 42, kMux = 11, kBoot = 0;
constexpr uint32_t kDecayMs = 100, kTsSettleMs = 500, kFreshMs = 350;
constexpr uint8_t kAdcPins[] = {1, 2, 3, 5, 6, 7, 8, 9, 10};
constexpr const char *kAdcNames[] = {"TS", "VCAP", "IMON_DC", "IMON_J8", "IMON_J7", "IMON_J9", "IMON_J10", "IMON_USB", "IMON_BACKUP"};
bool high(uint8_t value, unsigned position) { return (value & (1u << position)) != 0; }
}

const char *BoardHardware::adcName(unsigned index) { return index < 9 ? kAdcNames[index] : "INVALID"; }

bool BoardHardware::fail(const char *format, ...) {
  if (!error_[0]) {
    va_list args;
    va_start(args, format);
    vsnprintf(error_, sizeof(error_), format, args);
    va_end(args);
  }
  return false;
}

bool BoardHardware::readPair(uint8_t address, uint8_t reg, uint8_t values[2]) {
  // Selecting the register address is necessary for a read. This sends no
  // register DATA and does not change outputs, polarity or configuration.
  Wire.beginTransmission(address);
  Wire.write(reg);
  const uint8_t code = Wire.endTransmission(false);
  if (code) return fail("I2C %02X register %02X select failed (%u)", address, reg, code);
  const size_t count = Wire.requestFrom(address, uint8_t(2));
  if (count != 2 || Wire.available() != 2) {
    while (Wire.available()) Wire.read();
    return fail("I2C %02X register %02X short read (%u)", address, reg, unsigned(count));
  }
  values[0] = Wire.read();
  values[1] = Wire.read();
  return true;
}

bool BoardHardware::writeRegister(uint8_t address, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(value);
  const uint8_t code = Wire.endTransmission();
  return code == 0 || fail("I2C %02X register %02X write failed (%u)", address, reg, code);
}

bool BoardHardware::readExpander(uint8_t address, ExpanderState &state) {
  ExpanderState next;
  // TCA9535 register auto-increment wraps within pairs, not across all 8 bytes.
  if (!readPair(address, 0, next.input) || !readPair(address, 2, next.output) ||
      !readPair(address, 4, next.polarity) || !readPair(address, 6, next.config)) {
    state.valid = false;  // Retain old raw bytes and timestamp, explicitly invalid.
    return false;
  }
  next.physicalInputs = uint16_t(next.input[0] ^ next.polarity[0]) |
      (uint16_t(next.input[1] ^ next.polarity[1]) << 8);
  next.atMs = millis();
  next.valid = true;
  state = next;
  return true;
}

void ARDUINO_ISR_ATTR BoardHardware::onInterrupt(void *argument) {
  auto *line = static_cast<IrqLine *>(argument);
  const bool level = gpio_get_level(static_cast<gpio_num_t>(line->gpio));
  portENTER_CRITICAL_ISR(&line->owner->irqMux_);
  if (level) line->rises = line->rises + 1;
  else line->falls = line->falls + 1;
  portEXIT_CRITICAL_ISR(&line->owner->irqMux_);
}

bool BoardHardware::beginMonitoring() {
  error_[0] = '\0';
  if (monitoring_) return fail("Monitoring already initialized");
  // Set hardware latches before Arduino's peripheral manager attaches outputs.
  gpio_set_level(static_cast<gpio_num_t>(kBackup), 0);
  pinMode(kBackup, OUTPUT);
  digitalWrite(kBackup, LOW);
  gpio_set_level(static_cast<gpio_num_t>(kLed), 1);
  pinMode(kLed, OUTPUT);
  digitalWrite(kLed, HIGH);
  // GPIO_0 connects only IC1 and SW1 to ground; retain a pull-up after boot.
  pinMode(kBoot, INPUT_PULLUP);
  analogReadResolution(12);
  for (uint8_t pin : kAdcPins) analogSetPinAttenuation(pin, ADC_11db);
  const uint8_t irqPins[] = {15, 16, 11, 39};
  for (unsigned i = 0; i < 4; ++i) {
    irq_[i].owner = this;
    irq_[i].gpio = irqPins[i];
    pinMode(irqPins[i], INPUT);  // Board pull-ups; no internal pull-up substitution.
    attachInterruptArg(irqPins[i], onInterrupt, &irq_[i], CHANGE);
  }
  if (!Wire.begin(17, 18, 100000)) return fail("Control I2C initialization failed; expander state unknown");
  Wire.setTimeOut(25);
  monitoring_ = true;
  return true;
}

bool BoardHardware::poll() {
  if (!monitoring_) return fail("Monitoring not initialized");
  if (result_ != Result::Failed) error_[0] = '\0';
  const bool internalOk = readExpander(kInternal, snapshot_.internal);
  const bool externalOk = readExpander(kExternal, snapshot_.external);
  if (internalOk) {
    const uint8_t in = uint8_t(snapshot_.internal.physicalInputs);
    snapshot_.dcPg = high(in, 0); snapshot_.buckPg = high(in, 1);
    snapshot_.usbPg = high(in, 2); snapshot_.chargerPg = high(in, 3);
    snapshot_.chargerStat = high(in, 4); snapshot_.backupPg = high(in, 5);
    snapshot_.boostPg = high(in, 6); snapshot_.ssBuckPg = high(in, 7);
  }
  if (externalOk) {
    const uint8_t in = uint8_t(snapshot_.external.physicalInputs);
    snapshot_.jetsonOn = high(in, 0); snapshot_.ext5vSsPg = high(in, 1);
    snapshot_.extSsPg = high(in, 2); snapshot_.extVbusPg = high(in, 3);
    snapshot_.ext5vVbusPg = high(in, 4);
  }
  for (unsigned i = 0; i < 9; ++i) {
    AnalogReading &reading = snapshot_.adc[i];
    reading.gpio = kAdcPins[i];
    const unsigned count = i < 2 ? 16 : 8;
    uint32_t raw = 0, mv = 0;
    for (unsigned n = 0; n < count; ++n) {
      raw += analogRead(reading.gpio);
      mv += analogReadMilliVolts(reading.gpio);
    }
    reading.raw = (raw + count / 2) / count;
    reading.mv = (mv + count / 2) / count;
    // 181 uA/A typical LM73100 IMON gain x schematic 820 ohms = 148.42 mV/A.
    // No zero-current offset or ADC/divider calibration has been applied.
    reading.currentA = i < 2 ? NAN : reading.mv / 148.42f;
  }
  snapshot_.vcapV = snapshot_.adc[1].mv * 0.0092f;
  snapshot_.tsMv = snapshot_.adc[0].mv;
  snapshot_.mainSelected = digitalRead(kMux) == HIGH;
  snapshot_.backupEnabled = digitalRead(kBackup) == HIGH;
  snapshot_.statusLed = digitalRead(kLed) == HIGH;
  snapshot_.bootButtonLow = digitalRead(kBoot) == LOW;
  snapshot_.internalIrqLow = digitalRead(15) == LOW;
  snapshot_.externalIrqLow = digitalRead(16) == LOW;
  snapshot_.pdIrqLow = digitalRead(39) == LOW;
  portENTER_CRITICAL(&irqMux_);
  snapshot_.internalIrqFalls = irq_[0].falls; snapshot_.internalIrqRises = irq_[0].rises;
  snapshot_.externalIrqFalls = irq_[1].falls; snapshot_.externalIrqRises = irq_[1].rises;
  snapshot_.muxFalls = irq_[2].falls; snapshot_.muxRises = irq_[2].rises;
  snapshot_.pdIrqFalls = irq_[3].falls; snapshot_.pdIrqRises = irq_[3].rises;
  portEXIT_CRITICAL(&irqMux_);
  snapshot_.atMs = millis();
  snapshot_.valid = internalOk && externalOk;
  return snapshot_.valid;
}

bool BoardHardware::freshInternal() const {
  return snapshot_.internal.valid && uint32_t(millis() - snapshot_.internal.atMs) <= kFreshMs;
}

bool BoardHardware::currentMain() const {
  // PMUX_ST high alone also means mux Hi-Z. Require exactly one input PG.
  return freshInternal() && snapshot_.mainSelected && digitalRead(kMux) == HIGH &&
      (snapshot_.usbPg != snapshot_.dcPg) && snapshot_.ssBuckPg;
}

bool BoardHardware::verifyOperational() {
  ExpanderState internal, external;
  if (!readExpander(kInternal, internal) || !readExpander(kExternal, external)) return false;
  const ExpanderState *states[] = {&internal, &external};
  const uint8_t addresses[] = {kInternal, kExternal}, expected[] = {expectedInternal_, expectedExternal_};
  for (unsigned i = 0; i < 2; ++i) {
    const ExpanderState &s = *states[i];
    if (s.config[0] != 0xff || s.config[1] != kUnusedInputs || s.polarity[0] || s.polarity[1] ||
        s.output[0] || s.output[1] != expected[i] || ((s.physicalInputs >> 8) & kDefined) != (expected[i] & kDefined))
      return fail("Expander %02X configuration/latch/physical enable mismatch", addresses[i]);
  }
  return true;
}

bool BoardHardware::writeControl(uint8_t address, uint8_t value) {
  if (!writeRegister(address, 3, value)) return false;
  if (address == kInternal) expectedInternal_ = value;
  else expectedExternal_ = value;
  ExpanderState s;
  if (!readExpander(address, s)) return false;
  if (s.config[0] != 0xff || s.config[1] != kUnusedInputs || s.polarity[0] || s.polarity[1] ||
      s.output[0] || s.output[1] != value || ((s.physicalInputs >> 8) & kDefined) != (value & kDefined))
    return fail("Expander %02X commanded enable/configuration readback mismatch", address);
  return true;
}

bool BoardHardware::verifyControlLow(uint8_t mask) {
  uint8_t outputs[2], inputs[2], polarity[2];
  if (!readPair(kInternal, 2, outputs) || !readPair(kInternal, 4, polarity) ||
      !readPair(kInternal, 0, inputs)) return false;
  expectedInternal_ = outputs[1];
  if ((outputs[1] & mask) || ((inputs[1] ^ polarity[1]) & mask))
    return fail("Internal latch/pin did not go low (mask %02X)", mask);
  return true;
}

bool BoardHardware::forceCeLow() {
  uint8_t outputs[2], config[2];
  if (!readPair(kInternal, 2, outputs)) return false;  // Never invent an unknown boost state.
  if (!writeRegister(kInternal, 3, outputs[1] & ~kCe)) return false;
  if (!readPair(kInternal, 6, config) ||
      !writeRegister(kInternal, 7, (config[1] | kUnusedInputs) & ~kCe) ||
      !verifyControlLow(kCe)) return false;
  ceOffAt_ = millis();
  return true;
}

bool BoardHardware::beginOperation(Operation operation, bool preempt) {
  if (busy() && !preempt) {
    error_[0] = '\0';
    return fail("Another hardware operation is running");
  }
  error_[0] = '\0';
  if (!monitoring_) return fail("Monitoring not initialized");
  operation_ = operation;
  result_ = Result::Running;
  stage_ = 0;
  operationAt_ = stageAt_ = millis();
  pgTiming_ = false;
  return true;
}

bool BoardHardware::startReconcile() { return beginOperation(Operation::Reconcile); }
bool BoardHardware::startPrepareCharge() { return beginOperation(Operation::PrepareCharge); }
bool BoardHardware::startEnableCharge() { return beginOperation(Operation::EnableCharge); }
bool BoardHardware::startDisableCharging() { return beginOperation(Operation::DisableCharging, true); }
bool BoardHardware::startRestorePor() { return beginOperation(Operation::RestorePor, true); }

bool BoardHardware::startPort(Port port, bool enabled) {
  uint8_t mask;
  switch (port) {
    case Port::Vbus: mask = 0x02; break;
    case Port::FiveVoltVbus: mask = 0x01; break;
    case Port::VbusSs: mask = 0x04; break;
    case Port::FiveVoltSs: mask = 0x08; break;
    default: error_[0] = '\0'; return fail("Unknown port");
  }
  if (!beginOperation(Operation::PortChange)) return false;
  port_ = port; portMask_ = mask; portEnabled_ = enabled;
  return true;
}

void BoardHardware::advance(uint8_t stage) { stage_ = stage; stageAt_ = millis(); pgTiming_ = false; }
void BoardHardware::succeed() { result_ = Result::Succeeded; }

bool BoardHardware::failOperation() {
  // A failed enable must never leave CE knowingly asserted. Boost removal still
  // belongs to the asynchronous DisableCharging operation after current decay.
  if (operation_ == Operation::PrepareCharge || operation_ == Operation::EnableCharge) {
    prepared_ = false;
    forceCeLow();
  } else if (operation_ == Operation::PortChange && portEnabled_) {
    // A readback mismatch may mean an unrelated output differs from our cache.
    // Rollback must only LOWER the requested bit in the actual latch; replaying
    // the cache could reassert CE, change its profile, or resurrect another port.
    auto clearActual = [&](uint8_t address, uint8_t mask) {
      uint8_t output[2], input[2], polarity[2];
      if (!readPair(address, 2, output) || !writeRegister(address, 3, output[1] & ~mask) ||
          !readPair(address, 2, output) || !readPair(address, 4, polarity) ||
          !readPair(address, 0, input)) return false;
      if (address == kInternal) expectedInternal_ = output[1];
      else expectedExternal_ = output[1];
      if ((output[1] & mask) || ((input[1] ^ polarity[1]) & mask))
        return fail("Port rollback could not verify %02X mask %02X low", address, mask);
      return true;
    };
    if (clearActual(kExternal, portMask_) && port_ == Port::FiveVoltVbus)
      clearActual(kInternal, kBuck);
  } else if (operation_ == Operation::Reconcile || operation_ == Operation::RestorePor) {
    // Independent exterior shutdown still helps when internal CE cannot be verified.
    if (writeRegister(kExternal, 3, 0)) writeRegister(kExternal, 7, kUnusedInputs);
  }
  result_ = Result::Failed;
  return false;
}

bool BoardHardware::waitPg(bool good, uint32_t timeoutMs) {
  if (good) {
    if (!pgTiming_) { pgTiming_ = true; pgAt_ = millis(); }
    if (uint32_t(millis() - pgAt_) >= 100) return true;
  } else pgTiming_ = false;
  if (uint32_t(millis() - stageAt_) >= timeoutMs) {
    fail("PG failed to remain valid for 100 ms within %lu ms", static_cast<unsigned long>(timeoutMs));
    failOperation();
  }
  return false;
}

bool BoardHardware::verifyPor(uint8_t address) {
  ExpanderState s;
  if (!readExpander(address, s)) return false;
  if (s.output[0] != 0xff || s.output[1] != 0xff || s.config[0] != 0xff || s.config[1] != 0xff ||
      s.polarity[0] || s.polarity[1] || ((s.physicalInputs >> 8) & kDefined))
    return fail("Expander %02X POR register/pulldown verification failed", address);
  return true;
}

void BoardHardware::service() {
  if (!busy()) return;
  if (operation_ == Operation::Reconcile || operation_ == Operation::RestorePor) {
    bool ok = true;
    switch (stage_) {
      case 0:
        reconciled_ = prepared_ = false;
        ok = forceCeLow();
        if (ok) advance(1);
        break;
      case 1:
        if (uint32_t(millis() - ceOffAt_) < kDecayMs) return;
        ok = verifyControlLow(kCe) && writeRegister(kExternal, 3, 0);
        if (ok) advance(2);
        break;
      case 2:
        ok = writeRegister(kInternal, 3, 0);
        if (ok) advance(3);
        break;
      case 3:
        ok = writeRegister(kInternal, 6, 0xff) && writeRegister(kExternal, 6, 0xff);
        if (ok) advance(4);
        break;
      case 4:
        ok = writeRegister(kInternal, 2, 0) && writeRegister(kExternal, 2, 0);
        if (ok) advance(5);
        break;
      case 5:
        ok = writeRegister(kInternal, 7, kUnusedInputs) && writeRegister(kExternal, 7, kUnusedInputs);
        if (ok) advance(6);
        break;
      case 6:
        ok = writeRegister(kInternal, 4, 0) && writeRegister(kInternal, 5, 0) &&
             writeRegister(kExternal, 4, 0) && writeRegister(kExternal, 5, 0);
        if (ok) advance(7);
        break;
      case 7:
        expectedInternal_ = expectedExternal_ = 0;
        ok = verifyOperational();
        if (ok && operation_ == Operation::Reconcile) { reconciled_ = true; succeed(); }
        else if (ok) advance(8);
        break;
      case 8:
        // High-impedance direction first. Only then may POR's HIGH output latch
        // defaults be restored without physically asserting every enable.
        ok = writeRegister(kInternal, 7, 0xff) && writeRegister(kExternal, 7, 0xff);
        if (ok) advance(9);
        break;
      case 9: {
        uint8_t inConfig[2], extConfig[2];
        ok = readPair(kInternal, 6, inConfig) && readPair(kExternal, 6, extConfig);
        if (ok && (inConfig[0] != 0xff || inConfig[1] != 0xff || extConfig[0] != 0xff || extConfig[1] != 0xff))
          ok = fail("Refusing POR HIGH latches before all pins verify as inputs");
        if (ok) ok = writeRegister(kInternal, 2, 0xff) && writeRegister(kInternal, 3, 0xff) &&
                     writeRegister(kExternal, 2, 0xff) && writeRegister(kExternal, 3, 0xff);
        if (ok) advance(10);
        break;
      }
      case 10:
        ok = verifyPor(kInternal) && verifyPor(kExternal);
        if (ok) succeed();
        break;
      default: ok = fail("Invalid reconciliation stage");
    }
    if (!ok) failOperation();
    return;
  }

  if (operation_ == Operation::DisableCharging) {
    prepared_ = false;
    if (stage_ == 0) {
      if (!forceCeLow()) { failOperation(); return; }
      advance(1);
    } else if (uint32_t(millis() - ceOffAt_) >= kDecayMs) {
      if (!verifyControlLow(kCe) || !writeRegister(kInternal, 3, expectedInternal_ & ~(kCe | kBoost)) ||
          !verifyControlLow(kCe | kBoost)) { failOperation(); return; }
      succeed();
    }
    return;
  }

  if (!reconciled_) { fail("Reconcile hardware before enabling a function"); failOperation(); return; }
  const bool chargingOperation = operation_ == Operation::PrepareCharge || operation_ == Operation::EnableCharge;
  const bool unbackedPort = operation_ == Operation::PortChange && portEnabled_ &&
      (port_ == Port::Vbus || port_ == Port::FiveVoltVbus);
  if ((chargingOperation || unbackedPort) && !currentMain()) {
    fail("Main source invalid or status stale during enable operation"); failOperation(); return;
  }

  if (operation_ == Operation::PrepareCharge) {
    switch (stage_) {
      case 0:
        prepared_ = false;
        if (!verifyOperational() || !forceCeLow()) { failOperation(); return; }
        advance(1);
        break;
      case 1:
        if (uint32_t(millis() - ceOffAt_) < kDecayMs) return;
        if (!verifyControlLow(kCe) || !writeControl(kInternal, expectedInternal_ & ~(kCe | kHighCurrent | kTsOverride))) {
          failOperation(); return;
        }
        advance(2);
        break;
      case 2:
        if (!writeControl(kInternal, expectedInternal_ | kBoost)) { failOperation(); return; }
        tsSelectedAt_ = millis();  // Allow the charger reference/real-TS path to recover after boost starts.
        advance(3);
        break;
      case 3:
        if (waitPg(freshInternal() && snapshot_.boostPg)) advance(4);
        break;
      case 4:
        if (uint32_t(millis() - tsSelectedAt_) < kTsSettleMs) return;
        if (snapshot_.boostPg && snapshot_.chargerPg && snapshot_.tsMv >= 1300 && snapshot_.tsMv <= 2350) {
          if (!verifyOperational() || !verifyControlLow(kCe | kHighCurrent | kTsOverride)) { failOperation(); return; }
          prepared_ = true;
          succeed();
        } else if (uint32_t(millis() - stageAt_) >= 2000) {
          fail("Real NTC/reference/charger PG did not qualify after boost startup"); failOperation();
        }
        break;
      default: fail("Invalid charge-preparation stage"); failOperation();
    }
    return;
  }

  if (operation_ == Operation::EnableCharge) {
    if (!prepared_ || !snapshot_.boostPg || !snapshot_.chargerPg || snapshot_.tsMv < 1300 || snapshot_.tsMv > 2350 ||
        !isfinite(snapshot_.vcapV) || snapshot_.vcapV >= 21.0f ||
        (expectedInternal_ & (kCe | kHighCurrent | kTsOverride)) || !(expectedInternal_ & kBoost)) {
      fail("Charger enable requires prepared real-NTC low-current supply and valid VCAP"); failOperation(); return;
    }
    if (!verifyOperational() || !writeControl(kInternal, expectedInternal_ | kCe)) { failOperation(); return; }
    succeed();
    return;
  }

  if (operation_ == Operation::PortChange) {
    if (portEnabled_ && (port_ == Port::VbusSs || port_ == Port::FiveVoltSs) &&
        (!freshInternal() || !snapshot_.ssBuckPg ||
         (!currentMain() && !(!snapshot_.mainSelected && snapshot_.backupPg && digitalRead(kBackup))))) {
      fail("Backed port requires valid selected source and backed buck PG"); failOperation(); return;
    }
    switch (stage_) {
      case 0:
        if (!verifyOperational()) { failOperation(); return; }
        if (!portEnabled_) {
          if (!writeControl(kExternal, expectedExternal_ & ~portMask_)) { failOperation(); return; }
          if (port_ == Port::FiveVoltVbus) advance(5); else succeed();
        } else if (port_ == Port::FiveVoltVbus) {
          if (!writeControl(kInternal, expectedInternal_ | kBuck)) { failOperation(); return; }
          advance(1);
        } else advance(2);
        break;
      case 1:
        if (waitPg(freshInternal() && snapshot_.buckPg)) advance(2);
        break;
      case 2:
        if (!writeControl(kExternal, expectedExternal_ | portMask_)) { failOperation(); return; }
        advance(3);
        break;
      case 3: {
        bool good = false;
        switch (port_) {
          case Port::Vbus: good = snapshot_.extVbusPg; break;
          case Port::FiveVoltVbus: good = snapshot_.ext5vVbusPg && snapshot_.buckPg; break;
          case Port::VbusSs: good = snapshot_.extSsPg; break;
          case Port::FiveVoltSs: good = snapshot_.ext5vSsPg && snapshot_.ssBuckPg; break;
        }
        if (waitPg(snapshot_.external.valid && uint32_t(millis() - snapshot_.external.atMs) <= kFreshMs && good)) succeed();
        break;
      }
      case 5:
        if ((expectedExternal_ & 0x01) || !writeControl(kInternal, expectedInternal_ & ~kBuck)) {
          fail("J8 must be off before disabling its buck"); failOperation(); return;
        }
        succeed();
        break;
      default: fail("Invalid port operation stage"); failOperation();
    }
  }
}

bool BoardHardware::setBackup(bool enabled) {
  error_[0] = '\0';
  if (!monitoring_) return fail("Monitoring not initialized");
  if (enabled && (!currentMain() || !isfinite(snapshot_.vcapV) || snapshot_.vcapV < 10.0f || snapshot_.vcapV > 21.0f))
    return fail("Backup arm requires valid main and VCAP within 10..21 V");
  if (!enabled && digitalRead(kBackup) && !currentMain()) return fail("Refusing to release sole backup source");
  digitalWrite(kBackup, enabled ? HIGH : LOW);
  return digitalRead(kBackup) == (enabled ? HIGH : LOW) || fail("GPIO42 backup enable readback failed");
}

bool BoardHardware::setLed(bool enabled) {
  if (!monitoring_) return fail("Monitoring not initialized");
  digitalWrite(kLed, enabled ? HIGH : LOW);
  return digitalRead(kLed) == (enabled ? HIGH : LOW) || fail("GPIO4 status LED readback failed");
}

bool BoardHardware::releaseBackupForShutdown() {
  gpio_set_level(static_cast<gpio_num_t>(kBackup), 0);
  pinMode(kBackup, OUTPUT);
  digitalWrite(kBackup, LOW);
  return digitalRead(kBackup) == LOW || fail("Final GPIO42 release failed");
}
