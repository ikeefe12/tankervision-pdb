#include "ChargerHold.h"
#include <math.h>
#include <stdio.h>

namespace {
constexpr uint32_t kPollMs = 20, kStartupMs = 2000, kStableMs = 100;
// ADC screening limits from characterization, not calibrated voltage accuracy.
constexpr float kRegMin = 19.8f, kRegMax = 21.4f, kTrip = 21.5f;
constexpr uint8_t kOutputs = 0x0b;  // Boost, CE, fixed TS; low-current ISET.
}

const char *ChargerHold::stateName() const {
  switch (state_) {
    case State::Stopped: return "STOPPED";
    case State::Starting: return "STARTING";
    case State::Holding: return "HOLDING";
    case State::Fault: return "FAULT";
  }
  return "UNKNOWN";
}

bool ChargerHold::trip(const char *reason) {
  snprintf(error_, sizeof(error_), "%s", reason);
  state_ = State::Fault;
  cleanupOk_ = board_.allOff();
  return false;
}

bool ChargerHold::start() {
  if (active()) return true;
  error_[0] = '\0';
  samples_ = maxGapMs_ = 0;
  voltage_ = minV_ = maxV_ = 0;
  qualifying_ = false;
  cleanupOk_ = true;
  if (!board_.allOff() || !board_.setBackup(false) ||
      !board_.setChargeHighCurrent(false) || !board_.setThermistorOverride(true) ||
      !board_.setBoost(true) || !board_.setCharger(true))
    return trip(board_.error());
  state_ = State::Starting;
  startedAt_ = millis();
  lastSampleAt_ = startedAt_;
  // Take the first sample immediately after CE readback, then at 20 ms intervals.
  lastSampleAt_ -= kPollMs;
  return service();
}

bool ChargerHold::service() {
  if (!active()) return state_ != State::Fault;
  const uint32_t now = millis(), gap = now - lastSampleAt_;
  if (gap < kPollMs) return true;
  if (samples_ && gap > maxGapMs_) maxGapMs_ = gap;
  lastSampleAt_ = now;
  Status s;
  if (!board_.readStatus(s)) return trip(board_.error());
  voltage_ = s.vcapV;
  ++samples_;
  if (!isfinite(s.vcapV) || s.vcapV > kTrip) return trip("VCAP overvoltage/ADC fault");
  if (!s.mainSelected || !(s.usbPg || s.dcPg) || !s.ssBuckPg ||
      !s.boostPg || !s.chargerPg || s.internalOutputs != kOutputs ||
      (s.internalOutputInputs & 0x1f) != kOutputs || s.externalOutputs != 0 ||
      (s.externalOutputInputs & 0x1f) != 0)
    return trip("source, PG, or enable-state guard");
  const bool regulated = s.vcapV >= kRegMin && s.vcapV <= kRegMax && s.chargerStat &&
      !s.backupPg && !s.buckPg && !s.extVbusPg && !s.ext5vVbusPg && !s.extSsPg && !s.ext5vSsPg;
  // GPIO1 observes the real sensor branch even when U14 selects the fixed TS
  // divider. Its open-sensor voltage is not a charger temperature fault here.
  if (state_ == State::Starting) {
    if (regulated) {
      if (!qualifying_) { stableAt_ = now; qualifying_ = true; }
      if (now - stableAt_ >= kStableMs) {
        state_ = State::Holding;
        minV_ = maxV_ = s.vcapV;
      }
    } else qualifying_ = false;
    if (state_ == State::Starting && now - startedAt_ >= kStartupMs)
      return trip("VCAP regulation/STAT qualification timeout");
  } else {
    if (!regulated) return trip("VCAP regulation, STAT, or inactive-output PG guard");
    if (s.vcapV < minV_) minV_ = s.vcapV;
    if (s.vcapV > maxV_) maxV_ = s.vcapV;
  }
  return true;
}

bool ChargerHold::stop() {
  state_ = State::Stopped;
  cleanupOk_ = board_.allOff();
  if (!cleanupOk_) {
    snprintf(error_, sizeof(error_), "%s", board_.error());
    state_ = State::Fault;
  } else error_[0] = '\0';
  return cleanupOk_;
}
