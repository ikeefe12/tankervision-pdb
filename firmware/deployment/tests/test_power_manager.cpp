#include "PowerManager.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

using Phase = PowerManager::Phase;
using Action = PowerManager::Action;
using Reason = PowerManager::ShutdownReason;

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); \
  std::exit(1); } } while (0)

struct Fixture {
  PowerManager pm;
  PowerManager::Inputs in;
  uint32_t now;
  explicit Fixture(uint32_t start = 0) : now(start) {
    in.statusValid = true;
    in.mainPresent = true;
    in.boostPowerGood = true;
    pm.begin(now);
  }
  Action action() const { return pm.pendingAction().kind; }
  void at(uint32_t time) { now = time; pm.tick(now, in); }
  void advance(uint32_t delta) { at(now + delta); }
  void done(Action expected, bool success = true) {
    CHECK(action() == expected);
    CHECK(pm.completeAction(pm.pendingAction().id, success));
    advance(1);
  }
  void charge() {
    advance(PowerManager::kSettlingMs);
    done(Action::PrepareCharging);
    done(Action::EnableCharging);
    CHECK(pm.phase() == Phase::Charging);
    CHECK(action() == Action::None);
  }
  void run() {
    charge();
    in.vcapV = 20.1f;
    advance(1);
    done(Action::ArmBackup);
    done(Action::EnableJetson);
    done(Action::PingJetson);
    CHECK(pm.phase() == Phase::Running);
  }
};

void settling_has_no_actions_and_uses_exact_boundary() {
  Fixture f;
  f.in.vcapV = 12.0f;
  f.in.mainPresent = false;
  f.at(9999);
  CHECK(f.pm.phase() == Phase::Settling);
  CHECK(f.action() == Action::None);
  f.in.mainPresent = true;
  f.at(10000);
  CHECK(f.action() == Action::PrepareCharging);
}

void normal_charge_qualifies_above_target_and_maintains_during_jetson_start() {
  Fixture f;
  f.charge();
  f.in.vcapV = 20.0f;
  f.advance(1);
  CHECK(f.pm.phase() == Phase::Charging);
  f.done(Action::ArmBackup);
  f.in.vcapV = 20.001f;
  f.advance(1);
  CHECK(f.action() == Action::EnableJetson);
  CHECK(f.pm.chargingEnabledExpected());
  CHECK(f.pm.maintenance());
  f.done(Action::EnableJetson);
  f.done(Action::PingJetson);
  CHECK(f.pm.phase() == Phase::Running);
  CHECK(f.pm.backupArmed());
  CHECK(f.pm.jetsonEnabled());
  CHECK(!f.pm.jetsonPongSeen());
  CHECK(f.pm.issues() == 0);
  f.in.jetsonPong = true;
  f.advance(1);
  CHECK(f.pm.jetsonPongSeen());
}

void full_bank_still_prepares_and_enables_then_maintains() {
  Fixture f;
  f.in.vcapV = 20.5f;
  f.advance(10000);
  CHECK(f.action() == Action::PrepareCharging);
  f.done(Action::PrepareCharging);
  CHECK(f.action() == Action::EnableCharging);
  f.done(Action::EnableCharging);
  CHECK(f.action() == Action::ArmBackup);
  CHECK(f.pm.chargingEnabledExpected());
  CHECK(f.pm.maintenance());
  f.done(Action::ArmBackup);
  f.done(Action::EnableJetson);
  f.done(Action::PingJetson);
  CHECK(f.pm.phase() == Phase::Running);
  CHECK(f.pm.issues() == 0);
}

void invalid_initial_telemetry_still_prepares_before_inhibiting_ce() {
  for (int mode = 0; mode < 3; ++mode) {
    Fixture f;
    if (mode == 0) f.in.vcapV = std::numeric_limits<float>::quiet_NaN();
    if (mode == 1) f.in.statusValid = false;
    if (mode == 2) f.in.vcapV = 21.1f;
    f.advance(10000);
    CHECK(f.action() == Action::PrepareCharging);
    f.advance(1000);
    CHECK(f.action() == Action::PrepareCharging);
    f.done(Action::PrepareCharging);
    CHECK(f.action() == Action::StopCharging);
    f.done(Action::StopCharging);
    CHECK(f.action() == Action::EnableJetson);
  }
}

void no_rise_stops_at_ten_seconds_but_continues_jetson() {
  Fixture f;
  f.charge();
  const uint32_t chargeAt = f.now;
  f.at(chargeAt + 9999);
  CHECK(f.pm.phase() == Phase::Charging);
  f.at(chargeAt + 10000);
  CHECK(f.action() == Action::StopCharging);
  CHECK(f.pm.issues() & PowerManager::ChargeNoRise);
  f.done(Action::StopCharging);
  CHECK(f.action() == Action::EnableJetson); // Empty cap cannot be armed.
  f.done(Action::EnableJetson);
  f.done(Action::PingJetson);
  CHECK(f.pm.phase() == Phase::Running);
  CHECK(!f.pm.backupArmed());
}

void significant_rise_restarts_timer_but_noise_and_recovery_do_not() {
  Fixture f;
  f.charge();
  f.in.vcapV = 0.09f;
  f.advance(9999);
  CHECK(f.pm.phase() == Phase::Charging);
  f.in.vcapV = 0.11f;
  f.advance(1); // A fresh significant rise at the boundary satisfies progress.
  const uint32_t progressAt = f.now;
  f.in.vcapV = 0.0f;
  f.advance(5000);
  f.in.vcapV = 0.20f; // Only 0.09 V above the last significant high.
  f.at(progressAt + 9999);
  CHECK(f.pm.phase() == Phase::Charging);
  f.at(progressAt + 10000);
  CHECK(f.pm.issues() & PowerManager::ChargeNoRise);
}

void backup_failure_continues_charge_and_main_powered_jetson() {
  Fixture f;
  f.in.vcapV = 12.0f;
  f.advance(10000);
  f.done(Action::PrepareCharging);
  f.done(Action::EnableCharging);
  f.done(Action::ArmBackup, false);
  CHECK(f.pm.phase() == Phase::Charging);
  CHECK(!f.pm.backupArmed());
  f.in.vcapV = 20.1f;
  f.advance(1);
  CHECK(f.action() == Action::EnableJetson);
  CHECK(f.pm.maintenance());
  CHECK(f.pm.issues() & PowerManager::BackupArmFailed);
}

void prepare_enable_and_stop_failures_are_reported_and_continue() {
  for (bool failPrepare : {false, true}) {
    Fixture f;
    f.advance(10000);
    f.done(Action::PrepareCharging, !failPrepare);
    if (!failPrepare) f.done(Action::EnableCharging, false);
    f.done(Action::StopCharging, false);
    CHECK(f.action() == Action::EnableJetson);
    f.done(Action::EnableJetson, false);
    f.done(Action::PingJetson, false);
    CHECK(f.pm.phase() == Phase::Running);
    CHECK(f.pm.issues() & (failPrepare ? PowerManager::ChargePreparationFailed
                                     : PowerManager::ChargerEnableFailed));
    CHECK(f.pm.issues() & PowerManager::ChargeStopFailed);
    CHECK(f.pm.issues() & PowerManager::JetsonEnableFailed);
    CHECK(f.pm.issues() & PowerManager::JetsonPingFailed);
  }
}

void prepare_gets_fifteen_seconds_other_actions_five() {
  Fixture f;
  f.advance(10000);
  f.advance(14999);
  CHECK(f.action() == Action::PrepareCharging);
  f.advance(1);
  CHECK(f.action() == Action::StopCharging);
  CHECK(f.pm.issues() & PowerManager::ActionTimedOut);
  CHECK(f.pm.issues() & PowerManager::ChargePreparationFailed);
  f.advance(4999);
  CHECK(f.action() == Action::StopCharging);
  f.advance(1);
  CHECK(f.action() == Action::EnableJetson);
  CHECK(f.pm.issues() & PowerManager::ChargeStopFailed);
}

void missing_boost_pg_and_real_ts_fault_never_enable_charger() {
  Fixture f;
  f.in.boostPowerGood = false;
  f.advance(10000);
  f.done(Action::PrepareCharging);
  f.advance(1999);
  CHECK(f.action() == Action::None);
  f.advance(1);
  CHECK(f.action() == Action::StopCharging);
  CHECK(f.pm.issues() & PowerManager::BoostPowerGoodTimeout);

  Fixture g;
  g.in.chargeFault = true;
  g.advance(10000);
  CHECK(g.action() == Action::PrepareCharging); // TS invalid before boost is normal.
  g.done(Action::PrepareCharging);
  CHECK(g.action() == Action::StopCharging);
  CHECK(g.pm.issues() & PowerManager::ChargerFault);
}

void charger_fault_or_lost_boost_stops_and_continues() {
  for (bool useTs : {false, true}) {
    Fixture f;
    f.charge();
    f.in.chargeFault = useTs;
    f.in.boostPowerGood = useTs;
    f.advance(1);
    CHECK(f.action() == Action::StopCharging);
    CHECK(f.pm.issues() & PowerManager::ChargerFault);
    f.done(Action::StopCharging);
    CHECK(f.action() == Action::EnableJetson);
  }
}

void invalid_voltage_status_and_overvoltage_stop_charging() {
  for (int mode = 0; mode < 4; ++mode) {
    Fixture f;
    f.charge();
    if (mode == 0) f.in.vcapV = std::numeric_limits<float>::quiet_NaN();
    if (mode == 1) f.in.vcapV = -0.1f;
    if (mode == 2) f.in.statusValid = false;
    if (mode == 3) f.in.vcapV = 21.0f;
    f.advance(1);
    CHECK(f.action() == Action::StopCharging);
    const uint32_t flag = mode < 2 ? PowerManager::InvalidVoltage
        : mode == 2 ? PowerManager::StatusUnavailable : PowerManager::ChargeOvervoltage;
    CHECK(f.pm.issues() & flag);
    CHECK(f.pm.phase() == Phase::JetsonStarting);
  }
}

void two_hour_ceiling_is_checked_before_new_progress() {
  Fixture f;
  f.charge();
  f.in.vcapV = 1.0f;
  f.advance(PowerManager::kMaximumChargeMs);
  CHECK(f.action() == Action::StopCharging);
  CHECK(f.pm.issues() & PowerManager::ChargeTimeLimit);
}

void healthy_maintenance_has_no_flat_voltage_or_two_hour_timeout() {
  Fixture f;
  f.run();
  CHECK(f.pm.chargingEnabledExpected());
  CHECK(f.pm.maintenance());
  f.advance(10000);
  CHECK(f.action() == Action::None);
  f.advance(3u * 60u * 60u * 1000u);
  CHECK(f.pm.phase() == Phase::Running);
  CHECK(f.action() == Action::None);
  CHECK(f.pm.chargingEnabledExpected());
  CHECK(f.pm.maintenance());
  CHECK(f.pm.issues() == 0);
}

void maintenance_fault_stops_charger_preserving_running_jetson_and_backup() {
  for (unsigned fault = 0; fault < 4; ++fault) {
    Fixture f;
    f.run();
    if (fault == 0) f.in.chargeFault = true;
    if (fault == 1) f.in.boostPowerGood = false;
    if (fault == 2) f.in.statusValid = false;
    if (fault == 3) f.in.vcapV = 21.0f;
    f.advance(1);
    CHECK(f.pm.phase() == Phase::Running);
    CHECK(f.action() == Action::StopCharging);
    CHECK(!f.pm.chargingEnabledExpected());
    CHECK(!f.pm.maintenance());
    CHECK(f.pm.jetsonEnabled());
    CHECK(f.pm.backupArmed());
    CHECK(f.pm.shutdownReason() == Reason::None);
    f.done(Action::StopCharging);
    CHECK(f.pm.phase() == Phase::Running);
    CHECK(f.action() == Action::None);
    f.advance(100000);
    CHECK(f.action() == Action::None);
    CHECK(f.pm.jetsonEnabled() && f.pm.backupArmed());
    CHECK(f.pm.issues() != 0);
  }
}

void maintenance_fault_during_j9_start_retries_interrupted_action_after_cleanup() {
  Fixture f;
  f.charge();
  f.in.vcapV = 20.2f;
  f.advance(1);
  f.done(Action::ArmBackup);
  CHECK(f.action() == Action::EnableJetson);
  const uint32_t old = f.pm.pendingAction().id;
  f.in.chargeFault = true;
  f.advance(1);
  CHECK(f.action() == Action::StopCharging);
  CHECK(f.pm.phase() == Phase::JetsonStarting);
  CHECK(!f.pm.completeAction(old, true));
  CHECK(f.pm.backupArmed());
  f.done(Action::StopCharging);
  CHECK(f.action() == Action::EnableJetson);
  f.done(Action::EnableJetson);
  f.done(Action::PingJetson);
  CHECK(f.pm.phase() == Phase::Running);
  CHECK(!f.pm.chargingEnabledExpected());
}

void fault_at_target_is_not_misclassified_as_healthy_qualification() {
  Fixture f;
  f.charge();
  f.in.vcapV = 20.5f;
  f.in.chargeFault = true;
  f.advance(1);
  CHECK(f.action() == Action::StopCharging);
  CHECK(f.pm.issues() & PowerManager::ChargerFault);
  CHECK(!f.pm.maintenance());
  CHECK(!f.pm.chargingEnabledExpected());
}

void shutdown_from_maintenance_always_requests_adapter_charger_cleanup() {
  for (bool reboot : {false, true}) {
    Fixture f;
    f.run();
    CHECK(f.pm.chargingEnabledExpected());
    if (reboot) CHECK(f.pm.requestFullReboot(f.now));
    else { f.in.mainPresent = false; f.advance(1); }
    CHECK(f.action() == Action::RequestJetsonShutdown);
    CHECK(!f.pm.chargingEnabledExpected());
    CHECK(!f.pm.maintenance());
    CHECK(f.pm.jetsonEnabled() && f.pm.backupArmed());
  }
}

void main_missing_after_settle_starts_shutdown_without_charging() {
  Fixture f;
  f.in.mainPresent = false;
  f.advance(9999);
  CHECK(f.action() == Action::None);
  f.advance(1);
  CHECK(f.action() == Action::RequestJetsonShutdown);
  CHECK(f.pm.shutdownReason() == Reason::MainLoss);
  CHECK(f.pm.issues() & PowerManager::MainUnavailableAtStartup);
  CHECK(f.pm.shutdownStartedMs() == 10000);
}

void main_loss_preempts_pending_enable_and_rejects_late_completion() {
  Fixture f;
  f.advance(10000);
  f.done(Action::PrepareCharging);
  const uint32_t old = f.pm.pendingAction().id;
  f.in.mainPresent = false;
  f.advance(1);
  CHECK(f.action() == Action::RequestJetsonShutdown);
  CHECK(!f.pm.completeAction(old, true));
  CHECK(f.pm.phase() == Phase::ShutdownWait);
  CHECK(f.pm.shutdownRemainingMs(f.now) == 60000);
}

void ack_ready_and_power_return_never_change_deadline() {
  for (unsigned variant = 0; variant < 8; ++variant) {
    Fixture f;
    f.run();
    f.in.mainPresent = false;
    f.advance(1);
    const uint32_t requested = f.now;
    f.done(Action::RequestJetsonShutdown);
    f.in.shutdownAck = variant & 1u;
    f.in.finalReady = variant & 2u;
    f.in.mainPresent = variant & 4u;
    f.at(requested + 1);
    CHECK(f.pm.shutdownAckSeen() == bool(variant & 1u));
    CHECK(f.pm.finalReadySeen() == bool(variant & 2u));
    f.in.shutdownAck = f.in.finalReady = false;
    f.at(requested + 59999);
    CHECK(f.pm.phase() == Phase::ShutdownWait);
    CHECK(f.action() == Action::None);
    CHECK(f.pm.shutdownRemainingMs(f.now) == 1);
    CHECK(!f.pm.requestFullReboot(f.now));
    f.at(requested + 60000);
    CHECK(f.pm.phase() == Phase::PowerOff);
    CHECK(f.action() == Action::CutPower);
    CHECK(f.pm.shutdownElapsedMs(f.now) == 60000);
    CHECK(f.pm.shutdownStartedMs() == requested);
  }
}

void low_voltage_and_status_failure_do_not_shorten_or_extend_sixty_seconds() {
  Fixture f;
  f.run();
  f.in.mainPresent = false;
  f.advance(1);
  const uint32_t requested = f.now;
  f.done(Action::RequestJetsonShutdown, false);
  f.in.vcapV = 4.0f;
  f.advance(1);
  CHECK(f.pm.phase() == Phase::ShutdownWait);
  f.in.statusValid = false;
  f.at(requested + 59999);
  CHECK(f.pm.phase() == Phase::ShutdownWait);
  f.at(requested + 60000);
  CHECK(f.action() == Action::CutPower);
  CHECK(f.pm.issues() & PowerManager::ShutdownRequestFailed);
}

void hung_shutdown_action_cannot_prevent_cutoff() {
  Fixture f;
  CHECK(f.pm.requestFullReboot(1));
  const uint32_t old = f.pm.pendingAction().id;
  f.at(60001); // No intermediary action completion or timeout processing.
  CHECK(f.action() == Action::CutPower);
  CHECK(!f.pm.completeAction(old, true));
}

void explicit_reboot_during_settling_is_immediate_and_idempotent() {
  Fixture f;
  CHECK(f.pm.requestFullReboot(0));
  CHECK(f.action() == Action::RequestJetsonShutdown);
  CHECK(f.pm.shutdownStartedMs() == 0); // Timestamp zero is not a sentinel.
  CHECK(f.pm.shutdownReason() == Reason::FullReboot);
  CHECK(!f.pm.requestFullReboot(500));
  f.done(Action::RequestJetsonShutdown);
  f.at(59999);
  CHECK(f.pm.phase() == Phase::ShutdownWait);
  f.at(60000);
  CHECK(f.action() == Action::CutPower);
}

void cutoff_completion_is_terminal_even_if_main_returns_or_cut_fails() {
  for (bool success : {false, true}) {
    Fixture f;
    CHECK(f.pm.requestFullReboot(0));
    f.at(60000);
    f.done(Action::CutPower, success);
    CHECK(f.pm.phase() == Phase::PowerOff);
    CHECK(f.action() == Action::None);
    f.in.mainPresent = true;
    f.advance(100000);
    CHECK(f.pm.phase() == Phase::PowerOff);
    CHECK(f.action() == Action::None);
    CHECK(!f.pm.requestFullReboot(f.now));
    CHECK(bool(f.pm.issues() & PowerManager::PowerOffFailed) == !success);
  }
}

void settling_charging_and_shutdown_timers_survive_millis_wrap() {
  Fixture f(UINT32_MAX - 5000);
  f.charge();
  f.advance(9999);
  CHECK(f.pm.phase() == Phase::Charging);
  f.advance(1);
  CHECK(f.pm.issues() & PowerManager::ChargeNoRise);

  Fixture g(UINT32_MAX - 30000);
  CHECK(g.pm.requestFullReboot(g.now));
  const uint32_t start = g.now;
  g.done(Action::RequestJetsonShutdown);
  g.at(start + 59999u);
  CHECK(g.pm.shutdownRemainingMs(g.now) == 1);
  g.at(start + 60000u);
  CHECK(g.action() == Action::CutPower);
}

void stale_and_duplicate_results_cannot_control_new_actions() {
  Fixture f;
  CHECK(!f.pm.completeAction(0, true));
  f.advance(10000);
  const uint32_t old = f.pm.pendingAction().id;
  CHECK(f.pm.completeAction(old, true));
  CHECK(!f.pm.completeAction(old, false));
  f.advance(1);
  CHECK(f.action() == Action::EnableCharging);
  CHECK(!f.pm.completeAction(old, true));
  f.pm.begin(f.now);
  f.advance(10000);
  CHECK(f.pm.pendingAction().id != old);
  CHECK(!f.pm.completeAction(old, true));
}

void unknown_status_does_not_invent_main_loss() {
  Fixture f;
  f.run();
  f.in.statusValid = false;
  f.in.mainPresent = false;
  f.advance(1);
  CHECK(f.pm.phase() == Phase::Running);
  CHECK(f.pm.issues() & PowerManager::StatusUnavailable);
  f.in.statusValid = true;
  f.advance(1);
  CHECK(f.pm.phase() == Phase::ShutdownWait);
}

int main() {
  void (*tests[])() = {
    settling_has_no_actions_and_uses_exact_boundary,
    normal_charge_qualifies_above_target_and_maintains_during_jetson_start,
    full_bank_still_prepares_and_enables_then_maintains,
    invalid_initial_telemetry_still_prepares_before_inhibiting_ce,
    no_rise_stops_at_ten_seconds_but_continues_jetson,
    significant_rise_restarts_timer_but_noise_and_recovery_do_not,
    backup_failure_continues_charge_and_main_powered_jetson,
    prepare_enable_and_stop_failures_are_reported_and_continue,
    prepare_gets_fifteen_seconds_other_actions_five,
    missing_boost_pg_and_real_ts_fault_never_enable_charger,
    charger_fault_or_lost_boost_stops_and_continues,
    invalid_voltage_status_and_overvoltage_stop_charging,
    two_hour_ceiling_is_checked_before_new_progress,
    healthy_maintenance_has_no_flat_voltage_or_two_hour_timeout,
    maintenance_fault_stops_charger_preserving_running_jetson_and_backup,
    maintenance_fault_during_j9_start_retries_interrupted_action_after_cleanup,
    fault_at_target_is_not_misclassified_as_healthy_qualification,
    shutdown_from_maintenance_always_requests_adapter_charger_cleanup,
    main_missing_after_settle_starts_shutdown_without_charging,
    main_loss_preempts_pending_enable_and_rejects_late_completion,
    ack_ready_and_power_return_never_change_deadline,
    low_voltage_and_status_failure_do_not_shorten_or_extend_sixty_seconds,
    hung_shutdown_action_cannot_prevent_cutoff,
    explicit_reboot_during_settling_is_immediate_and_idempotent,
    cutoff_completion_is_terminal_even_if_main_returns_or_cut_fails,
    settling_charging_and_shutdown_timers_survive_millis_wrap,
    stale_and_duplicate_results_cannot_control_new_actions,
    unknown_status_does_not_invent_main_loss
  };
  for (auto test : tests) test();
  std::printf("PowerManager: %zu scenarios passed\n", sizeof(tests) / sizeof(tests[0]));
}
