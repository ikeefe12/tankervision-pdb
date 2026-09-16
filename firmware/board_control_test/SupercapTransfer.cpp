#include "SupercapTransfer.h"

#include <math.h>
#include <stdio.h>

namespace {
constexpr uint32_t kSampleMs = 20, kChargeTimeoutMs = 2UL * 60 * 60 * 1000;
constexpr uint32_t kPlateauMs = 30000, kStatLowMs = 100, kDecayMs = 100;
constexpr uint32_t kMainReturnGraceMs = 20000;
constexpr float kArmV = 10.0f, kCutoffV = 7.0f, kTripV = 21.0f;
constexpr float kFullMinV = 20.0f, kFullMaxV = 20.95f, kPlateauSpanV = 0.15f;
constexpr uint32_t kTsMinMv = 1300, kTsMaxMv = 2350;
constexpr uint8_t kChargingOutputs = 0x03;  // Boost + CE; real TS, low ISET.
}

bool SupercapTransfer::mainValid(const Status &s) {
  return s.mainSelected && (s.usbPg || s.dcPg);
}

const char *SupercapTransfer::stateName() const {
  switch (state_) {
    case State::Stopped: return "STOPPED";
    case State::Charging: return "CHARGING";
    case State::Ready: return "READY";
    case State::Backup: return "BACKUP";
    case State::Returned: return "RETURNED";
    case State::Fault: return "FAULT";
    case State::Cutoff: return "CUTOFF";
  }
  return "UNKNOWN";
}

uint32_t SupercapTransfer::backupDurationMs() const {
  if (!backupConfirmed_) return 0;
  return (backupEnded_ ? backupEndedAt_ : millis()) - backupConfirmedAt_;
}

uint32_t SupercapTransfer::sourceLossElapsedMs() const {
  return lossActive_ ? millis() - firstLossAt_ : 0;
}

uint32_t SupercapTransfer::sourceLossRemainingMs() const {
  if (!lossActive_) return 0;
  const uint32_t elapsed = sourceLossElapsedMs();
  return elapsed >= kMainReturnGraceMs ? 0 : kMainReturnGraceMs - elapsed;
}

const char *SupercapTransfer::cutoffReasonName() const {
  switch (cutoffReason_) {
    case CutoffReason::None: return "NONE";
    case CutoffReason::MainTimeout: return "MAIN_TIMEOUT";
    case CutoffReason::LowVoltage: return "LOW_VOLTAGE";
  }
  return "UNKNOWN";
}

bool SupercapTransfer::requestCutoff(CutoffReason reason, uint32_t now) {
  if (backupConfirmed_ && !backupEnded_) {
    backupEndedAt_ = now;
    backupEnded_ = true;
  }
  cutoffReason_ = reason;
  state_ = State::Cutoff;
  monitoring_ = false;
  // Do not start another I2C cleanup here: an unavailable bus must not delay the
  // deadline. The application records the reason, then releases GPIO42 directly.
  return true;
}

bool SupercapTransfer::clearExternalOutputs() {
  // Attempt every independent disable even when another one failed.
  bool ok = board_.setPort(Port::FiveVoltVbus, false);
  ok = board_.setPort(Port::Vbus, false) && ok;
  ok = board_.setPort(Port::Supervised, false) && ok;
  ok = board_.setPort(Port::FiveVoltSupervised, false) && ok;
  ok = board_.setJetsonButton(false) && ok;
  return ok;
}

bool SupercapTransfer::beginShutdown() {
  // Do not use allOff(): it can drop an armed backup while main remains valid.
  // A loss may precede mux/PG propagation, so retain GPIO42 unconditionally here.
  shutdownPending_ = true;
  ceOffVerified_ = board_.setCharger(false);
  if (!ceOffVerified_) {
    // An unrelated expander/status fault must not prevent an independent attempt
    // to lower CE. This emergency fallback can block for the 100 ms decay time.
    ceOffVerified_ = board_.disableChargingForPowerLoss();
  }
  if (ceOffVerified_) ceOffAt_ = millis();
  const bool outputsOff = clearExternalOutputs();
  cleanupOk_ = ceOffVerified_ && outputsOff;
  return cleanupOk_;
}

bool SupercapTransfer::finishShutdown(uint32_t now) {
  if (!shutdownPending_) return cleanupOk_;
  if (!ceOffVerified_) {
    ceOffVerified_ = board_.setCharger(false);
    if (!ceOffVerified_) ceOffVerified_ = board_.disableChargingForPowerLoss();
    if (ceOffVerified_) ceOffAt_ = millis();
    cleanupOk_ = false;
    return false;
  }
  // Split the required CE-to-boost delay across service calls.
  if (now - ceOffAt_ < kDecayMs) return cleanupOk_;
  bool ok = clearExternalOutputs();
  ok = board_.setBuck5V(false) && ok;
  ok = board_.setBoost(false) && ok;
  cleanupOk_ = ok;
  if (ok) shutdownPending_ = false;
  return ok;
}

bool SupercapTransfer::fault(const char *reason) {
  if (!error_[0]) snprintf(error_, sizeof(error_), "%s", reason);
  state_ = State::Fault;
  plateau_ = false;
  monitoring_ = true;  // An armed bank still needs cutoff supervision.
  if (!shutdownPending_) beginShutdown();
  return false;
}

bool SupercapTransfer::start(bool powerBudgetValidated) {
  if (state_ == State::Cutoff) return false;
  if (state_ == State::Charging || state_ == State::Ready) return true;
  if (shutdownPending_) return false;
  error_[0] = '\0';
  stopped_ = false;
  cleanupOk_ = true;
  if (!powerBudgetValidated) return fault("Input power budget has not been validated");
  Status s;
  if (!board_.readStatus(s)) return fault(board_.error());
  lastStatus_ = s;
  statusValid_ = true;
  if (!mainValid(s) || s.usbPg == s.dcPg || !s.ssBuckPg)
    return fault("Charging requires one valid main source and backed 5 V PG");
  if (!isfinite(s.vcapV) || s.vcapV < 0 || s.vcapV >= kTripV)
    return fault("VCAP outside allowed charge-start range");

  // Startup is a bounded operation using the shared driver's verified setters.
  // The normal service path has no waits beyond these bounded transition calls.
  if (!board_.setCharger(false) || !clearExternalOutputs() ||
      !board_.setBuck5V(false) || !board_.setBoost(false) ||
      !board_.setChargeHighCurrent(false) || !board_.setThermistorOverride(false) ||
      !board_.setBoost(true)) return fault(board_.error());
  if (!board_.readStatus(s)) return fault(board_.error());
  lastStatus_ = s;
  if (s.tsMv < kTsMinMv || s.tsMv > kTsMaxMv)
    return fault("Real thermistor is outside the allowed charging window");
  if (!board_.setCharger(true)) return fault(board_.error());

  state_ = State::Charging;
  monitoring_ = true;
  startedAt_ = millis();
  lastSampleAt_ = startedAt_ - kSampleMs;
  samples_ = maxGapMs_ = 0;
  firstLossAt_ = backupConfirmedAt_ = backupEndedAt_ = 0;
  sourceLossCount_ = 0;
  lossActive_ = backupConfirmed_ = reachedReady_ = false;
  backupEnded_ = false;
  cutoffReason_ = CutoffReason::None;
  plateau_ = statLow_ = false;
  return service();
}

bool SupercapTransfer::stop() {
  if (state_ == State::Cutoff) return false;
  stopped_ = true;
  state_ = State::Stopped;
  plateau_ = false;
  monitoring_ = true;
  bool ok = beginShutdown();
  Status s;
  if (board_.readStatus(s)) {
    lastStatus_ = s;
    statusValid_ = true;
    if (mainValid(s) && backupArmed_) {
      const bool released = board_.setBackup(false);
      if (released) backupArmed_ = false;
      ok = released && ok;
    }
  } else {
    statusValid_ = false;
    ok = false;
  }
  if (!ok) return fault("Requested stop could not verify all shutdown operations");
  return true;
}

bool SupercapTransfer::service() {
  if (!monitoring_) return state_ != State::Fault;
  const uint32_t now = millis(), gap = now - lastSampleAt_;
  const bool deadlineDue = lossActive_ && now - firstLossAt_ >= kMainReturnGraceMs;
  // At the deadline bypass the normal sample interval for one last main-source
  // check. A valid return in this call cancels cutoff before it is requested.
  if (gap < kSampleMs && !deadlineDue) return state_ != State::Fault;
  if (samples_ && gap > maxGapMs_) maxGapMs_ = gap;
  lastSampleAt_ = now;
  Status s;
  if (!board_.readStatus(s)) {
    statusValid_ = false;
    if (lossActive_ && millis() - firstLossAt_ >= kMainReturnGraceMs)
      return requestCutoff(CutoffReason::MainTimeout, millis());
    // Retry bounded shutdown operations while preserving the bank's only path.
    fault(board_.error());
    finishShutdown(millis());
    if (lossActive_ && millis() - firstLossAt_ >= kMainReturnGraceMs)
      return requestCutoff(CutoffReason::MainTimeout, millis());
    return false;
  }
  lastStatus_ = s;
  statusValid_ = true;
  ++samples_;

  // Source loss takes priority over charger/TS faults: those unbacked domains
  // are expected to collapse after input removal. PG and mux need not agree in
  // the first sampled frame during their propagation delays.
  const bool main = mainValid(s);
  if (!main && !lossActive_) {
    lossActive_ = true;
    ++sourceLossCount_;
    firstLossAt_ = now;
    backupConfirmed_ = false;
    backupConfirmedAt_ = backupEndedAt_ = 0;
    backupEnded_ = false;
    plateau_ = false;
    if (state_ != State::Fault && !stopped_) state_ = State::Backup;
    if (!beginShutdown()) fault("Could not verify main-load shutdown after source loss");
    if (!backupArmed_) fault("Main source lost before backup was armed");
  }
  const bool onBackup = !s.mainSelected && s.backupPg && s.ssBuckPg && backupArmed_;
  if (lossActive_ && onBackup && !backupConfirmed_) {
    backupConfirmed_ = true;
    backupConfirmedAt_ = now;
  }
  if (lossActive_ && main) {
    lossActive_ = false;
    if (backupConfirmed_ && !backupEnded_) {
      backupEndedAt_ = now;
      backupEnded_ = true;
    }
    if (state_ != State::Fault && !stopped_) state_ = State::Returned;
    // No recharge on any input return. Keep watching for subsequent loss while
    // retaining backup and the finished episode's duration. Only an explicit
    // start() with a revalidated power budget can restart charging.
  }

  if (lossActive_ && millis() - firstLossAt_ >= kMainReturnGraceMs)
    return requestCutoff(CutoffReason::MainTimeout, millis());

  finishShutdown(millis());
  if (lossActive_ && millis() - firstLossAt_ >= kMainReturnGraceMs)
    return requestCutoff(CutoffReason::MainTimeout, millis());
  if (!isfinite(s.vcapV) || s.vcapV < 0) return fault("VCAP ADC returned an invalid value");
  if (s.vcapV >= kTripV && state_ != State::Fault)
    return fault("VCAP reached the 21.0 V ADC overvoltage guard");
  if (!main && backupArmed_ && s.vcapV <= kCutoffV) {
    return requestCutoff(CutoffReason::LowVoltage, millis());
  }
  if (!s.ssBuckPg && state_ != State::Fault)
    return fault("Backed 5 V PG fell during the supervised test");
  if (backupArmed_ && !s.backupPg && state_ != State::Fault)
    return fault("Armed backup switch lost PG");

  if (state_ == State::Charging || state_ == State::Ready) {
    if (!s.boostPg || !s.chargerPg || s.internalOutputs != kChargingOutputs ||
        (s.internalOutputInputs & 0x1f) != kChargingOutputs ||
        s.externalOutputs != 0 || (s.externalOutputInputs & 0x1f) != 0)
      return fault("Charger supply PG or expected enable state changed");
    if (s.tsMv < kTsMinMv || s.tsMv > kTsMaxMv)
      return fault("Real thermistor moved outside the allowed charging window");
    if (!s.chargerStat) {
      if (!statLow_) { statLow_ = true; statLowAt_ = now; }
      if (now - statLowAt_ >= kStatLowMs)
        return fault("Charger STAT stayed low with charging enabled");
    } else statLow_ = false;
    if (!backupArmed_ && s.vcapV >= kArmV) {
      const bool armed = board_.setBackup(true);
      // A failed arm can either roll back or preserve GPIO42 after main loss;
      // record the actual pin, not just the setter's success result.
      backupArmed_ = digitalRead(42) == HIGH;
      if (!armed) return fault(board_.error());
    }
    if (state_ == State::Ready && s.vcapV < kFullMinV) {
      state_ = State::Charging;
      plateau_ = false;
    }
    if (state_ == State::Charging) {
      const bool nearFull = s.vcapV >= kFullMinV && s.vcapV <= kFullMaxV && s.chargerStat;
      if (nearFull) {
        if (!plateau_) {
          plateau_ = true;
          plateauAt_ = now;
          plateauMin_ = plateauMax_ = s.vcapV;
        }
        plateauMin_ = fminf(plateauMin_, s.vcapV);
        plateauMax_ = fmaxf(plateauMax_, s.vcapV);
        if (plateauMax_ - plateauMin_ > kPlateauSpanV) {
          plateauAt_ = now;
          plateauMin_ = plateauMax_ = s.vcapV;
        }
        if (now - plateauAt_ >= kPlateauMs && backupArmed_ && s.backupPg) {
          state_ = State::Ready;
          reachedReady_ = true;
        }
      } else plateau_ = false;
      if (state_ == State::Charging && now - startedAt_ >= kChargeTimeoutMs)
        return fault("Charge did not reach a stable full-voltage plateau within two hours");
    }
  }
  if (stopped_ && !backupArmed_ && !shutdownPending_ && main) monitoring_ = false;
  return state_ != State::Fault;
}
