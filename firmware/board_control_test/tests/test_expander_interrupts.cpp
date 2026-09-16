// Compile the actual interrupt monitor with synchronous GPIO/ISR injection and
// a status-read stub. This verifies software bookkeeping, not electrical timing.
#include "ExpanderInterrupts.h"
#include "BoardControl.h"
#include <cassert>
#include <cstdio>

namespace {
int levels[50] = {};
void (*handlers[50])(void *) = {};
void *contexts[50] = {};
bool failRead = false, pulseDuringRead = false;
uint32_t now = 0;

void edge(int pin, int value) {
  levels[pin] = value;
  if (handlers[pin]) handlers[pin](contexts[pin]);
}
}

uint32_t micros() { return ++now; }
void pinMode(int, int) {}
void attachInterruptArg(int pin, void (*handler)(void *), void *argument, int mode) {
  assert(mode == CHANGE);
  handlers[pin] = handler;
  contexts[pin] = argument;
}
void detachInterrupt(int pin) { handlers[pin] = nullptr; }
int gpio_get_level(int pin) { return levels[pin]; }

bool BoardControl::readStatus(Status &status) {
  if (failRead) return false;
  if (pulseDuringRead) {
    edge(15, LOW);
    edge(15, HIGH);
    pulseDuringRead = false;
  } else {
    if (levels[15] == LOW) edge(15, HIGH);
    if (levels[16] == LOW) edge(16, HIGH);
  }
  status.internalInputs = 0xac;
  status.externalInputs = 0;
  return true;
}

int main() {
  levels[15] = levels[16] = HIGH;
  ExpanderInterrupts irq;
  BoardControl board;
  Status status;
  ExpanderInterrupts::AckResult ack;
  irq.begin();
  assert(irq.snapshot().internal.fallingEdges == 0);

  edge(15, LOW);
  const auto asserted = irq.snapshot();
  assert(asserted.internal.activeLow && asserted.internal.pending);
  assert(asserted.internal.fallingEdges == 1 && asserted.internal.lastFallingUs > 0);
  assert(asserted.external.fallingEdges == 0 && !asserted.external.pending);
  puts("PASS internal/external edge isolation and timestamp capture");

  assert(irq.acknowledge(board, status, ack));
  assert(ack.before.internal.activeLow && !ack.after.internal.activeLow);
  assert(!ack.after.internal.pending && ack.after.internal.risingEdges == 1);
  assert(ack.after.internal.lastRisingUs > ack.before.internal.lastFallingUs);
  puts("PASS asserted line observed released after successful input read");

  edge(16, LOW);
  edge(16, HIGH);
  assert(irq.snapshot().external.pending && !irq.snapshot().external.activeLow);
  assert(irq.acknowledge(board, status, ack));
  assert(!ack.after.external.pending && ack.after.external.fallingEdges == 1);
  assert(ack.after.external.risingEdges == 1);
  puts("PASS short pulse remains pending until acknowledged");

  edge(15, LOW);
  failRead = true;
  status.internalInputs = 99;
  assert(!irq.acknowledge(board, status, ack));
  assert(ack.after.internal.pending && ack.after.internal.activeLow);
  assert(status.internalInputs == 99);
  failRead = false;
  assert(irq.acknowledge(board, status, ack));
  puts("PASS failed read preserves pending event and caller status");

  pulseDuringRead = true;
  assert(irq.acknowledge(board, status, ack));
  assert(ack.after.internal.pending && !ack.after.internal.activeLow);
  assert(ack.after.internal.fallingEdges == ack.before.internal.fallingEdges + 1);
  assert(irq.acknowledge(board, status, ack));
  assert(!ack.after.internal.pending);
  puts("PASS event during acknowledge retained for subsequent service");

  irq.end();
  levels[15] = LOW;
  irq.begin();
  assert(irq.snapshot().internal.pending && irq.snapshot().internal.fallingEdges == 0);
  irq.end();
  puts("PASS initially asserted input flagged without invented falling edge");

  {
    ExpanderInterrupts scoped;
    scoped.begin();
    assert(handlers[15] && handlers[16]);
  }
  assert(!handlers[15] && !handlers[16]);
  puts("PASS scoped monitor detaches handlers before destruction");
  puts("7 expander interrupt host tests passed");
}
