#include "PdDiagnostics.h"

#include <driver/gpio.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {
constexpr uint8_t kAddress = 0x08;
constexpr int kSda = 40, kScl = 41, kInterrupt = 39;
constexpr uint16_t kDeviceMode = 0x0000, kSiliconId = 0x0002;
constexpr uint16_t kInterruptStatus = 0x0006, kVersions = 0x0010;
constexpr uint16_t kPdStatus = 0x1008, kTypeCStatus = 0x100c;
constexpr uint16_t kCurrentPdo = 0x1010, kCurrentRdo = 0x1014;
// CYPD3177-specific HPI Utility guide, 002-29388 Rev. *B, Figure 26.
constexpr uint16_t kSelectSinkPdo = 0x1005, kPdResponse = 0x1400;
constexpr uint16_t kDataMemoryWrite = 0x1800;

uint32_t littleEndian32(const uint8_t *bytes) {
  return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
         (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}

void putLittleEndian32(uint8_t *bytes, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) bytes[i] = uint8_t(value >> (8 * i));
}

bool validFixtureData(const PdSnapshot &s) {
  // CYPD3177 IDs are 0x95/0x2004 on production silicon; the original HPI
  // document's conflicting defaults are acknowledged by Infineon. See notes.
  if (s.deviceMode != 0x95 || s.siliconId != 0x2004 || !s.attached() ||
      (s.pdStatus & (1UL << 8)) || !s.fixedSupply() ||
      s.selectedMillivolts() != 20000 || s.sourceMilliamps() == 0 ||
      s.sourceMilliamps() > 5000 || s.objectPosition() == 0 ||
      s.operatingMilliamps() > s.sourceMilliamps() ||
      s.capabilityMismatch()) return false;
  // With GiveBack=1 the low field is minimum current, otherwise maximum.
  if (s.giveBack()) return s.limitMilliamps() <= s.operatingMilliamps();
  return s.limitMilliamps() >= s.operatingMilliamps() &&
         s.limitMilliamps() <= s.sourceMilliamps();
}

void printSnapshot(Stream &log, const PdSnapshot &s, unsigned index) {
  log.printf("PD SAMPLE %u mode=0x%02X silicon=0x%04X status=0x%08lX typeC=0x%02X PDO=0x%08lX RDO=0x%08lX\n",
             index, s.deviceMode, s.siliconId, (unsigned long)s.pdStatus,
             s.typeCStatus, (unsigned long)s.currentPdo, (unsigned long)s.currentRdo);
  log.printf("PD CONTRACT attached=%u powerRole=%s dataRole=%s CC=%u partnerRevisionCode=%lu fixed=%u voltage_mV=%lu source_mA=%lu operating_mA=%lu %s_mA=%lu object=%u mismatch=%u source_advertised_max_mW=%lu\n",
             s.attached(), (s.pdStatus & (1UL << 8)) ? "source" : "sink",
             (s.pdStatus & (1UL << 6)) ? "DFP" : "UFP",
             unsigned((s.typeCStatus >> 1) & 1) + 1,
             (unsigned long)((s.pdStatus >> 16) & 7), s.fixedSupply(),
             (unsigned long)s.selectedMillivolts(), (unsigned long)s.sourceMilliamps(),
             (unsigned long)s.operatingMilliamps(), s.giveBack() ? "minimum" : "maximum",
             (unsigned long)s.limitMilliamps(), s.objectPosition(), s.capabilityMismatch(),
             (unsigned long)(s.selectedMillivolts() * s.sourceMilliamps() / 1000));
  log.printf("PD IRQ GPIO39=%u INTERRUPT=0x%02X falling=%lu rising=%lu pending=%u\n",
             !s.interruptLow, s.interruptStatus,
             (unsigned long)s.interruptFallingEdges, (unsigned long)s.interruptRisingEdges,
             s.interruptStatus != 0);
}
}  // namespace

bool PdDiagnostics::fail(const char *format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(error_, sizeof(error_), format, args);
  va_end(args);
  return false;
}

void ARDUINO_ISR_ATTR PdDiagnostics::interruptHandler(void *context) {
  auto *self = static_cast<PdDiagnostics *>(context);
  if (gpio_get_level(GPIO_NUM_39)) self->risingEdges_ = self->risingEdges_ + 1;
  else self->fallingEdges_ = self->fallingEdges_ + 1;
}

bool PdDiagnostics::begin() {
  error_[0] = '\0';
  if (ready_) return true;
  pinMode(kInterrupt, INPUT);  // Board supplies the power-domain-aware pull-up.
  fallingEdges_ = 0;
  risingEdges_ = 0;
  bus_.setTimeOut(25);
  if (!bus_.begin(kSda, kScl, 100000)) return fail("PD I2C controller begin failed");
  attachInterruptArg(kInterrupt, interruptHandler, this, CHANGE);
  ready_ = true;
  return true;
}

void PdDiagnostics::end() {
  if (!ready_) return;
  detachInterrupt(kInterrupt);
  bus_.end();
  ready_ = false;
}

bool PdDiagnostics::readBytes(uint16_t address, uint8_t *data, size_t length) {
  // HPI uses a 16-bit LITTLE-ENDIAN register pointer. A read transmits only
  // these two bytes; a third byte would become a register write.
  bus_.beginTransmission(kAddress);
  if (bus_.write(uint8_t(address)) != 1 || bus_.write(uint8_t(address >> 8)) != 1)
    return fail("PD pointer buffer failed at 0x%04X", address);
  const uint8_t result = bus_.endTransmission(false);
  if (result != 0) return fail("PD pointer 0x%04X I2C error %u", address, result);
  const size_t received = bus_.requestFrom(kAddress, length, true);
  if (received != length) {
    while (bus_.available()) bus_.read();
    return fail("PD read 0x%04X short: %u/%u", address, unsigned(received), unsigned(length));
  }
  for (size_t i = 0; i < length; ++i) {
    const int value = bus_.read();
    if (value < 0) return fail("PD read 0x%04X missing byte %u", address, unsigned(i));
    data[i] = uint8_t(value);
  }
  return true;
}

bool PdDiagnostics::writeBytes(uint16_t address, const uint8_t *data, size_t length) {
  bus_.beginTransmission(kAddress);
  if (bus_.write(uint8_t(address)) != 1 || bus_.write(uint8_t(address >> 8)) != 1)
    return fail("PD write pointer buffer failed at 0x%04X", address);
  for (size_t i = 0; i < length; ++i) {
    if (bus_.write(data[i]) != 1)
      return fail("PD write buffer failed at 0x%04X byte %u", address, unsigned(i));
  }
  const uint8_t result = bus_.endTransmission(true);
  return result == 0 || fail("PD write 0x%04X I2C error %u", address, result);
}

bool PdDiagnostics::readSnapshot(PdSnapshot &snapshot) {
  error_[0] = '\0';
  if (!ready_) return fail("PD diagnostics not initialized");
  PdSnapshot s;
  uint8_t bytes[4];
  if (!readBytes(kDeviceMode, &s.deviceMode, 1) || !readBytes(kSiliconId, bytes, 2)) return false;
  s.siliconId = uint16_t(bytes[0]) | (uint16_t(bytes[1]) << 8);
  if (!readBytes(kVersions, s.versions, sizeof(s.versions)) ||
      !readBytes(kPdStatus, bytes, 4)) return false;
  s.pdStatus = littleEndian32(bytes);
  // TYPE_C_STATUS is one byte on CYPD3177; do not borrow the misleading
  // four-byte size from its old documentation and include BUS_VOLTAGE bytes.
  if (!readBytes(kTypeCStatus, &s.typeCStatus, 1) || !readBytes(kCurrentPdo, bytes, 4)) return false;
  s.currentPdo = littleEndian32(bytes);
  if (!readBytes(kCurrentRdo, bytes, 4)) return false;
  s.currentRdo = littleEndian32(bytes);
  if (!readBytes(kInterruptStatus, &s.interruptStatus, 1)) return false;
  s.interruptLow = digitalRead(kInterrupt) == LOW;
  s.interruptFallingEdges = fallingEdges_;
  s.interruptRisingEdges = risingEdges_;
  snapshot = s;
  return true;
}

bool PdDiagnostics::requestFixed20VCurrent(uint16_t milliamps, Stream &log,
                                          PdSnapshot &accepted) {
  error_[0] = '\0';
  if (!ready_) return fail("PD diagnostics not initialized");
  if (milliamps < 100 || milliamps > 3000 || milliamps % 10)
    return fail("PD current must be 100..3000 mA in 10 mA steps");
  PdSnapshot initial;
  if (!readSnapshot(initial)) return false;
  if (!validFixtureData(initial) || initial.sourceMilliamps() < milliamps)
    return fail("PD request needs valid attached 20 V source with >=%u mA", milliamps);
  const auto matches = [&](const PdSnapshot &s) {
    return validFixtureData(s) && s.currentPdo == initial.currentPdo &&
           s.objectPosition() == initial.objectPosition() && !s.giveBack() &&
           s.operatingMilliamps() >= milliamps &&
           s.limitMilliamps() >= s.operatingMilliamps();
  };
  bool wroteProfile = !matches(initial);
  uint8_t response[4] = {};
  if (wroteProfile) {
    // Read the prior response so logs distinguish pre-existing data. We do not
    // assume reading acknowledges GPIO39 or invent interrupt-clear semantics.
    if (!readBytes(kPdResponse, response, sizeof(response))) return false;
    log.printf("PD REQUEST before PDO=0x%08lX RDO=0x%08lX response=0x%08lX requested_mA=%u\n",
               (unsigned long)initial.currentPdo, (unsigned long)initial.currentRdo,
               (unsigned long)littleEndian32(response), milliamps);
    uint8_t payload[32] = {0x50, 0x4b, 0x4e, 0x53};
    // First fixed sink PDO advertises Higher Capability, since 5 V cannot run
    // this board. The second is fixed 20 V with the requested operating current.
    putLittleEndian32(payload + 4, (1UL << 28) | (100UL << 10) | 90);
    putLittleEndian32(payload + 8, (400UL << 10) | (milliamps / 10));
    if (!writeBytes(kDataMemoryWrite, payload, sizeof(payload))) return false;
    const uint8_t mask = 0x03;
    if (!writeBytes(kSelectSinkPdo, &mask, 1)) return false;
  } else {
    log.printf("PD REQUEST existing active current >=%u mA; verifying stability without configuration writes\n",
               milliamps);
  }

  const uint32_t started = millis();
  uint32_t stableSince = started;
  bool stable = false, responseSuccess = !wroteProfile;
  uint32_t lastResponse = 0xffffffff;
  while (uint32_t(millis() - started) < 5000) {
    if (wroteProfile) {
      if (!readBytes(kPdResponse, response, sizeof(response))) return false;
      const uint32_t raw = littleEndian32(response);
      if (raw != lastResponse) {
        log.printf("PD REQUEST response=0x%08lX\n", (unsigned long)raw);
        lastResponse = raw;
      }
      // CYPD3177 KBA documents command SUCCESS=0x02. Fresh matching RDO below
      // is also mandatory, so a stale success response cannot authorize loads.
      if (response[0] == 0x02) responseSuccess = true;
    }
    PdSnapshot current;
    if (!readSnapshot(current)) return false;
    if (responseSuccess && matches(current)) {
      if (!stable) { stableSince = millis(); stable = true; }
      if (uint32_t(millis() - stableSince) >= 500) {
        accepted = current;
        printSnapshot(log, current, 0);
        log.printf("PD REQUEST PASS active operating_mA=%lu maximum_mA=%lu stable_ms=%lu\n",
                   (unsigned long)current.operatingMilliamps(),
                   (unsigned long)current.limitMilliamps(),
                   (unsigned long)(millis() - stableSince));
        log.println("PD REQUEST IRQ acknowledged/released NOT CLAIMED; active contract verified by readback");
        return true;
      }
    } else stable = false;
    delay(25);
  }
  return fail("PD request timed out: no successful stable 20 V/>=%u mA active RDO", milliamps);
}

bool runPdDiagnostics(Stream &log) {
  PdDiagnostics pd;
  log.println("PD BEGIN CYPD3177 read-only; SDA40 SCL41 address0x08 100kHz timeout25ms");
  if (!pd.begin()) {
    log.printf("PD FAIL %s\n", pd.error());
    return false;
  }
  bool ok = true, communicationOk = true, powerRequestValidated = true;
  unsigned completeSamples = 0;
  PdSnapshot first, s;
  for (unsigned i = 0; i < 3; ++i) {
    if (!pd.readSnapshot(s)) {
      log.printf("PD FAIL sample%u %s\n", i, pd.error());
      ok = false;
      communicationOk = false;
      break;
    }
    ++completeSamples;
    printSnapshot(log, s, i);
    if (i == 0) {
      first = s;
      log.print("PD VERSION raw_address_order=");
      for (uint8_t value : s.versions) log.printf("%02X", value);
      log.println();
      log.println("PD VERSION NOT VERIFIED: raw register bytes cannot establish CYPD3177 silicon firmware revision; chip date code needed");
    }
    if (!validFixtureData(s)) {
      log.println("PD FAIL unexpected identity/attachment/20V fixed PDO/RDO/current bounds/mismatch; see raw snapshot");
      ok = false;
    }
    if (s.deviceMode != 0x95 || s.siliconId != 0x2004) communicationOk = false;
    // CY4533 guide section 2.4 specifies RDO operating current = configured
    // ISNK_COARSE + ISNK_FINE. Both pins are grounded on this board, so zero
    // is explainable configuration data, not a transport error. It establishes
    // no usable nonzero current allowance for the intended board loads.
    if (s.operatingMilliamps() == 0) powerRequestValidated = false;
    if (s.deviceMode != first.deviceMode || s.siliconId != first.siliconId ||
        memcmp(s.versions, first.versions, sizeof(s.versions)) != 0 ||
        s.pdStatus != first.pdStatus || s.typeCStatus != first.typeCStatus ||
        s.currentPdo != first.currentPdo || s.currentRdo != first.currentRdo) {
      log.println("PD FAIL identity/contract changed between complete snapshots");
      ok = false;
      communicationOk = false;
    }
    if (s.interruptLow != (s.interruptStatus != 0)) {
      log.println("PD FAIL GPIO39 disagrees with INTERRUPT register; inspect transition or wiring");
      ok = false;
      communicationOk = false;
    }
    if (i < 2) delay(100);
  }
  pd.end();
  log.println("PD IRQ EVENT TEST NOT RUN: no event generated or acknowledged; recorded pin level, pending status and naturally observed edges only");
  log.printf("PD COMMUNICATION %s complete_samples=%u identity/repeated-data/IRQ-level checks\n",
             communicationOk && completeSamples == 3 ? "PASS" : "FAIL", completeSamples);
  log.println("PD SOURCE ADVERTISEMENT is an adapter ceiling, not a negotiated board power budget; no load current measured");
  log.println("PD STATUS is also recorded raw; established-contract bit is not asserted by this diagnostic");
  if (ok && !powerRequestValidated) {
    log.printf("PD POWER_REQUEST UNVALIDATED operating_mA=0 matches grounded ISNK pins; RDO maximum_mA=%lu; source ceiling is not the requested allowance\n",
               (unsigned long)s.limitMilliamps());
    if (s.limitMilliamps() == 100)
      log.println("PD RDO 100mA maximum field is observed; its origin is not explained by available CYPD3177 documentation");
    log.println("PD SUMMARY PARTIAL communication passed; power request not validated");
    return false;
  }
  log.printf("PD SUMMARY %s read-only communication and stable active PDO/RDO\n", ok ? "PASS" : "FAIL");
  return ok && powerRequestValidated;
}
