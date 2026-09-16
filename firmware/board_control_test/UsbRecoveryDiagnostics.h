#pragma once

#include <Arduino.h>
#include <esp_event.h>

// Observes Arduino-ESP32 3.3.3 HWCDC without resetting USB or calling its
// state-changing isConnected()/operator bool(). One instance per firmware.
class UsbRecoveryDiagnostics {
 public:
  static constexpr uint32_t kEventCapacity = 24;
  enum class Kind : uint32_t { Boot = 1, BusReset, Rx, SofConnection, CdcConnection, ApplicationLine };
  struct Event { uint32_t timeMs, kind, value; };
  struct Snapshot {
    uint32_t session, resetReason, timeMs;
    uint32_t busResets, rxEvents, rxBytes, txEvents, txBytes;
    uint32_t applicationRxBytes, applicationLines;
    uint32_t sofKnown, sofConnected, cdcKnown, cdcConnected, connectionObservations;
    int32_t availableRx;  // -1 means HWCDC has no receive queue.
    uint32_t interruptRaw, interruptEnabled, interruptStatus;
    uint32_t head, count;
    Event events[kEventCapacity];
  };
  static constexpr size_t kBlobBytes = sizeof(Snapshot) + 5 * sizeof(uint32_t);
  using BetweenRows = void (*)();

  bool begin(uint32_t session);  // After Serial.begin(); installs task callbacks.
  void service();              // Every loop; read-only sampling at 250 ms.
  void observeConnection(bool connected);  // Pass an existing bool(Serial) result.
  void observeApplicationRx(int value, bool commandComplete = false);
  bool snapshot(Snapshot &out, bool previous = false) const;
  const char *priorSource() const;  // RTC_VALIDATED, NVS_IMPORTED, or NONE.
  size_t exportBlob(void *destination, size_t capacity) const;
  bool importPriorBlob(const void *blob, size_t length);  // Does not replace RTC prior.
  void dump(Stream &out, BetweenRows betweenRows = nullptr) const;
  // Independently dump a saved NVS record even when a separate RTC prior exists.
  static bool dumpBlob(Stream &out, const void *blob, size_t length,
                       BetweenRows betweenRows = nullptr);

 private:
  bool started_ = false, priorAvailable_ = false, priorFromNvs_ = false;
  uint32_t lastSampleMs_ = 0;
  Snapshot prior_ = {};
  static void eventCallback(void *, esp_event_base_t, int32_t, void *);
  static void printSnapshot(Stream &, const char *, const Snapshot &, BetweenRows);
};
