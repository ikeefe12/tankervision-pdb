#include "PowerManager.h"

#include <cmath>

void PowerManager::begin(uint32_t now) {
  // Keep ids monotonic across explicit reinitialization so late adapter results
  // from a previous cycle cannot acknowledge a new action.
  const uint32_t previousId = nextActionId_;
  *this = PowerManager();
  nextActionId_ = previousId;
  begun_ = true;
  phaseAt_ = now;
}

bool PowerManager::voltageValid(float voltage) {
  return std::isfinite(voltage) && voltage >= 0.0f;
}

void PowerManager::cancelAction() {
  pending_ = {};
  completed_ = false;
}

void PowerManager::issueAction(Action action, uint32_t now) {
  cancelAction();
  if (++nextActionId_ == 0) ++nextActionId_;
  pending_ = {action, nextActionId_};
  pendingAt_ = now;
}

bool PowerManager::completeAction(uint32_t id, bool success) {
  if (pending_.kind == Action::None || pending_.id != id || completed_)
    return false;
  completed_ = true;
  completionSuccess_ = success;
  return true;
}

void PowerManager::leaveCharging(uint32_t issue) {
  issues_ |= issue;
  // A charger fault may interrupt J9 startup, but must not leave its incomplete
  // action marked done. Retry the interrupted startup step after charger cleanup.
  if (pending_.kind == Action::EnableJetson) jetsonAttempted_ = false;
  if (pending_.kind == Action::PingJetson) pingAttempted_ = false;
  if (pending_.kind == Action::ArmBackup) backupAttempted_ = false;
  cancelAction();
  if (phase_ == Phase::ChargePrepare || phase_ == Phase::Charging)
    phase_ = Phase::JetsonStarting;
  chargingEnabledExpected_ = false;
  maintenance_ = false;
  stopNeeded_ = true;
  stopAttempted_ = false;
}

void PowerManager::startShutdown(uint32_t now, ShutdownReason reason) {
  cancelAction();
  phase_ = Phase::ShutdownWait;
  shutdownReason_ = reason;
  shutdownAt_ = now;
  shutdownAckSeen_ = false;
  finalReadySeen_ = false;
  shutdownRequestSent_ = false;
  chargingEnabledExpected_ = false;
  maintenance_ = false;
  issueAction(Action::RequestJetsonShutdown, now);
}

bool PowerManager::requestFullReboot(uint32_t now) {
  if (!begun_ || phase_ == Phase::ShutdownWait || phase_ == Phase::PowerOff)
    return false;
  startShutdown(now, ShutdownReason::FullReboot);
  return true;
}

void PowerManager::finishAction(bool success, uint32_t now,
                                const Inputs &inputs) {
  const Action action = pending_.kind;
  cancelAction();
  switch (action) {
    case Action::PrepareCharging:
      if (!success) {
        leaveCharging(ChargePreparationFailed);
      } else {
        prepared_ = true;
        phaseAt_ = now;
      }
      break;
    case Action::EnableCharging:
      if (!success) {
        leaveCharging(ChargerEnableFailed);
      } else {
        phase_ = Phase::Charging;
        chargingEnabledExpected_ = true;
        chargeAt_ = progressAt_ = now;
        progressV_ = inputs.vcapV;
      }
      break;
    case Action::StopCharging:
      if (!success) issues_ |= ChargeStopFailed;
      stopNeeded_ = false;
      break;
    case Action::ArmBackup:
      backupArmed_ = success;
      if (!success) issues_ |= BackupArmFailed;
      break;
    case Action::EnableJetson:
      jetsonEnabled_ = success;
      if (!success) issues_ |= JetsonEnableFailed;
      break;
    case Action::PingJetson:
      if (!success) issues_ |= JetsonPingFailed;
      phase_ = Phase::Running;
      break;
    case Action::RequestJetsonShutdown:
      shutdownRequestSent_ = success;
      if (!success) issues_ |= ShutdownRequestFailed;
      break;
    case Action::CutPower:
      if (!success) issues_ |= PowerOffFailed;
      // No policy restart: actual esp_restart(), if power remains, is owned by
      // the adapter. ACK, main return, and subsequent ticks cannot re-enable.
      break;
    case Action::None:
      break;
  }
}

void PowerManager::tick(uint32_t now, const Inputs &inputs) {
  if (!begun_) return;
  if (inputs.jetsonPong) jetsonPongSeen_ = true;

  if (phase_ == Phase::PowerOff) {
    if (completed_) finishAction(completionSuccess_, now, inputs);
    return;
  }

  if (!inputs.statusValid) issues_ |= StatusUnavailable;
  if (inputs.statusValid && !voltageValid(inputs.vcapV))
    issues_ |= InvalidVoltage;

  // The ten-second settling window has no hardware actions. Explicit full
  // reboot remains available during it; main-loss monitoring starts afterward.
  if (phase_ == Phase::Settling) {
    if (static_cast<uint32_t>(now - phaseAt_) < kSettlingMs) return;
    phase_ = Phase::ChargePrepare;
    phaseAt_ = now;
    if (inputs.statusValid && !inputs.mainPresent)
      issues_ |= MainUnavailableAtStartup;
  }

  if (phase_ != Phase::ShutdownWait && inputs.statusValid &&
      !inputs.mainPresent)
    startShutdown(now, ShutdownReason::MainLoss);

  if (phase_ == Phase::ShutdownWait) {
    shutdownAckSeen_ |= inputs.shutdownAck;
    finalReadySeen_ |= inputs.finalReady;
    // Check ahead of all completions/timeouts. A hung operation, bad I2C sample,
    // ACK, ready message, or power return cannot defer the original deadline.
    if (static_cast<uint32_t>(now - shutdownAt_) >= kShutdownMs) {
      phase_ = Phase::PowerOff;
      issueAction(Action::CutPower, now);
      return;
    }
  }

  if (completed_) {
    finishAction(completionSuccess_, now, inputs);
  } else if (pending_.kind != Action::None &&
             static_cast<uint32_t>(now - pendingAt_) >=
                 (pending_.kind == Action::PrepareCharging ? kPrepareTimeoutMs
                                                          : kActionTimeoutMs)) {
    issues_ |= ActionTimedOut;
    finishAction(false, now, inputs);
  }

  if (phase_ == Phase::ShutdownWait) return;

  // Preparation always runs first: it includes mandatory reconciliation and PD
  // validation, even for a full bank or invalid initial telemetry. Once it has
  // completed, unsafe measurements can inhibit CE. The normal >20 V endpoint
  // is evaluated only after CE has actually been enabled, preserving startup's
  // requested boost -> charger -> observe sequence.
  if ((phase_ == Phase::ChargePrepare && prepared_) ||
      phase_ == Phase::Charging || chargingEnabledExpected_) {
    if (!inputs.statusValid) {
      leaveCharging(StatusUnavailable);
    } else if (!voltageValid(inputs.vcapV)) {
      leaveCharging(InvalidVoltage);
    } else if (inputs.vcapV >= kMaximumV) {
      leaveCharging(ChargeOvervoltage);
    } else if ((prepared_ || phase_ == Phase::Charging) && inputs.chargeFault) {
      leaveCharging(ChargerFault);
    } else if (chargingEnabledExpected_ && !inputs.boostPowerGood) {
      leaveCharging(ChargerFault);
    } else if (phase_ == Phase::Charging && inputs.vcapV > kChargeTargetV) {
      // Successful qualification leaves boost/CE on. Normal CV regulation has
      // no progress timeout or two-hour ceiling; keep monitoring its health.
      maintenance_ = true;
      phase_ = Phase::JetsonStarting;
    }
  }

  if (phase_ == Phase::Charging) {
    if (static_cast<uint32_t>(now - chargeAt_) >= kMaximumChargeMs) {
      leaveCharging(ChargeTimeLimit);
    } else {
      if (inputs.vcapV >= progressV_ + kSignificantRiseV) {
        progressV_ = inputs.vcapV;
        progressAt_ = now;
      }
      if (static_cast<uint32_t>(now - progressAt_) >= kNoRiseMs)
        leaveCharging(ChargeNoRise);
    }
  }

  if (pending_.kind != Action::None) return;

  if ((phase_ == Phase::JetsonStarting || phase_ == Phase::Running) &&
      stopNeeded_ && !stopAttempted_) {
    stopAttempted_ = true;
    issueAction(Action::StopCharging, now);
    return;
  }

  // Attempt once per boot, after preparation/CE or on the failure path before
  // supplying J9. An arming failure does not prevent main-powered Jetson start.
  if ((phase_ == Phase::Charging || phase_ == Phase::JetsonStarting ||
       phase_ == Phase::Running) &&
      !backupAttempted_ && inputs.statusValid && inputs.mainPresent &&
      voltageValid(inputs.vcapV) && inputs.vcapV >= kBackupArmV &&
      inputs.vcapV <= kMaximumV) {
    backupAttempted_ = true;
    issueAction(Action::ArmBackup, now);
    return;
  }

  switch (phase_) {
    case Phase::ChargePrepare:
      if (!prepared_) {
        issueAction(Action::PrepareCharging, now);
      } else if (inputs.boostPowerGood && !enableChargeAttempted_) {
        enableChargeAttempted_ = true;
        issueAction(Action::EnableCharging, now);
      } else if (static_cast<uint32_t>(now - phaseAt_) >= kBoostPgTimeoutMs) {
        leaveCharging(BoostPowerGoodTimeout);
        stopAttempted_ = true;
        issueAction(Action::StopCharging, now);
      }
      break;
    case Phase::JetsonStarting:
      if (!jetsonAttempted_) {
        jetsonAttempted_ = true;
        issueAction(Action::EnableJetson, now);
      } else if (!pingAttempted_) {
        pingAttempted_ = true;
        issueAction(Action::PingJetson, now);
      }
      break;
    case Phase::Settling:
    case Phase::Charging:
    case Phase::Running:
    case Phase::ShutdownWait:
    case Phase::PowerOff:
      break;
  }
}

uint32_t PowerManager::shutdownElapsedMs(uint32_t now) const {
  return shutdownReason_ == ShutdownReason::None ? 0
      : static_cast<uint32_t>(now - shutdownAt_);
}

uint32_t PowerManager::shutdownRemainingMs(uint32_t now) const {
  if (shutdownReason_ == ShutdownReason::None || phase_ == Phase::PowerOff)
    return 0;
  const uint32_t elapsed = shutdownElapsedMs(now);
  return elapsed >= kShutdownMs ? 0 : kShutdownMs - elapsed;
}

const char *PowerManager::phaseName(Phase phase) {
  switch (phase) {
    case Phase::Settling: return "SETTLING";
    case Phase::ChargePrepare: return "CHARGE_PREPARE";
    case Phase::Charging: return "CHARGING";
    case Phase::JetsonStarting: return "JETSON_STARTING";
    case Phase::Running: return "RUNNING";
    case Phase::ShutdownWait: return "SHUTDOWN_WAIT";
    case Phase::PowerOff: return "POWER_OFF";
  }
  return "UNKNOWN";
}

const char *PowerManager::actionName(Action action) {
  switch (action) {
    case Action::None: return "NONE";
    case Action::PrepareCharging: return "PREPARE_CHARGING";
    case Action::EnableCharging: return "ENABLE_CHARGING";
    case Action::StopCharging: return "STOP_CHARGING";
    case Action::ArmBackup: return "ARM_BACKUP";
    case Action::EnableJetson: return "ENABLE_JETSON";
    case Action::PingJetson: return "PING_JETSON";
    case Action::RequestJetsonShutdown: return "REQUEST_JETSON_SHUTDOWN";
    case Action::CutPower: return "CUT_POWER";
  }
  return "UNKNOWN";
}

const char *PowerManager::issueName(Issue issue) {
  switch (issue) {
    case StatusUnavailable: return "STATUS_UNAVAILABLE";
    case InvalidVoltage: return "INVALID_VOLTAGE";
    case ChargePreparationFailed: return "CHARGE_PREPARATION_FAILED";
    case ChargerEnableFailed: return "CHARGER_ENABLE_FAILED";
    case ChargerFault: return "CHARGER_FAULT";
    case ChargeNoRise: return "CHARGE_NO_SIGNIFICANT_RISE";
    case ChargeStopFailed: return "CHARGE_STOP_FAILED";
    case BackupArmFailed: return "BACKUP_ARM_FAILED";
    case JetsonEnableFailed: return "JETSON_ENABLE_FAILED";
    case JetsonPingFailed: return "JETSON_PING_FAILED";
    case ShutdownRequestFailed: return "SHUTDOWN_REQUEST_FAILED";
    case PowerOffFailed: return "POWER_OFF_FAILED";
    case ActionTimedOut: return "ACTION_TIMED_OUT";
    case BoostPowerGoodTimeout: return "BOOST_POWER_GOOD_TIMEOUT";
    case ChargeOvervoltage: return "CHARGE_OVERVOLTAGE";
    case ChargeTimeLimit: return "CHARGE_TIME_LIMIT";
    case MainUnavailableAtStartup: return "MAIN_UNAVAILABLE_AT_STARTUP";
  }
  return "UNKNOWN";
}

const char *PowerManager::shutdownReasonName(ShutdownReason reason) {
  switch (reason) {
    case ShutdownReason::None: return "NONE";
    case ShutdownReason::MainLoss: return "MAIN_LOSS";
    case ShutdownReason::FullReboot: return "FULL_REBOOT";
  }
  return "UNKNOWN";
}
