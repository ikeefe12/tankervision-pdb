#pragma once

#include <Arduino.h>
#include <Wire.h>

enum class Port : uint8_t { Vbus, FiveVoltVbus, Supervised, FiveVoltSupervised };

struct Status {
  uint8_t internalInputs = 0;
  uint8_t internalOutputInputs = 0;  // Actual voltage sensed at U26 P1.
  uint8_t internalOutputs = 0;      // U26 P1 output latch.
  uint8_t externalInputs = 0;
  uint8_t externalOutputInputs = 0;  // Actual voltage sensed at U43 P1.
  uint8_t externalOutputs = 0;      // U43 P1 output latch.
  bool usbPg = false, dcPg = false, backupPg = false;
  bool chargerPg = false, chargerStat = false;
  bool boostPg = false, buckPg = false, ssBuckPg = false;
  bool extVbusPg = false, ext5vVbusPg = false;
  bool extSsPg = false, ext5vSsPg = false, jetsonOn = false;
  // Raw PMUX_ST: high also occurs with the mux output in Hi-Z. Combine with PG.
  bool mainSelected = false;
  float vcapV = 0;
  uint32_t tsMv = 0;
};

// Blocking, bounded board operations for use by a test runner or final firmware.
// Call from one task, which must also own Wire. No interrupts or autonomous loop.
// Successful setters verify configuration, output latch and physical enable pins.
// PG is awaited for converter/port enable; disable PG decay is measured by caller.
class BoardControl {
 public:
  bool begin(Stream &log);
  bool readStatus(Status &status);  // On failure, leaves status untouched.
  bool setBoost(bool enabled);
  bool setBuck5V(bool enabled);
  bool setCharger(bool enabled);
  bool setChargeHighCurrent(bool enabled);
  // false selects the real NTC; true selects the fixed normal-temperature divider.
  // Requires CE low and waits >=100 ms after CE disable before switching TS.
  // After verified selection, waits 100 ms for override or 500 ms for real NTC
  // (TS RC/deglitch settling). This verifies the select pin, not TS pin voltage.
  bool setThermistorOverride(bool enabled);
  bool setPort(Port port, bool enabled);
  bool setBackup(bool enabled);
  // Intentional final energy cutoff. Log/persist shutdown BEFORE calling: the
  // CPU may lose power inside this function. Independent of begin/I2C/main PG.
  // Unlike setBackup(false), this can release the board's sole power source.
  bool releaseBackupForShutdown();
  // Bounded U26-only recovery for source loss or a failed normal status read.
  // Clears CE, verifies its latch/pin, waits >=100 ms, then clears boost.
  // Preserves backup, charger profile, buck and exterior outputs; no PG wait.
  // Returns false if either control cannot be verified off. No begin prerequisite.
  bool disableChargingForPowerLoss();
  bool setStatusLed(bool enabled);
  bool setJetsonButton(bool pressed);
  bool allOff();  // Preserves armed backup if main power is absent/uncertain.
  const char *error() const { return error_; }
  // Supported ADC1 pins: 1,2,3,5,6,7,8,9,10. UINT32_MAX means invalid argument.
  uint32_t readAdcMv(int pin);

 private:
  struct Registers { uint8_t input[2], output[2], polarity[2], config[2]; };
  enum class Pg : uint8_t { Boost, Buck, Vbus, FiveVoltVbus, Supervised, FiveVoltSupervised, Backup };
  Stream *log_ = nullptr;
  char error_[160] = {};
  bool ready_ = false, backupEnabled_ = false;
  uint8_t internalOutputs_ = 0, externalOutputs_ = 0;
  uint32_t chargerDisabledAt_ = 0;
  void clearError() { error_[0] = '\0'; }
  bool fail(const char *format, ...);
  bool requireReady();
  bool failInitialization();
  bool readPair(uint8_t address, uint8_t reg, uint8_t *values);
  bool writeRegister(uint8_t address, uint8_t reg, uint8_t value);
  bool readRegisters(uint8_t address, Registers &registers);
  bool validate(uint8_t address, const Registers &registers, uint8_t expected);
  bool readStatusImpl(Status &status);
  bool writeOutputs(uint8_t address, uint8_t value);
  bool waitPg(Pg pg, bool requireMain, uint32_t timeoutMs = 2000);
  bool setBuckImpl(bool enabled);
  bool setChargerImpl(bool enabled);
  bool mainPresent(const Status &status) const;
  void waitForChargerDecay();
};
