#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>

class BoardControl;
struct Status;

// GPIO interrupt capture only: no I2C, Serial or board-control calls in an ISR.
// One instance owns the board's two INT pins until end() or destruction. Call
// begin() after BoardControl.begin(); all methods other than the ISR belong to
// the Wire task, including destruction (which detaches the GPIO handlers).
class ExpanderInterrupts {
 public:
  struct LineSnapshot {
    uint32_t fallingEdges = 0;
    uint32_t risingEdges = 0;
    uint32_t lastFallingUs = 0;
    uint32_t lastRisingUs = 0;
    bool activeLow = false;  // Live GPIO level: true means INT asserted.
    bool pending = false;    // Software service flag; may outlive a short pulse.
  };
  struct Snapshot {
    LineSnapshot internal;  // U26, address 0x20, GPIO15.
    LineSnapshot external;  // U43, address 0x21, GPIO16.
    uint32_t capturedAtUs = 0;
  };
  struct AckResult {
    Snapshot before;
    Snapshot after;
  };

  ExpanderInterrupts() = default;
  ~ExpanderInterrupts() { end(); }
  ExpanderInterrupts(const ExpanderInterrupts &) = delete;
  ExpanderInterrupts &operator=(const ExpanderInterrupts &) = delete;
  void begin();  // Resets counters; uses the board's existing 10-kohm pull-ups.
  void end();
  Snapshot snapshot() const;  // No I2C or flag clearing; safe between setters.

  // Read both expander input-register pairs using BoardControl's validation.
  // Return value reports that read's success; status is unchanged on failure.
  // Inspect after.*.activeLow for the observed release of each hardware INT.
  // A fresh input change may reassert INT during/after the read; it is retained
  // as pending, and the caller must service it again in its normal task loop.
  bool acknowledge(BoardControl &board, Status &status, AckResult &result);

 private:
  struct Line {
    ExpanderInterrupts *owner = nullptr;
    int pin = 0;
    volatile uint32_t fallingEdges = 0;
    volatile uint32_t risingEdges = 0;
    volatile uint32_t lastFallingUs = 0;
    volatile uint32_t lastRisingUs = 0;
    volatile bool pending = false;
  };
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  Line internal_, external_;
  bool attached_ = false;
  static void ARDUINO_ISR_ATTR onEdge(void *argument);
  static LineSnapshot copyLine(const Line &line);
};
