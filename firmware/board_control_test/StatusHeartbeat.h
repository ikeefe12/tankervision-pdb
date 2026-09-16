#pragma once

#include "BoardControl.h"

// Call from the main loop after BoardControl::begin(). The LED toggles only when
// service() runs, so a blocked loop visibly stops the heartbeat. Own D7 through
// this helper while active; other code must not independently write the LED.
class StatusHeartbeat {
 public:
  explicit StatusHeartbeat(BoardControl &board) : board_(board) {}
  bool begin();    // Establish LED off, start the 500 ms off/on cadence.
  bool service();  // At most one toggle per call; never blocks or catches up.
  bool stop();     // Stop scheduling and request LED off; may be retried on fault.
  bool level() const { return level_; }  // Last successfully verified LED level.
  uint32_t transitions() const { return transitions_; }  // Since successful begin.
  const char *error() const { return error_; }

 private:
  BoardControl &board_;
  bool running_ = false, level_ = false;
  uint32_t lastTransitionAt_ = 0, transitions_ = 0;
  char error_[160] = {};
  bool outputFailed();
};
