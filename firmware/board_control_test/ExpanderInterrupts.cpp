#include "ExpanderInterrupts.h"

#include "BoardControl.h"
#include <driver/gpio.h>

namespace {
constexpr int kInternalInterruptPin = 15;
constexpr int kExternalInterruptPin = 16;
}

void ExpanderInterrupts::begin() {
  end();
  internal_.owner = external_.owner = this;
  internal_.pin = kInternalInterruptPin;
  external_.pin = kExternalInterruptPin;
  internal_.fallingEdges = 0;
  internal_.risingEdges = 0;
  external_.fallingEdges = 0;
  external_.risingEdges = 0;
  internal_.lastFallingUs = 0;
  internal_.lastRisingUs = 0;
  external_.lastFallingUs = 0;
  external_.lastRisingUs = 0;
  internal_.pending = false;
  external_.pending = false;
  // R77 and R137 provide the pull-ups. Do not mask a missing board pull-up by
  // silently enabling the ESP32's internal pull-up during this test.
  pinMode(kInternalInterruptPin, INPUT);
  pinMode(kExternalInterruptPin, INPUT);
  attachInterruptArg(kInternalInterruptPin, onEdge, &internal_, CHANGE);
  attachInterruptArg(kExternalInterruptPin, onEdge, &external_, CHANGE);
  attached_ = true;
  portENTER_CRITICAL(&mux_);
  // A line already asserted at attach time is work to service, not a fabricated
  // falling edge. Any concurrent real edge remains pending through this OR.
  internal_.pending = internal_.pending || gpio_get_level(GPIO_NUM_15) == 0;
  external_.pending = external_.pending || gpio_get_level(GPIO_NUM_16) == 0;
  portEXIT_CRITICAL(&mux_);
}

void ExpanderInterrupts::end() {
  if (!attached_) return;
  detachInterrupt(kInternalInterruptPin);
  detachInterrupt(kExternalInterruptPin);
  attached_ = false;
}

void ARDUINO_ISR_ATTR ExpanderInterrupts::onEdge(void *argument) {
  auto *line = static_cast<Line *>(argument);
  const bool low = gpio_get_level(static_cast<gpio_num_t>(line->pin)) == 0;
  const uint32_t at = micros();
  portENTER_CRITICAL_ISR(&line->owner->mux_);
  if (low) {
    line->fallingEdges = line->fallingEdges + 1;
    line->lastFallingUs = at;
    line->pending = true;
  } else {
    line->risingEdges = line->risingEdges + 1;
    line->lastRisingUs = at;
  }
  portEXIT_CRITICAL_ISR(&line->owner->mux_);
}

ExpanderInterrupts::LineSnapshot ExpanderInterrupts::copyLine(const Line &line) {
  LineSnapshot next;
  next.fallingEdges = line.fallingEdges;
  next.risingEdges = line.risingEdges;
  next.lastFallingUs = line.lastFallingUs;
  next.lastRisingUs = line.lastRisingUs;
  next.pending = line.pending;
  next.activeLow = gpio_get_level(static_cast<gpio_num_t>(line.pin)) == 0;
  return next;
}

ExpanderInterrupts::Snapshot ExpanderInterrupts::snapshot() const {
  Snapshot next;
  portENTER_CRITICAL(&mux_);
  next.internal = copyLine(internal_);
  next.external = copyLine(external_);
  next.capturedAtUs = micros();
  portEXIT_CRITICAL(&mux_);
  return next;
}

bool ExpanderInterrupts::acknowledge(BoardControl &board, Status &status,
                                    AckResult &result) {
  result.before = snapshot();
  if (!board.readStatus(status)) {
    result.after = snapshot();
    return false;
  }
  // TCA9535 Rev E section 7.3.3: read of the affected input port clears its
  // interrupt at ACK; reading its other port does not. BoardControl reads both
  // ports of both devices. A return to the original input state also clears INT,
  // so edge counts alone cannot prove read-to-clear without a before-low sample.
  // Source: https://www.ti.com/lit/ds/symlink/tca9535.pdf
  portENTER_CRITICAL(&mux_);
  if (internal_.fallingEdges == result.before.internal.fallingEdges &&
      gpio_get_level(GPIO_NUM_15) != 0)
    internal_.pending = false;
  if (external_.fallingEdges == result.before.external.fallingEdges &&
      gpio_get_level(GPIO_NUM_16) != 0)
    external_.pending = false;
  portEXIT_CRITICAL(&mux_);
  result.after = snapshot();
  return true;
}
