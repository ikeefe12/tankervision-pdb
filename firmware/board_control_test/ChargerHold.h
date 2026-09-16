#pragma once

#include "BoardControl.h"

// Unloaded VCAP measurement mode. The caller must service() frequently from the
// same task as BoardControl; this class never waits for or writes to USB serial.
// No timeout while holding. Stop explicitly before connecting a supercap/load.
class ChargerHold {
 public:
  enum class State { Stopped, Starting, Holding, Fault };
  explicit ChargerHold(BoardControl &board) : board_(board) {}
  bool start();  // Starts regulation qualification; poll service() to completion.
  bool service();  // False on a latched fault; performs best-effort shutdown.
  bool stop();
  bool active() const { return state_ == State::Starting || state_ == State::Holding; }
  State state() const { return state_; }
  const char *stateName() const;
  const char *error() const { return error_; }
  bool cleanupOk() const { return cleanupOk_; }
  uint32_t samples() const { return samples_; }
  uint32_t maxServiceGapMs() const { return maxGapMs_; }
  float voltage() const { return voltage_; }
  float minVoltage() const { return minV_; }
  float maxVoltage() const { return maxV_; }

 private:
  BoardControl &board_;
  State state_ = State::Stopped;
  char error_[192] = {};
  bool cleanupOk_ = true, qualifying_ = false;
  uint32_t startedAt_ = 0, lastSampleAt_ = 0, stableAt_ = 0;
  uint32_t samples_ = 0, maxGapMs_ = 0;
  float voltage_ = 0, minV_ = 0, maxV_ = 0;
  bool trip(const char *reason);
};
