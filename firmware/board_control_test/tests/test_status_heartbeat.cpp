#include "StatusHeartbeat.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
struct Edge { bool level; uint32_t at; };
uint32_t clockMs;
bool boardReady, failWrite, led;
std::vector<Edge> writes;

void reset() {
  clockMs = 1000;
  boardReady = true;
  failWrite = led = false;
  writes.clear();
}
}

uint32_t millis() { return clockMs; }

// This link deliberately provides only the LED driver operation. A heartbeat
// dependency on delay(), Wire, ADC, or status reads would fail to link.
bool BoardControl::setStatusLed(bool enabled) {
  if (!boardReady || failWrite) {
    snprintf(error_, sizeof(error_), "%s", boardReady ? "GPIO4 readback mismatch" : "BoardControl not initialized");
    return false;
  }
  error_[0] = '\0';
  led = enabled;
  writes.push_back({enabled, clockMs});
  return true;
}

static void cadenceAndLoopLiveness() {
  reset(); BoardControl board; StatusHeartbeat heartbeat(board);
  assert(heartbeat.service());
  assert(writes.empty());  // No heartbeat before an explicit begin.
  assert(heartbeat.begin());
  assert(!led && !heartbeat.level() && heartbeat.transitions() == 0);
  clockMs += 499;
  assert(heartbeat.service());
  assert(writes.size() == 1);
  ++clockMs;
  assert(heartbeat.service());
  assert(led && heartbeat.level() && heartbeat.transitions() == 1);
  clockMs += 500;
  assert(heartbeat.service());
  assert(!led && heartbeat.transitions() == 2);
  // Time without loop service cannot produce a hidden timer/PWM transition.
  clockMs += 7000;
  assert(!led && heartbeat.transitions() == 2 && writes.size() == 3);
}

static void blockedLoopProducesOneEdgeWithoutCatchup() {
  reset(); BoardControl board; StatusHeartbeat heartbeat(board);
  assert(heartbeat.begin());
  clockMs += 7250;
  assert(heartbeat.service());
  assert(led && heartbeat.transitions() == 1 && writes.size() == 2);
  for (unsigned i = 0; i < 10; ++i) assert(heartbeat.service());
  assert(writes.size() == 2);
  clockMs += 499;
  assert(heartbeat.service());
  assert(led && writes.size() == 2);
  ++clockMs;
  assert(heartbeat.service());
  assert(!led && writes.size() == 3);
  assert(uint32_t(writes.back().at - writes[1].at) == 500);
}

static void cadenceSurvivesMillisRollover() {
  reset(); clockMs = UINT32_MAX - 200;
  BoardControl board; StatusHeartbeat heartbeat(board);
  assert(heartbeat.begin());
  clockMs += 499;
  assert(heartbeat.service());
  assert(!led && heartbeat.transitions() == 0);
  ++clockMs;
  assert(heartbeat.service());
  assert(led && heartbeat.transitions() == 1);
  clockMs += 500;
  assert(heartbeat.service());
  assert(!led && heartbeat.transitions() == 2);
}

static void stopAndRestartAreExplicit() {
  reset(); BoardControl board; StatusHeartbeat heartbeat(board);
  assert(heartbeat.begin());
  clockMs += 500;
  assert(heartbeat.service());
  assert(heartbeat.stop());
  assert(!led && !heartbeat.level() && heartbeat.transitions() == 2);
  const size_t stoppedWrites = writes.size();
  clockMs += 5000;
  assert(heartbeat.service());
  assert(writes.size() == stoppedWrites);
  assert(heartbeat.begin());
  assert(heartbeat.transitions() == 0);
  clockMs += 500;
  assert(heartbeat.service());
  assert(led && heartbeat.transitions() == 1);
}

static void failedPinVerificationLatchesUntilExplicitRecovery() {
  reset(); BoardControl board; StatusHeartbeat heartbeat(board);
  boardReady = false;
  assert(!heartbeat.begin());
  assert(strstr(heartbeat.error(), "not initialized"));
  assert(!heartbeat.service() && writes.empty());
  boardReady = true;
  assert(heartbeat.begin());
  clockMs += 500;
  failWrite = true;
  assert(!heartbeat.service());
  assert(strstr(heartbeat.error(), "GPIO4"));
  assert(!heartbeat.level() && heartbeat.transitions() == 0);
  failWrite = false;
  clockMs += 5000;
  assert(!heartbeat.service());
  assert(writes.size() == 1);  // No autonomous retry after a fault.
  assert(heartbeat.stop());
  assert(heartbeat.error()[0] == '\0' && !led);
  assert(heartbeat.begin());
  clockMs += 500;
  assert(heartbeat.service());
  failWrite = true;
  assert(!heartbeat.stop());
  assert(heartbeat.level() && heartbeat.transitions() == 1);
  failWrite = false;
  assert(heartbeat.stop());
  assert(!led && heartbeat.transitions() == 2);
}

int main() {
  cadenceAndLoopLiveness();
  blockedLoopProducesOneEdgeWithoutCatchup();
  cadenceSurvivesMillisRollover();
  stopAndRestartAreExplicit();
  failedPinVerificationLatchesUntilExplicitRecovery();
  std::puts("PASS heartbeat cadence, loop liveness, no catch-up, millis wrap, stop/restart, GPIO fault recovery");
}
