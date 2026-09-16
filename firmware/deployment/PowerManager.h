#pragma once

#include <stdint.h>

// Single-task, allocation-free deployment policy. No Arduino, I/O, waits, or
// reset calls. Feed a fresh observation to tick(); execute pendingAction() in a
// nonblocking adapter, then report its result by id. Replaced ids are cancelled:
// an adapter must never finish an old enable operation after a newer shutdown.
class PowerManager {
 public:
  enum class Phase : uint8_t {
    Settling, ChargePrepare, Charging, JetsonStarting, Running, ShutdownWait,
    PowerOff
  };
  enum class Action : uint8_t {
    None,
    // Always the first action after settling (unless shutdown was requested).
    // Reconcile controls, validate PD budget, select low current and real TS,
    // enable boost. Adapter validates TS (1300..2350 mV) after boost is stable.
    PrepareCharging,
    // CE remains enabled after successful >20 V initial qualification to
    // maintain the bank. Only a charger issue or shutdown requests it off.
    EnableCharging,
    StopCharging,  // CE low, >=100 ms decay, boost low; preserve backup.
    ArmBackup,    // Only requested with valid main and raw VCAP in 10..21 V.
    EnableJetson, // J9 / Port::Supervised.
    PingJetson,   // Completion means sent; Inputs::jetsonPong is reply evidence.
    // Send shutdown request promptly and initiate nonblocking charger cleanup.
    // This completion and all replies leave the original 60-second timer intact.
    RequestJetsonShutdown,
    // Disable all ports, restore expander defaults, release GPIO42. If the MCU
    // survives for 250 ms, the adapter calls esp_restart(). Policy is terminal.
    CutPower
  };
  enum class ShutdownReason : uint8_t { None, MainLoss, FullReboot };
  enum Issue : uint32_t {
    StatusUnavailable = 1u << 0,
    InvalidVoltage = 1u << 1,
    ChargePreparationFailed = 1u << 2,
    ChargerEnableFailed = 1u << 3,
    ChargerFault = 1u << 4,
    ChargeNoRise = 1u << 5,
    ChargeStopFailed = 1u << 6,
    BackupArmFailed = 1u << 7,
    JetsonEnableFailed = 1u << 8,
    JetsonPingFailed = 1u << 9,
    ShutdownRequestFailed = 1u << 10,
    PowerOffFailed = 1u << 11,
    ActionTimedOut = 1u << 12,
    BoostPowerGoodTimeout = 1u << 13,
    ChargeOvervoltage = 1u << 14,
    ChargeTimeLimit = 1u << 15,
    MainUnavailableAtStartup = 1u << 16
  };
  struct Inputs {
    bool statusValid = false;
    bool mainPresent = false;  // Validated input PG AND main mux selection.
    float vcapV = 0.0f;        // Raw voltage, no current ADC dependency.
    bool boostPowerGood = false;
    bool chargeFault = false; // Adapter's deglitched real-TS / charger checks.
    // Per-tick receive events. ACK/ready latch only during ShutdownWait.
    bool shutdownAck = false;
    bool finalReady = false;
    bool jetsonPong = false;
  };
  struct PendingAction {
    Action kind = Action::None;
    uint32_t id = 0;
  };

  static constexpr uint32_t kSettlingMs = 10000;
  static constexpr uint32_t kNoRiseMs = 10000;
  static constexpr float kSignificantRiseV = 0.10f;
  static constexpr float kChargeTargetV = 20.0f; // Qualify strictly above target.
  static constexpr float kMaximumV = 21.0f;
  static constexpr float kBackupArmV = 10.0f;
  static constexpr uint32_t kMaximumChargeMs = 2u * 60u * 60u * 1000u;
  static constexpr uint32_t kShutdownMs = 60000;
  static constexpr uint32_t kActionTimeoutMs = 5000;
  static constexpr uint32_t kPrepareTimeoutMs = 15000;
  static constexpr uint32_t kBoostPgTimeoutMs = 2000;

  void begin(uint32_t now);
  void tick(uint32_t now, const Inputs &inputs);
  PendingAction pendingAction() const { return pending_; }
  // Returns false for a stale id, no pending action, or a duplicate completion.
  // Results take effect on the next tick, never from a callback context.
  bool completeAction(uint32_t id, bool success);
  // Starts an irrevocable deadline immediately, including during settling.
  // Repeated requests return false and never restart or shorten the deadline.
  bool requestFullReboot(uint32_t now);

  Phase phase() const { return phase_; }
  uint32_t issues() const { return issues_; }
  bool backupArmed() const { return backupArmed_; }
  // Intended CE state from successful enable until stop/shutdown is requested.
  // The adapter should monitor real TS, charger PG and CE readback throughout
  // initial charging AND maintenance. Disable failures remain issue evidence.
  bool chargingEnabledExpected() const { return chargingEnabledExpected_; }
  bool maintenance() const { return maintenance_; }
  bool jetsonEnabled() const { return jetsonEnabled_; }
  bool jetsonPongSeen() const { return jetsonPongSeen_; }
  bool shutdownAckSeen() const { return shutdownAckSeen_; }
  bool finalReadySeen() const { return finalReadySeen_; }
  bool shutdownRequestSent() const { return shutdownRequestSent_; }
  ShutdownReason shutdownReason() const { return shutdownReason_; }
  uint32_t shutdownStartedMs() const { return shutdownAt_; }
  uint32_t shutdownElapsedMs(uint32_t now) const;
  uint32_t shutdownRemainingMs(uint32_t now) const;
  static const char *phaseName(Phase phase);
  static const char *actionName(Action action);
  static const char *issueName(Issue issue);
  static const char *shutdownReasonName(ShutdownReason reason);

 private:
  Phase phase_ = Phase::Settling;
  PendingAction pending_;
  uint32_t nextActionId_ = 0, pendingAt_ = 0, issues_ = 0;
  uint32_t phaseAt_ = 0, chargeAt_ = 0, progressAt_ = 0, shutdownAt_ = 0;
  float progressV_ = 0.0f;
  ShutdownReason shutdownReason_ = ShutdownReason::None;
  bool begun_ = false, completed_ = false, completionSuccess_ = false;
  bool prepared_ = false, enableChargeAttempted_ = false;
  bool chargingEnabledExpected_ = false, maintenance_ = false;
  bool stopNeeded_ = false, stopAttempted_ = false;
  bool backupAttempted_ = false, backupArmed_ = false;
  bool jetsonAttempted_ = false, jetsonEnabled_ = false, pingAttempted_ = false;
  bool jetsonPongSeen_ = false, shutdownAckSeen_ = false, finalReadySeen_ = false;
  bool shutdownRequestSent_ = false;
  void issueAction(Action action, uint32_t now);
  void cancelAction();
  void finishAction(bool success, uint32_t now, const Inputs &inputs);
  void leaveCharging(uint32_t issue = 0);
  void startShutdown(uint32_t now, ShutdownReason reason);
  static bool voltageValid(float voltage);
};
