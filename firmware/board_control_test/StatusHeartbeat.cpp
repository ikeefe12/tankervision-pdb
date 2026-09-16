#include "StatusHeartbeat.h"

#include <stdio.h>

namespace {
constexpr uint32_t kHalfPeriodMs = 500;
}

bool StatusHeartbeat::outputFailed() {
  running_ = false;
  snprintf(error_, sizeof(error_), "%s", board_.error()[0] ? board_.error() : "Status LED write/readback failed");
  return false;
}

bool StatusHeartbeat::begin() {
  running_ = false;
  if (!board_.setStatusLed(false)) return outputFailed();
  level_ = false;
  transitions_ = 0;
  lastTransitionAt_ = millis();
  error_[0] = '\0';
  running_ = true;
  return true;
}

bool StatusHeartbeat::service() {
  if (!running_) return error_[0] == '\0';
  const uint32_t now = millis();
  if (uint32_t(now - lastTransitionAt_) < kHalfPeriodMs) return true;
  const bool next = !level_;
  if (!board_.setStatusLed(next)) return outputFailed();
  level_ = next;
  ++transitions_;
  // Start the next half-period at this actual operation. A delayed main loop
  // causes one late edge, never a burst of toggles to replay elapsed intervals.
  lastTransitionAt_ = millis();
  return true;
}

bool StatusHeartbeat::stop() {
  running_ = false;
  if (!board_.setStatusLed(false)) return outputFailed();
  if (level_) ++transitions_;
  level_ = false;
  error_[0] = '\0';
  return true;
}
