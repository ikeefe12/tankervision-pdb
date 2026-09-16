#include "ChargerCharacterization.h"
#include <math.h>

namespace {
constexpr float kTarget = 20.629f;
// Broad ADC bench acceptance, not the tighter charger accuracy specification.
constexpr float kRegMin = 19.8f, kRegMax = 21.4f, kTrip = 21.5f;
constexpr uint32_t kStatusPeriodUs = 20000, kBinUs = 10000;
constexpr size_t kCapacity = 1100;  // 10 ms bins, max 10-second phase.
struct Sample {
  uint32_t us;
  uint16_t minMv, maxMv, meanMv;
  uint8_t input, output, outputPins;
};
Sample samples[kCapacity];
enum class Expectation { Observe, Inhibited, Regulated };
struct Summary {
  bool ok = true;
  uint32_t count = 0, tailCount = 0, maxGapUs = 0;
  uint32_t statusCount = 0, statTransitions = 0, statTailLow = 0;
  float minV = 100, maxV = 0, tailMin = 100, tailMax = 0;
  double tailSum = 0;
  int32_t firstInWindowUs = -1;
};

bool action(bool ok, BoardControl &board, Stream &log, const char *name) {
  log.printf("%s CHARGER_ACTION %s%s%s\n", ok ? "PASS" : "FAIL", name,
             ok ? "" : ": ", ok ? "" : board.error());
  return ok;
}

// Sampling is performed without serial writes. Once CE is off, emit min/max/mean
// bins. Each ADC value is already a 16-sample average from the reusable driver.
bool phase(BoardControl &board, Stream &log, const char *name, uint32_t durationMs,
           bool ce, bool override, bool highCurrent, Expectation expectation) {
  log.printf("PHASE_BEGIN %s duration_ms=%lu CE=%u TS_OVERRIDE=%u ISET_HIGH=%u\n",
             name, static_cast<unsigned long>(durationMs), ce, override, highCurrent);
  Status status;
  if (!board.readStatus(status)) return action(false, board, log, "phase initial status");
  const uint8_t wanted = 1u | (ce ? 2u : 0u) | (highCurrent ? 4u : 0u) | (override ? 8u : 0u);
  // Enable functions perform their own finite I2C/readback operations; startup
  // faster than their return and the ~1.1 ms ADC RC cannot be reconstructed here.
  const uint32_t requestUs = micros();
  if (!board.setCharger(ce)) {
    char detail[160]; snprintf(detail, sizeof(detail), "%s", board.error());
    const bool cleanup = board.allOff();
    log.printf("FAIL CHARGER_ACTION set phase CE: %s cleanup=%u\n", detail, cleanup);
    return false;
  }
  const uint32_t start = micros(), durationUs = durationMs * 1000;
  uint32_t previousUs = start, lastStatusUs = start - kStatusPeriodUs;
  uint32_t binStart = 0, binSum = 0, binCount = 0;
  uint16_t binMin = UINT16_MAX, binMax = 0;
  size_t used = 0;
  Summary summary;
  bool lastStat = status.chargerStat;
  const char *fault = nullptr;
  char faultDetail[160] = {};
  while (micros() - start < durationUs) {
    const uint32_t adcMv = board.readAdcMv(2);
    const uint32_t now = micros(), elapsed = now - start;
    const float voltage = adcMv * 0.0092f;
    const uint32_t gap = now - previousUs;
    if (gap > summary.maxGapUs) summary.maxGapUs = gap;
    previousUs = now;
    ++summary.count;
    if (voltage < summary.minV) summary.minV = voltage;
    if (voltage > summary.maxV) summary.maxV = voltage;
    if (summary.firstInWindowUs < 0 && voltage >= kRegMin && voltage <= kRegMax)
      summary.firstInWindowUs = elapsed;
    if (elapsed >= durationUs - min(durationUs, uint32_t(2000000))) {
      ++summary.tailCount; summary.tailSum += voltage;
      if (voltage < summary.tailMin) summary.tailMin = voltage;
      if (voltage > summary.tailMax) summary.tailMax = voltage;
    }
    binSum += adcMv; ++binCount;
    if (adcMv < binMin) binMin = adcMv;
    if (adcMv > binMax) binMax = adcMv;
    if (elapsed - binStart >= kBinUs) {
      if (used >= kCapacity) { fault = "capture buffer full"; break; }
      samples[used++] = {elapsed, binMin, binMax, uint16_t((binSum + binCount / 2) / binCount),
                        status.internalInputs, status.internalOutputs, status.internalOutputInputs};
      binStart = elapsed; binSum = binCount = 0; binMin = UINT16_MAX; binMax = 0;
    }
    if (adcMv == UINT32_MAX || !isfinite(voltage) || voltage > kTrip) {
      fault = "VCAP overvoltage/ADC fault"; break;
    }
    if (now - lastStatusUs >= kStatusPeriodUs) {
      lastStatusUs = now;
      if (!board.readStatus(status)) {
        fault = "I2C/status read failed";
        snprintf(faultDetail, sizeof(faultDetail), "%s", board.error()); break;
      }
      ++summary.statusCount;
      if (status.chargerStat != lastStat) ++summary.statTransitions;
      lastStat = status.chargerStat;
      if (elapsed >= durationUs - min(durationUs, uint32_t(2000000)) && !status.chargerStat)
        ++summary.statTailLow;
      if (!status.mainSelected || !(status.usbPg != status.dcPg) || !status.ssBuckPg ||
          !status.boostPg || !status.chargerPg || status.externalOutputs != 0 ||
          status.internalOutputs != wanted || (status.internalOutputInputs & 0x1f) != wanted) {
        fault = "source, PG, or control-state guard"; break;
      }
      if (status.vcapV > kTrip) { fault = "VCAP status overvoltage"; break; }
      // GPIO1 remains on the real sensor branch, even when TS selects the fixed divider.
      if (status.tsMv < 2700) { fault = "open-sensor bench precondition changed"; break; }
    }
    delay(1);
  }
  // Stop CE before any serial output or statistics evaluation, on every exit.
  const uint32_t stopRequestUs = micros();
  const bool ceOff = board.setCharger(false);
  const uint32_t stoppedUs = micros();
  if (!ceOff) {
    if (!fault) { fault = "CE shutdown readback failed"; snprintf(faultDetail, sizeof(faultDetail), "%s", board.error()); }
    if (!board.allOff()) {
      log.println("FAIL CHARGER_SHUTDOWN state unknown; skipping CSV and retrying outer cleanup");
      return false;
    }
  }
  if (binCount && used < kCapacity)
    samples[used++] = {stopRequestUs - start, binMin, binMax, uint16_t((binSum + binCount / 2) / binCount),
                      status.internalInputs, status.internalOutputs, status.internalOutputInputs};
  const size_t activeUsed = used;
  float shutdownFirst = -1, shutdownLast = -1;
  int32_t belowOneUs = -1;
  // Capture the fast no-bank discharge before sending any serial data. The first
  // observation is after the verified CE-low call, whose duration is reported.
  if (ce && ceOff && !fault) {
    const uint32_t shutdownStart = micros();
    uint32_t offBinStart = 0, offStatusUs = shutdownStart - kStatusPeriodUs;
    binSum = binCount = 0; binMin = UINT16_MAX; binMax = 0;
    while (micros() - shutdownStart < 1500000) {
      const uint32_t mv = board.readAdcMv(2), now = micros(), elapsed = now - shutdownStart;
      const float v = mv * 0.0092f;
      if (shutdownFirst < 0) shutdownFirst = v;
      shutdownLast = v;
      if (belowOneUs < 0 && v < 1.0f) belowOneUs = elapsed;
      binSum += mv; ++binCount;
      if (mv < binMin) binMin = mv;
      if (mv > binMax) binMax = mv;
      if (now - offStatusUs >= kStatusPeriodUs) {
        offStatusUs = now;
        if (!board.readStatus(status) || status.internalOutputs != (wanted & ~2u) ||
            status.externalOutputs != 0 || !status.mainSelected || !status.ssBuckPg ||
            !(status.usbPg != status.dcPg) || !status.boostPg) {
          fault = "shutdown observation status guard"; break;
        }
      }
      if (!isfinite(v) || v > kTrip) { fault = "shutdown observation overvoltage"; break; }
      if (elapsed - offBinStart >= kBinUs) {
        if (used >= kCapacity) { fault = "shutdown capture buffer full"; break; }
        samples[used++] = {elapsed, binMin, binMax, uint16_t((binSum + binCount / 2) / binCount),
                          status.internalInputs, status.internalOutputs, status.internalOutputInputs};
        offBinStart = elapsed; binSum = binCount = 0; binMin = UINT16_MAX; binMax = 0;
      }
      delay(1);
    }
  }

  // Print only after the charge command has been cleared. Input/output bytes are
  // the most recent status sample, refreshed nominally every 20 ms.
  log.println("CHARGE_CSV phase,time_us,adc_min_mv,adc_max_mv,adc_mean_mv,input,output,pins");
  for (size_t i = 0; i < used; ++i) {
    const Sample &s = samples[i];
    log.printf("CHARGE_CSV %s%s,%lu,%u,%u,%u,%u,%u,%u\n", name, i < activeUsed ? "" : "_shutdown",
               static_cast<unsigned long>(s.us), s.minMv, s.maxMv, s.meanMv,
               s.input, s.output, s.outputPins);
  }
  summary.ok = !fault && ceOff && summary.tailCount != 0;
  if (expectation == Expectation::Regulated)
    summary.ok = summary.ok && summary.tailMin >= kRegMin && summary.tailMax <= kRegMax &&
                 summary.tailMax - summary.tailMin <= 0.5f && summary.statTailLow == 0;
  if (expectation == Expectation::Inhibited)
    summary.ok = summary.ok && summary.maxV < 2.0f && summary.statTransitions >= 2;
  log.printf("CHARGE_SUMMARY phase=%s result=%s count=%lu min=%.4f max=%.4f tail_min=%.4f "
             "tail_max=%.4f tail_mean=%.4f stat_edges=%lu stat_tail_low=%lu first_window_us=%ld "
             "max_adc_gap_us=%lu ce_call_us=%lu stop_call_us=%lu sensor_mv=%lu\n",
             name, summary.ok ? "PASS" : "FAIL", static_cast<unsigned long>(summary.count),
             summary.minV, summary.maxV, summary.tailMin, summary.tailMax,
             summary.tailCount ? summary.tailSum / summary.tailCount : 0,
             static_cast<unsigned long>(summary.statTransitions), static_cast<unsigned long>(summary.statTailLow),
             static_cast<long>(summary.firstInWindowUs), static_cast<unsigned long>(summary.maxGapUs),
             static_cast<unsigned long>(start - requestUs), static_cast<unsigned long>(stoppedUs - stopRequestUs),
             static_cast<unsigned long>(status.tsMv));
  if (ce) log.printf("CHARGE_SHUTDOWN phase=%s first=%.4f last=%.4f below_1_us=%ld\n",
                     name, shutdownFirst, shutdownLast, static_cast<long>(belowOneUs));
  if (fault) log.printf("CHARGE_FAULT %s %s\n", fault, faultDetail);
  if (!summary.ok) board.allOff();
  return summary.ok;
}

bool waitDischarged(BoardControl &board, Stream &log) {
  const uint32_t start = millis();
  Status s;
  while (millis() - start < 10000) {
    if (!board.readStatus(s)) return action(false, board, log, "discharge status");
    if (s.internalOutputs & 2u) return action(false, board, log, "CE must be off during discharge");
    if (s.vcapV < 1.0f) {
      log.printf("PASS CHARGER_DISCHARGED elapsed_ms=%lu VCAP=%.4f\n",
                 static_cast<unsigned long>(millis() - start), s.vcapV); return true;
    }
    delay(10);
  }
  log.printf("FAIL CHARGER_DISCHARGE_TIMEOUT VCAP=%.4f\n", s.vcapV);
  return false;
}

bool sequence(BoardControl &board, Stream &log) {
  if (!action(board.allOff(), board, log, "initial cleanup") ||
      !action(board.setChargeHighCurrent(false), board, log, "low current") ||
      !action(board.setThermistorOverride(false), board, log, "real sensor selected") ||
      !action(board.setBoost(true), board, log, "boost on and PG stable")) return false;
  if (!waitDischarged(board, log)) return false;
  if (!phase(board, log, "real_ce_off", 1000, false, false, false, Expectation::Observe)) return false;
  if (!phase(board, log, "real_ntc", 3000, true, false, false, Expectation::Inhibited)) return false;
  if (!phase(board, log, "real_ce_off_decay", 1000, false, false, false, Expectation::Observe)) return false;
  if (!waitDischarged(board, log)) return false;
  if (!action(board.setThermistorOverride(true), board, log, "fixed TS selected with CE low")) return false;
  // The reusable selector waits for TS filtering/deglitch before returning.
  if (!phase(board, log, "override_ce_off", 500, false, true, false, Expectation::Observe)) return false;
  if (!phase(board, log, "override_low", 8000, true, true, false, Expectation::Regulated)) return false;
  if (!phase(board, log, "override_off_decay", 3000, false, true, false, Expectation::Observe)) return false;
  if (!waitDischarged(board, log)) return false;
  if (!phase(board, log, "override_restart", 4000, true, true, false, Expectation::Regulated)) return false;
  if (!waitDischarged(board, log)) return false;
  if (!action(board.setChargeHighCurrent(true), board, log, "high current selected with CE low")) return false;
  if (!phase(board, log, "override_high", 5000, true, true, true, Expectation::Regulated)) return false;
  if (!waitDischarged(board, log)) return false;
  if (!action(board.setChargeHighCurrent(false), board, log, "low current restored") ||
      !action(board.setThermistorOverride(false), board, log, "real sensor restored with CE low")) return false;
  if (!phase(board, log, "real_ntc_return", 3000, true, false, false, Expectation::Inhibited)) return false;
  return true;
}
} // namespace

void runChargerCharacterization(BoardControl &board, Stream &log) {
  log.printf("BEGIN CHARGER_DEEP no_supercap target=%.3f window=%.1f..%.1f trip=%.1f\n",
             kTarget, kRegMin, kRegMax, kTrip);
  log.println("Sampling begins after CE setter returns; RC filtering and polling do not measure switching ripple or peak overshoot.");
  const bool ok = sequence(board, log);
  const bool cleanup = action(board.allOff(), board, log, "final cleanup; override and CE off");
  Status final;
  const bool readable = board.readStatus(final);
  const bool off = readable && final.internalOutputs == 0 && final.externalOutputs == 0;
  if (readable)
    log.printf("CHARGE_FINAL OUT=%02X PINS=%02X EOUT=%02X VCAP=%.4f TS_mV=%lu PG=%u STAT=%u\n",
               final.internalOutputs, final.internalOutputInputs, final.externalOutputs, final.vcapV,
               static_cast<unsigned long>(final.tsMv), final.chargerPg, final.chargerStat);
  log.printf("RESULT CHARGER_DEEP %s cleanup=%s\n", ok && cleanup && off ? "PASS" : "FAIL", cleanup && off ? "PASS" : "FAIL");
}
