#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>

enum class Port : uint8_t { Vbus, FiveVoltVbus, VbusSs, FiveVoltSs };

struct ExpanderState {
  bool valid = false;
  uint32_t atMs = 0;
  uint8_t input[2] = {}, output[2] = {}, polarity[2] = {}, config[2] = {};
  uint16_t physicalInputs = 0;  // Input registers XOR polarity, including reserved pins.
};

struct AnalogReading {
  uint8_t gpio = 0;
  uint16_t raw = 0;       // Averaged 12-bit ADC count.
  uint32_t mv = 0;        // Averaged adjacent ADC conversions in millivolts.
  float currentA = 0;     // Nominal uncalibrated IMON estimate; NAN for TS/VCAP.
};

struct HardwareSnapshot {
  uint32_t atMs = 0;
  bool valid = false;     // Both expander snapshots valid; ADC/GPIO still sampled on failure.
  ExpanderState internal, external;
  // ADC order: TS, VCAP, DC IMON, J8 IMON, J7 IMON, J9 IMON, J10 IMON, USB IMON, backup IMON.
  AnalogReading adc[9];
  float vcapV = 0;
  uint32_t tsMv = 0;
  bool usbPg = false, dcPg = false, backupPg = false, chargerPg = false, chargerStat = false;
  bool boostPg = false, buckPg = false, ssBuckPg = false;
  bool extVbusPg = false, ext5vVbusPg = false, extSsPg = false, ext5vSsPg = false;
  bool jetsonOn = false, mainSelected = false, backupEnabled = false, statusLed = false;
  bool bootButtonLow = false;  // SW1/GPIO0 pressed; no external pull-up on this PCB.
  bool internalIrqLow = false, externalIrqLow = false, pdIrqLow = false;
  uint32_t internalIrqFalls = 0, internalIrqRises = 0, externalIrqFalls = 0, externalIrqRises = 0;
  uint32_t muxFalls = 0, muxRises = 0, pdIrqFalls = 0, pdIrqRises = 0;
};

// One main-loop owner for this object and Wire. No delay(), PG wait loops, serial,
// PD bus calls or flash writes. I2C transactions have a 25 ms timeout.
class BoardHardware {
 public:
  enum class Result : uint8_t { Idle, Running, Succeeded, Failed };
  enum class Operation : uint8_t { None, Reconcile, PrepareCharge, EnableCharge, DisableCharging, PortChange, RestorePor };
  bool beginMonitoring();  // GPIO42 low/D7 high; no expander register-data writes.
  bool poll();             // Read all ADC/GPIO and both full register sets; call ~100 ms.
  const HardwareSnapshot &snapshot() const { return snapshot_; }
  static const char *adcName(unsigned index);
  bool startReconcile();
  bool startPrepareCharge();  // Main+budget checked by caller; CE off, real TS/low ISET, boost PG, >=500 ms TS settling.
  bool startEnableCharge();   // Asserts CE only after prepared supply/real TS/current guards.
  bool startDisableCharging();  // Preempts any enable job; preserves ports and backup.
  bool startPort(Port port, bool enabled);  // J8 automatically prepares/turns off its buck.
  bool startRestorePor();  // Preempts jobs; all outputs off, then config/output FFFF, polarity 0000. GPIO42 retained.
  void service();          // At most one bounded stage per call; no internal poll loop.
  bool busy() const { return result_ == Result::Running; }
  Result result() const { return result_; }
  Operation operation() const { return operation_; }
  const char *error() const { return error_; }
  bool setBackup(bool enabled);  // Arm: current valid main, 10..21 V. Release refuses sole backup.
  bool setLed(bool enabled);
  bool releaseBackupForShutdown();  // Explicit last power action; no I2C/ready/source guard.

 private:
  HardwareSnapshot snapshot_;
  bool monitoring_ = false, reconciled_ = false, prepared_ = false;
  Result result_ = Result::Idle;
  Operation operation_ = Operation::None;
  uint8_t stage_ = 0, portMask_ = 0, expectedInternal_ = 0, expectedExternal_ = 0;
  Port port_ = Port::Vbus;
  bool portEnabled_ = false, pgTiming_ = false;
  uint32_t stageAt_ = 0, operationAt_ = 0, pgAt_ = 0, ceOffAt_ = 0, tsSelectedAt_ = 0;
  char error_[192] = {};
  struct IrqLine {
    BoardHardware *owner = nullptr;
    uint8_t gpio = 0;
    volatile uint32_t falls = 0, rises = 0;
  } irq_[4];  // Internal, external, mux, PD.
  mutable portMUX_TYPE irqMux_ = portMUX_INITIALIZER_UNLOCKED;
  static void ARDUINO_ISR_ATTR onInterrupt(void *argument);
  bool fail(const char *format, ...);
  bool failOperation();
  bool beginOperation(Operation operation, bool preempt = false);
  bool readPair(uint8_t address, uint8_t reg, uint8_t values[2]);
  bool writeRegister(uint8_t address, uint8_t reg, uint8_t value);
  bool readExpander(uint8_t address, ExpanderState &state);
  bool writeControl(uint8_t address, uint8_t value);
  bool verifyControlLow(uint8_t mask);
  bool forceCeLow();
  bool verifyOperational();
  bool verifyPor(uint8_t address);
  bool currentMain() const;
  bool freshInternal() const;
  bool waitPg(bool good, uint32_t timeoutMs = 2000);
  void advance(uint8_t stage);
  void succeed();
};
