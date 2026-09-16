#include "BoardHardware.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

TwoWire Wire;
static uint32_t now;
static int gpio[49];
static int gpioMode[49];
static uint32_t adc[49];
static void (*handlers[49])(void *);
static void *handlerArgs[49];
uint32_t millis() { return now; }
void digitalWrite(int pin, int value) { gpio[pin] = value; }
int digitalRead(int pin) { return gpio[pin]; }
int gpio_set_level(int pin, unsigned value) { gpio[pin] = value; return 0; }
int gpio_get_level(int pin) { return gpio[pin]; }
void pinMode(int pin, int mode) { gpioMode[pin] = mode; }
void analogReadResolution(int) {}
void analogSetPinAttenuation(int, int) {}
uint16_t analogRead(int pin) { return adc[pin] * 4095 / 3300; }
uint32_t analogReadMilliVolts(int pin) { return adc[pin]; }
void attachInterruptArg(int pin, void (*handler)(void *), void *arg, int) { handlers[pin] = handler; handlerArgs[pin] = arg; }

uint8_t TwoWire::endTransmission(bool) {
  MockExpander &d = address_ == 0x20 ? internal : external;
  if (!d.present) return 2;
  pointer_ = tx_.front();
  if (tx_.size() == 2) {
    const uint8_t ignored = pointer_ == 3 ? d.ignoredOutput : pointer_ == 7 ? d.ignoredConfig : 0;
    d.reg[pointer_] = (d.reg[pointer_] & ignored) | (tx_[1] & ~ignored);
    writes.push_back({address_, pointer_, tx_[1], d.physical(), now});
  }
  return 0;
}

size_t TwoWire::requestFrom(uint8_t address, uint8_t count) {
  rx_.clear();
  MockExpander &d = address == 0x20 ? internal : external;
  if (!d.present) return 0;
  uint8_t input = address == 0x20 ? internalInput : externalInput;
  if (automaticInputs) {
    const uint8_t in = internal.physical(), ex = external.physical();
    if (address == 0x20) {
      input = 0x88 | (mainPg ? 0x04 : 0);
      if ((in & 0x01) && boostPg) input |= 0x40;
      if ((in & 0x10) && buckPg) input |= 0x02;
      if (in & 0x02) input |= 0x10;
      if (gpio[42]) input |= 0x20;
    } else {
      input = 0;
      if (ex & 0x02) input |= 0x08;
      if ((ex & 0x01) && (in & 0x10) && buckPg) input |= 0x10;
      if (ex & 0x04) input |= 0x04;
      if (ex & 0x08) input |= 0x02;
    }
  }
  for (uint8_t i = 0; i < count; ++i) {
    const uint8_t reg = (pointer_ & 0xfe) | ((pointer_ + i) & 1);
    rx_.push_back(reg == 0 ? input ^ d.reg[4] : reg == 1 ? d.physical() ^ d.reg[5] : d.reg[reg]);
  }
  return count;
}

static void reset() {
  Wire = TwoWire{}; now = 1000;
  std::fill(std::begin(gpio), std::end(gpio), 0);
  std::fill(std::begin(gpioMode), std::end(gpioMode), 0);
  std::fill(std::begin(adc), std::end(adc), 1800);
  std::fill(std::begin(handlers), std::end(handlers), nullptr);
  gpio[11] = gpio[15] = gpio[16] = gpio[39] = HIGH;
  adc[2] = 1300;
}

static void tick(BoardHardware &h, unsigned ms = 20) {
  now += ms;
  h.poll();
  const uint32_t before = now;
  h.service();
  assert(now == before);  // No delay() is linked; waiting belongs to this caller.
}
static void finish(BoardHardware &h, unsigned limitMs = 5000) {
  const uint32_t start = now;
  while (h.busy() && uint32_t(now - start) < limitMs) tick(h);
  assert(!h.busy());
}
static void ready(BoardHardware &h) {
  assert(h.beginMonitoring()); assert(h.poll()); assert(h.startReconcile()); finish(h);
  assert(h.result() == BoardHardware::Result::Succeeded);
  assert(h.poll());
}
static void charge(BoardHardware &h) {
  assert(h.startPrepareCharge()); finish(h);
  assert(h.result() == BoardHardware::Result::Succeeded);
  assert((Wire.internal.physical() & 0x0f) == 1);
  assert(h.startEnableCharge()); finish(h);
  assert(h.result() == BoardHardware::Result::Succeeded);
  assert((Wire.internal.physical() & 3) == 3);
}

static void monitoringNeverChangesRetainedExpanders() {
  reset(); BoardHardware h;
  Wire.internal.reg[3] = 0x0f; Wire.internal.reg[7] = 0xe0;
  Wire.external.reg[3] = 0x1f; Wire.external.reg[7] = 0xe0;
  assert(h.beginMonitoring());
  for (unsigned i = 0; i < 100; ++i) tick(h, 100);
  assert(Wire.writes.empty());
  assert(Wire.internal.physical() == 0x0f && Wire.external.physical() == 0x1f);
  assert(gpio[42] == LOW && gpio[4] == HIGH);
  assert(h.snapshot().adc[0].gpio == 1 && h.snapshot().adc[8].gpio == 10);
  assert(std::isnan(h.snapshot().adc[1].currentA));
  assert(std::fabs(h.snapshot().vcapV - 11.96f) < 0.001f);
}

static void bootButtonUsesPullupAndReportsBothLevels() {
  reset(); BoardHardware h;
  assert(h.beginMonitoring());
  assert(gpioMode[0] == INPUT_PULLUP);
  gpio[0] = HIGH;
  assert(h.poll() && !h.snapshot().bootButtonLow);
  gpio[0] = LOW;
  assert(h.poll() && h.snapshot().bootButtonLow);
  gpio[0] = HIGH;
  Wire.external.present = false;
  assert(!h.poll() && !h.snapshot().bootButtonLow);
  assert(Wire.writes.empty());
}

static void reconcileAndPorRespectSequencing() {
  reset(); BoardHardware h;
  Wire.internal.reg[3] = 0x0f; Wire.internal.reg[7] = 0xe0;
  assert(h.beginMonitoring()); assert(h.poll()); assert(h.startReconcile());
  h.service();
  assert((Wire.internal.physical() & 0x0f) == 0x0d);
  tick(h, 99); assert(Wire.internal.physical() & 1);
  finish(h); assert(h.result() == BoardHardware::Result::Succeeded);
  uint32_t ceAt = 0, boostAt = 0;
  for (const auto &w : Wire.writes) if (w.address == 0x20 && w.reg == 3) {
    if (w.value & 1) ceAt = w.at; else boostAt = w.at;
  }
  assert(boostAt >= ceAt + 100);
  assert(h.poll()); assert(h.setBackup(true));
  Wire.writes.clear();
  assert(h.startRestorePor()); finish(h);
  assert(h.result() == BoardHardware::Result::Succeeded);
  for (const auto &w : Wire.writes) assert((w.physical & 0x1f) == 0);
  assert(Wire.internal.reg[3] == 0xff && Wire.internal.reg[7] == 0xff);
  assert(Wire.external.reg[3] == 0xff && Wire.external.reg[7] == 0xff);
  assert(gpio[42] == HIGH);  // Only the final explicit release may cut backup.
  assert(h.releaseBackupForShutdown()); assert(gpio[42] == LOW);
}

static void chargingPreparationAndEmergencyStopAreAsynchronous() {
  reset(); BoardHardware h; ready(h);
  uint32_t started = now;
  assert(h.startPrepareCharge()); finish(h);
  assert(h.result() == BoardHardware::Result::Succeeded && now - started >= 500);
  assert(!(Wire.internal.physical() & 0x0e));
  assert(h.startEnableCharge()); finish(h);
  assert(Wire.internal.physical() & 2);
  Wire.external.present = false;
  Wire.writes.clear();
  assert(h.startDisableCharging()); h.service();
  assert((Wire.internal.physical() & 3) == 1);
  tick(h, 99); assert(Wire.internal.physical() & 1);
  finish(h); assert(h.result() == BoardHardware::Result::Succeeded);
  assert(!(Wire.internal.physical() & 3));
  for (const auto &w : Wire.writes) assert(w.address == 0x20);
}

static void j8WaitsForBuckAndUrgentStopPreemptsEnable() {
  reset(); BoardHardware h; ready(h);
  assert(h.startPort(Port::FiveVoltVbus, true)); finish(h);
  assert(h.result() == BoardHardware::Result::Succeeded);
  assert(Wire.internal.physical() & 0x10); assert(Wire.external.physical() & 1);
  assert(h.startPort(Port::FiveVoltVbus, false)); finish(h);
  assert(!(Wire.internal.physical() & 0x10)); assert(!(Wire.external.physical() & 1));
  Wire.buckPg = false;
  assert(h.startPort(Port::FiveVoltVbus, true)); tick(h);
  assert(h.busy() && (Wire.internal.physical() & 0x10));
  assert(h.startDisableCharging()); finish(h);
  assert(h.operation() == BoardHardware::Operation::DisableCharging);
  assert(!(Wire.external.physical() & 1));
  assert(Wire.internal.physical() & 0x10);  // Policy decides when to remove port/buck loads.
}

static void pgTimeoutDoesNotBlockCallerOrEnableJ8() {
  reset(); BoardHardware h; ready(h); Wire.buckPg = false;
  Wire.writes.clear();
  assert(h.startPort(Port::FiveVoltVbus, true));
  unsigned telemetry = 0; uint32_t last = now;
  while (h.busy()) {
    tick(h);
    if (now - last >= 1000) { ++telemetry; last = now; }
  }
  assert(telemetry >= 2 && h.result() == BoardHardware::Result::Failed);
  assert(!(Wire.internal.physical() & 0x10) && !(Wire.external.physical() & 1));
  for (const auto &w : Wire.writes) if (w.address == 0x21 && w.reg == 3) assert(!(w.value & 1));
}

static void portRollbackPreservesActualStateInsteadOfCachedHighBits() {
  reset(); BoardHardware h; ready(h); charge(h);
  assert(h.startPort(Port::VbusSs, true)); finish(h);
  assert(h.result() == BoardHardware::Result::Succeeded);
  assert(h.poll());
  assert(h.startPort(Port::FiveVoltVbus, true));
  // Model a register mismatch after acceptance. Cache says CE+boost and J9 are
  // on, but actual hardware has them off and a different profile/port selected.
  Wire.internal.reg[3] = 0x18;  // TS override + buck; CE and boost actually low.
  Wire.external.reg[3] = 0x03;  // J7 + J8; J9 actually low.
  Wire.writes.clear();
  h.service();
  assert(h.result() == BoardHardware::Result::Failed);
  assert(Wire.internal.reg[3] == 0x08);  // Clear buck only; preserve actual profile.
  assert(Wire.external.reg[3] == 0x02);  // Clear J8 only; preserve actual J7.
  for (const auto &w : Wire.writes) if (w.reg == 3) {
    if (w.address == 0x20) assert(!(w.value & 0x03));  // Never reassert CE or boost.
    else assert(!(w.value & 0x04));                  // Never resurrect cached J9.
  }
}

static void failedPortRollbackNeverBlindlyDropsItsBuck() {
  reset(); BoardHardware h; ready(h); charge(h);
  assert(h.startPort(Port::FiveVoltVbus, true)); finish(h);
  assert(h.result() == BoardHardware::Result::Succeeded);
  assert(h.poll());
  assert(h.startPort(Port::FiveVoltVbus, true));
  Wire.internal.reg[3] = 0x10;  // Mismatch triggers rollback; actual CE is low.
  Wire.external.ignoredOutput = 0x01;  // J8 cannot be disabled.
  Wire.writes.clear();
  h.service();
  assert(h.result() == BoardHardware::Result::Failed);
  assert(Wire.internal.reg[3] == 0x10 && (Wire.external.physical() & 1));
  for (const auto &w : Wire.writes) assert(w.address != 0x20);
}

static void porPreemptsWithAlreadyLowCeWithoutRaisingAnEnable() {
  reset(); BoardHardware h; ready(h);
  assert(h.startPort(Port::FiveVoltVbus, true)); tick(h);
  assert(h.busy() && (Wire.internal.physical() & 0x10));
  Wire.writes.clear();
  assert(h.startRestorePor()); finish(h);
  assert(h.result() == BoardHardware::Result::Succeeded);
  for (const auto &w : Wire.writes) {
    // Buck can remain high only until its shutdown stage. Neither CE/boost nor
    // the interrupted J8 enable can ever assert during POR restoration.
    if (w.address == 0x20) assert(!(w.physical & 0x03));
    else assert(!(w.physical & 0x01));
  }
}

static void stuckCeAndPorDirectionFaultCannotAssertUnknownEnables() {
  reset(); BoardHardware h; ready(h); charge(h);
  Wire.internal.forcedHigh = 2;
  assert(h.startRestorePor()); finish(h);
  assert(h.result() == BoardHardware::Result::Failed);
  assert((Wire.internal.physical() & 3) == 3);
  reset(); BoardHardware second; ready(second);
  Wire.internal.ignoredConfig = 0x1f;
  Wire.writes.clear();
  assert(second.startRestorePor()); finish(second);
  assert(second.result() == BoardHardware::Result::Failed);
  for (const auto &w : Wire.writes)
    if (w.address == 0x20 && w.reg == 3) assert(w.value != 0xff);
}

static void rawInputsPolarityAdcAndIrqRemainObservableOnI2cFailure() {
  reset(); BoardHardware h; ready(h);
  Wire.automaticInputs = false;
  Wire.internalInput = 0xe5; Wire.externalInput = 0x19;
  Wire.internal.reg[4] = 0xff;
  assert(h.poll());
  const HardwareSnapshot &s = h.snapshot();
  assert(s.dcPg && !s.buckPg && s.usbPg && s.backupPg && s.boostPg && s.ssBuckPg);
  assert(s.jetsonOn && s.extVbusPg && s.ext5vVbusPg && !s.extSsPg && !s.ext5vSsPg);
  assert(s.internal.input[0] == uint8_t(0xe5 ^ 0xff));
  gpio[15] = LOW; handlers[15](handlerArgs[15]);
  gpio[15] = HIGH; handlers[15](handlerArgs[15]);
  Wire.external.present = false; adc[2] = 1000; now += 100;
  assert(!h.poll());
  assert(!s.valid && s.internal.valid && !s.external.valid);
  assert(s.internalIrqFalls == 1 && s.internalIrqRises == 1);
  assert(std::fabs(s.vcapV - 9.2f) < 0.001f);
}

int main() {
  monitoringNeverChangesRetainedExpanders();
  bootButtonUsesPullupAndReportsBothLevels();
  reconcileAndPorRespectSequencing();
  chargingPreparationAndEmergencyStopAreAsynchronous();
  j8WaitsForBuckAndUrgentStopPreemptsEnable();
  pgTimeoutDoesNotBlockCallerOrEnableJ8();
  portRollbackPreservesActualStateInsteadOfCachedHighBits();
  failedPortRollbackNeverBlindlyDropsItsBuck();
  porPreemptsWithAlreadyLowCeWithoutRaisingAnEnable();
  stuckCeAndPorDirectionFaultCannotAssertUnknownEnables();
  rawInputsPolarityAdcAndIrqRemainObservableOnI2cFailure();
  std::puts("PASS deployment hardware: read-only settle, asynchronous sequencing, PG timeout/preemption, J8, POR, faults, ADC/raw/IRQ telemetry");
}
