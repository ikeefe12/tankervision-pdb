#pragma once

#include <Arduino.h>
#include <Wire.h>

struct PdSnapshot {
  uint8_t deviceMode = 0;
  uint16_t siliconId = 0;
  uint8_t versions[16] = {};  // Raw READ_ALL_VERSION bytes, address order.
  uint32_t pdStatus = 0;
  uint8_t typeCStatus = 0;
  uint32_t currentPdo = 0, currentRdo = 0;
  uint8_t interruptStatus = 0;
  bool interruptLow = false;
  uint32_t interruptFallingEdges = 0, interruptRisingEdges = 0;

  bool attached() const { return typeCStatus & 1; }
  bool fixedSupply() const { return (currentPdo >> 30) == 0; }
  uint32_t selectedMillivolts() const { return ((currentPdo >> 10) & 0x3ff) * 50; }
  uint32_t sourceMilliamps() const { return (currentPdo & 0x3ff) * 10; }
  uint32_t operatingMilliamps() const { return ((currentRdo >> 10) & 0x3ff) * 10; }
  uint32_t limitMilliamps() const { return (currentRdo & 0x3ff) * 10; }
  uint8_t objectPosition() const { return (currentRdo >> 28) & 7; }
  bool capabilityMismatch() const { return currentRdo & (1UL << 26); }
  bool giveBack() const { return currentRdo & (1UL << 27); }
};

// CYPD3177 diagnostics/control on the dedicated PD I2C controller. Own this
// object and its TwoWire(1) bus from one task; never invoke I2C from an ISR.
// begin()/readSnapshot() send read pointers only. Configuration writes occur
// only in the explicitly invoked requestFixed20VCurrent(). No reset is sent.
// begin()/end() exclusively own GPIO39's interrupt handler while active.
class PdDiagnostics {
 public:
  ~PdDiagnostics() { end(); }
  PdDiagnostics() = default;
  PdDiagnostics(const PdDiagnostics &) = delete;
  PdDiagnostics &operator=(const PdDiagnostics &) = delete;
  bool begin();
  void end();
  bool readSnapshot(PdSnapshot &snapshot);  // Failure leaves argument untouched.
  // Call only with downstream converters/charger/ports disabled: a PD contract
  // change can interrupt input power. Requests a volatile 5 V/900 mA fallback
  // plus a 20 V fixed PDO at the requested current (100..3000 mA, steps of 10).
  // Requires an already attached 20 V source advertising sufficient current.
  // Success needs matching active PDO/RDO stable for 500 ms; failure leaves
  // accepted untouched and never grants permission to enable loads. The host
  // must independently recheck its board input PG before enabling its load.
  bool requestFixed20VCurrent(uint16_t milliamps, Stream &log, PdSnapshot &accepted);
  const char *error() const { return error_; }

 private:
  TwoWire bus_{1};
  bool ready_ = false;
  volatile uint32_t fallingEdges_ = 0, risingEdges_ = 0;
  char error_[128] = {};
  static void ARDUINO_ISR_ATTR interruptHandler(void *context);
  bool readBytes(uint16_t address, uint8_t *data, size_t length);
  bool writeBytes(uint16_t address, const uint8_t *data, size_t length);
  bool fail(const char *format, ...);
};

// Bounded fixture test: three complete, stable 20 V fixed-supply snapshots.
// Success covers communication, decoded active PDO/RDO data, and IRQ level/status
// consistency. It does not claim a generated/acknowledged PD interrupt event.
// Zero requested operating current produces PARTIAL and returns false even when
// communication passes, because a nonzero power allowance is not validated.
bool runPdDiagnostics(Stream &log);
