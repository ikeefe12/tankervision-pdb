#pragma once
#include <Arduino.h>
#include <Wire.h>

struct PdReading {
  bool valid = false;
  uint32_t atMs = 0, status = 0, pdo = 0, rdo = 0, response = 0;
  uint8_t mode = 0, typeC = 0, interrupt = 0, versions[16] = {};
  uint16_t silicon = 0;
  bool attached() const { return typeC & 1; }
  uint32_t millivolts() const { return ((pdo >> 10) & 1023) * 50; }
  uint32_t sourceMa() const { return (pdo & 1023) * 10; }
  uint32_t operatingMa() const { return ((rdo >> 10) & 1023) * 10; }
  uint32_t maximumMa() const { return (rdo & 1023) * 10; }
};

// CYPD3177 HPI: separate I2C bus, read-only monitoring during settling, volatile
// sink request only from startBudget() after all loads have been reconciled off.
class PowerDelivery {
 public:
  enum class Result { Idle, Running, Succeeded, Failed };
  static constexpr uint16_t kRequestedMa = 3000;
  bool begin();
  bool poll(bool usbMainPresent);
  bool startBudget();
  void service();
  void cancel() { result_ = Result::Idle; }
  const PdReading &reading() const { return reading_; }
  Result result() const { return result_; }
  bool budgetValid() const {
    return result_ == Result::Succeeded && reading_.valid &&
      millis() - reading_.atMs <= 1500 && matches(reading_);
  }
  const char *error() const { return error_; }
 private:
  TwoWire bus_{1};
  PdReading reading_;
  bool begun_ = false, wrote_ = false, responseOk_ = false, stable_ = false;
  uint8_t stage_ = 0;
  uint32_t startAt_ = 0, sampleAt_ = 0, stableAt_ = 0, originalPdo_ = 0, originalObject_ = 0;
  Result result_ = Result::Idle;
  char error_[128] = {};
  bool read(uint16_t address, uint8_t *bytes, size_t n);
  bool write(uint16_t address, const uint8_t *bytes, size_t n);
  bool fail(const char *message);
  bool sensible(const PdReading &) const;
  bool matches(const PdReading &) const;
};
