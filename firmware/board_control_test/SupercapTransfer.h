#pragma once

#include "BoardControl.h"

// Real-supercap charging and supervised source transfer. Own this object and
// BoardControl from one task. service() samples at 20 ms; transition operations
// use the driver's bounded setters. No serial, PD I2C, or flash access occurs here.
// The application owns the status LED. After observed main loss, request cutoff
// at 20 seconds without a validated return, or sooner at the 7 V backup limit.
class SupercapTransfer {
 public:
  enum class State { Stopped, Charging, Ready, Backup, Returned, Fault, Cutoff };
  enum class CutoffReason { None, MainTimeout, LowVoltage };
  explicit SupercapTransfer(BoardControl &board) : board_(board) {}
  // The application must validate the actual input power budget first. Starting
  // is explicit and bounded; it does not happen automatically after boot/return.
  bool start(bool powerBudgetValidated);
  bool service();  // False for a latched fault; keep calling while active().
  bool stop();     // On backup, retains voltage and 20-second loss supervision.
  bool active() const { return monitoring_; }
  State state() const { return state_; }
  const char *stateName() const;
  const char *error() const { return error_; }
  const Status &lastStatus() const { return lastStatus_; }
  bool statusValid() const { return statusValid_; }
  float voltage() const { return lastStatus_.vcapV; }
  uint32_t samples() const { return samples_; }
  uint32_t lastSampleMs() const { return lastSampleAt_; }
  uint32_t maxServiceGapMs() const { return maxGapMs_; }
  // Latest episode: first observed invalid main status, then confirmed backup
  // duration. Values remain available after return and reset on the next loss.
  uint32_t firstLossMs() const { return firstLossAt_; }
  uint32_t backupDurationMs() const;
  // Counts detected main-loss episodes, including one awaiting backup PG/mux
  // confirmation. Reset by an explicit successful start().
  uint32_t sourceLossCount() const { return sourceLossCount_; }
  bool sourceLossActive() const { return lossActive_; }
  // Zero when no loss is active. Return must be validated within 20 seconds of
  // the first invalid main sample; backup PG confirmation does not restart it.
  uint32_t sourceLossElapsedMs() const;
  uint32_t sourceLossRemainingMs() const;
  bool backupArmed() const { return backupArmed_; }
  bool backupConfirmed() const { return backupConfirmed_; }
  // Historical for the whole explicit run; preserved across loss/return episodes.
  bool reachedReady() const { return reachedReady_; }
  // The application logs/commits its result, then deliberately calls
  // BoardControl::releaseBackupForShutdown(). No power-cut occurs in service().
  bool cutoffRequested() const { return state_ == State::Cutoff; }
  CutoffReason cutoffReason() const { return cutoffReason_; }
  const char *cutoffReasonName() const;
  bool cleanupOk() const { return cleanupOk_ && !shutdownPending_; }

 private:
  BoardControl &board_;
  State state_ = State::Stopped;
  CutoffReason cutoffReason_ = CutoffReason::None;
  Status lastStatus_;
  char error_[192] = {};
  bool statusValid_ = false, monitoring_ = false, backupArmed_ = false;
  bool backupConfirmed_ = false, reachedReady_ = false, lossActive_ = false;
  bool backupEnded_ = false;
  bool stopped_ = false, cleanupOk_ = true, shutdownPending_ = false;
  bool ceOffVerified_ = false, plateau_ = false, statLow_ = false;
  uint32_t startedAt_ = 0, lastSampleAt_ = 0, maxGapMs_ = 0, samples_ = 0;
  uint32_t firstLossAt_ = 0, backupConfirmedAt_ = 0, backupEndedAt_ = 0;
  uint32_t sourceLossCount_ = 0;
  uint32_t ceOffAt_ = 0, plateauAt_ = 0, statLowAt_ = 0;
  float plateauMin_ = 0, plateauMax_ = 0;
  bool fault(const char *reason);
  bool beginShutdown();
  bool finishShutdown(uint32_t now);
  bool clearExternalOutputs();
  bool requestCutoff(CutoffReason reason, uint32_t now);
  static bool mainValid(const Status &s);
};
