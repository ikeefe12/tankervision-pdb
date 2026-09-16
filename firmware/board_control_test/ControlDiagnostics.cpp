#include "ControlDiagnostics.h"
#include "ExpanderInterrupts.h"
#include "PdDiagnostics.h"
#include <math.h>

namespace {
constexpr float kMinV = 19.8f, kMaxV = 21.4f, kTripV = 21.5f;
constexpr uint8_t kChargeOutputs = 0x0b;

class Run {
 public:
  Run(BoardControl &board, Stream &log) : board(board), log(log) {}
  BoardControl &board;
  Stream &log;
  ExpanderInterrupts irq;
  unsigned passes = 0, failures = 0;
  uint8_t baseInputs = 0, wantedOutputs = kChargeOutputs;
  bool backupEnabled = false;

  bool check(bool ok, const char *name) {
    if (ok) ++passes; else ++failures;
    log.printf("%s CONTROL_CHECK %s%s%s\n", ok ? "PASS" : "FAIL", name,
               ok || !board.error()[0] ? "" : ": ", ok ? "" : board.error());
    return ok;
  }

  bool guardedStatus(Status &s, bool starting = false) {
    if (!board.readStatus(s)) return check(false, "status read");
    const bool source = s.mainSelected && (s.usbPg || s.dcPg) && s.ssBuckPg;
    const bool voltage = isfinite(s.vcapV) && s.vcapV <= kTripV &&
        (starting || (s.vcapV >= kMinV && s.vcapV <= kMaxV && s.chargerStat));
    if (!source || !voltage || !s.boostPg || !s.chargerPg ||
        s.internalOutputs != wantedOutputs ||
        digitalRead(42) != (backupEnabled ? HIGH : LOW)) {
      log.printf("CONTROL_GUARD IN=%02X OUT=%02X EOUT=%02X VCAP=%.4f STAT=%u MAIN=%u GPIO42=%d\n",
                 s.internalInputs, s.internalOutputs, s.externalOutputs, s.vcapV,
                 s.chargerStat, s.mainSelected, digitalRead(42));
      return check(false, "source/charger/voltage/enable guard");
    }
    return true;
  }

  bool startCharger() {
    if (!check(board.allOff(), "initial all off") ||
        !check(board.setBackup(false), "backup initially disabled") ||
        !check(board.setChargeHighCurrent(false), "low charge-current profile") ||
        !check(board.setThermistorOverride(true), "fixed TS selected and settled") ||
        !check(board.setBoost(true), "boost enabled and PG qualified") ||
        !check(board.setCharger(true), "charger enabled")) return false;
    const uint32_t started = millis();
    uint32_t stableAt = 0;
    bool timing = false;
    while (millis() - started < 2000) {
      Status s;
      if (!guardedStatus(s, true)) return false;
      if (s.vcapV >= kMinV && s.vcapV <= kMaxV && s.chargerStat &&
          !s.backupPg && !s.buckPg && (s.externalInputs & 0x1f) == 0 && s.externalOutputs == 0) {
        if (!timing) { timing = true; stableAt = millis(); }
        if (millis() - stableAt >= 100) {
          baseInputs = s.internalInputs;
          log.printf("CONTROL_REGULATED VCAP=%.4f IN=%02X OUT=%02X PG=%u STAT=%u MAIN=%u\n",
                     s.vcapV, s.internalInputs, s.internalOutputs, s.chargerPg, s.chargerStat, s.mainSelected);
          return check(true, "unloaded VCAP regulation qualified");
        }
      } else timing = false;
      delay(10);
    }
    return check(false, "unloaded VCAP regulation timeout");
  }

  bool dwell(uint32_t duration, uint8_t expectedIn, uint8_t expectedExt) {
    const uint32_t start = millis();
    while (millis() - start < duration) {
      Status s;
      if (!guardedStatus(s)) return false;
      if (s.internalInputs != expectedIn || (s.externalInputs & 0x1f) != expectedExt)
        return check(false, "PG changed during dwell");
      delay(10);
    }
    return true;
  }

  template <typename Action>
  bool transition(const char *name, bool internalLine, uint8_t expectedIn,
                  uint8_t expectedExt, Action action, bool requireHeldLow = false) {
    Status initial;
    ExpanderInterrupts::AckResult initialAck;
    if (!irq.acknowledge(board, initial, initialAck)) return check(false, "initial IRQ acknowledge read");
    if (!check(!initialAck.after.internal.activeLow && !initialAck.after.external.activeLow,
               "both IRQ lines initially released")) return false;
    const auto before = irq.snapshot();
    if (!check(action(), name)) return false;

    bool sawHeldLow = false, ackReleased = false;
    if (requireHeldLow) {
      // GPIO42 has no expander write/read after it changes. Observe its PG falling
      // IRQ before any status read acknowledges it. Continue direct voltage/mux
      // checks during this bounded gap in I2C polling.
      const uint32_t waitStart = millis();
      while (millis() - waitStart < 1500) {
        const uint32_t mv = board.readAdcMv(2);
        const float v = mv * 0.0092f;
        if (mv == UINT32_MAX || !isfinite(v) || v < kMinV || v > kMaxV || digitalRead(11) != HIGH)
          return check(false, "direct VCAP/main guard while awaiting held interrupt");
        if (irq.snapshot().internal.activeLow) { sawHeldLow = true; break; }
        delay(2);
      }
      Status acknowledged;
      ExpanderInterrupts::AckResult ack;
      if (!irq.acknowledge(board, acknowledged, ack)) return check(false, "held IRQ acknowledge read");
      ackReleased = ack.before.internal.activeLow && !ack.after.internal.activeLow &&
                    !ack.after.external.activeLow && acknowledged.internalInputs == expectedIn &&
                    (acknowledged.externalInputs & 0x1f) == expectedExt;
      log.printf("IRQ_ACK %s held_low=%u released_after_read=%u IN=%02X EXT=%02X\n",
                 name, sawHeldLow, ackReleased, acknowledged.internalInputs, acknowledged.externalInputs);
      if (!check(sawHeldLow && ackReleased, "held internal interrupt clears on input read")) return false;
    }

    const uint32_t start = millis();
    uint32_t stableAt = 0;
    bool timing = false, settled = false;
    Status final;
    while (millis() - start < 2500) {
      if (!guardedStatus(final)) return false;
      if (final.internalInputs == expectedIn && (final.externalInputs & 0x1f) == expectedExt) {
        if (!timing) { timing = true; stableAt = millis(); }
        if (millis() - stableAt >= 100) { settled = true; break; }
      } else timing = false;
      delay(10);
    }
    const auto after = irq.snapshot();
    const uint32_t inFall = after.internal.fallingEdges - before.internal.fallingEdges;
    const uint32_t inRise = after.internal.risingEdges - before.internal.risingEdges;
    const uint32_t extFall = after.external.fallingEdges - before.external.fallingEdges;
    const uint32_t extRise = after.external.risingEdges - before.external.risingEdges;
    const bool expectedEdges = internalLine ? (inFall && inRise && !extFall && !extRise) :
                                             (extFall && extRise && !inFall && !inRise);
    const bool ok = settled && expectedEdges && !after.internal.activeLow && !after.external.activeLow;
    log.printf("IRQ_TRANSITION name=%s result=%s before_IN=%02X after_IN=%02X expected_IN=%02X "
               "before_EXT=%02X after_EXT=%02X expected_EXT=%02X in_fall=%lu in_rise=%lu "
               "ext_fall=%lu ext_rise=%lu GPIO42=%d VCAP=%.4f MAIN=%u\n",
               name, ok ? "PASS" : "FAIL", initial.internalInputs, final.internalInputs, expectedIn,
               initial.externalInputs, final.externalInputs, expectedExt,
               static_cast<unsigned long>(inFall), static_cast<unsigned long>(inRise),
               static_cast<unsigned long>(extFall), static_cast<unsigned long>(extRise),
               digitalRead(42), final.vcapV, final.mainSelected);
    return check(ok, "PG transition and corresponding interrupt delivery") && dwell(500, expectedIn, expectedExt);
  }

  bool switchingTests() {
    if (!startCharger()) return false;
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
      log.printf("BACKUP_CYCLE %u\n", cycle + 1);
      backupEnabled = true;
      if (!transition("backup_on", true, baseInputs | 0x20, 0,
                      [&] { return board.setBackup(true); })) return false;
      backupEnabled = false;
      if (!transition("backup_off", true, baseInputs, 0,
                      [&] { return board.setBackup(false); }, true)) return false;
    }
    log.println("RESULT BACKUP_CONTROL PASS cycles=3; main remained selected; no source-loss transfer");
    wantedOutputs = kChargeOutputs | 0x10;
    if (!transition("buck_on", true, baseInputs | 0x02, 0,
                    [&] { return board.setBuck5V(true); })) return false;
    wantedOutputs = kChargeOutputs;
    if (!transition("buck_off", true, baseInputs, 0,
                    [&] { return board.setBuck5V(false); })) return false;

    // Pre-enable the J8 prerequisite so each following external transition has
    // exactly one intended expander input change and a stable internal baseline.
    wantedOutputs = kChargeOutputs | 0x10;
    if (!transition("buck_on_for_ports", true, baseInputs | 0x02, 0,
                    [&] { return board.setBuck5V(true); })) return false;
    const Port ports[] = {Port::Vbus, Port::FiveVoltVbus, Port::Supervised, Port::FiveVoltSupervised};
    const uint8_t bits[] = {0x08, 0x10, 0x04, 0x02};
    const char *onNames[] = {"J7_on", "J8_on", "J9_on", "J10_on"};
    const char *offNames[] = {"J7_off", "J8_off", "J9_off", "J10_off"};
    for (unsigned i = 0; i < 4; ++i) {
      if (!transition(onNames[i], false, baseInputs | 0x02, bits[i],
                      [&] { return board.setPort(ports[i], true); }) ||
          !transition(offNames[i], false, baseInputs | 0x02, 0,
                      [&] { return board.setPort(ports[i], false); })) return false;
    }
    wantedOutputs = kChargeOutputs;
    if (!transition("buck_final_off", true, baseInputs, 0,
                    [&] { return board.setBuck5V(false); })) return false;
    log.println("RESULT EXPANDER_IRQ PASS internal=GPIO15 external=GPIO16");
    return true;
  }

  bool ledTest() {
    log.println("LED_VISUAL D7: three 1.5-second ON pulses, each followed by 1.5 seconds OFF");
    bool ok = true;
    for (unsigned i = 0; i < 3 && ok; ++i) {
      ok = check(board.setStatusLed(true), "D7 GPIO4 high readback");
      if (!ok) break;
      log.printf("LED_D7 ON pulse=%u\n", i + 1); delay(1500);
      ok = check(board.setStatusLed(false), "D7 GPIO4 low readback");
      log.printf("LED_D7 OFF pulse=%u\n", i + 1); delay(1500);
    }
    const bool off = check(board.setStatusLed(false), "D7 final off");
    log.printf("RESULT STATUS_LED %s visual_confirmation=PENDING\n", ok && off ? "GPIO_PASS" : "FAIL");
    return ok && off;
  }
};
}

void runControlDiagnostics(BoardControl &board, Stream &log) {
  Run run(board, log);
  log.println("BEGIN CONTROL_NEXT tests=1,2,3,6 no_loads no_supercap no_current_ADC");
  if (!run.check(board.allOff(), "suite initial cleanup")) return;
  // PD accesses cannot delay a live charge-control loop: run with CE low.
  const bool pdOk = runPdDiagnostics(log);
  run.irq.begin();
  const bool switchingOk = run.switchingTests();
  const bool cleanup = run.check(board.allOff(), "switching cleanup; CE and boost off");
  run.irq.end();
  const bool ledOk = cleanup && run.ledTest();
  Status final;
  const bool readable = board.readStatus(final);
  const bool off = readable && final.internalOutputs == 0 && final.externalOutputs == 0 && digitalRead(42) == LOW;
  if (readable) log.printf("CONTROL_FINAL IN=%02X OUT=%02X EXT=%02X EOUT=%02X GPIO42=%d GPIO4=%d VCAP=%.4f MAIN=%u\n",
                          final.internalInputs, final.internalOutputs, final.externalInputs, final.externalOutputs,
                          digitalRead(42), digitalRead(4), final.vcapV, final.mainSelected);
  log.printf("RESULT CONTROL_NEXT %s switching=%s pd=%s led_gpio=%s cleanup=%s checks_pass=%u checks_fail=%u led_visual=PENDING\n",
             switchingOk && pdOk && ledOk && cleanup && off ? "PASS" : "FAIL",
             switchingOk ? "PASS" : "FAIL", pdOk ? "PASS" : "FAIL", ledOk ? "PASS" : "FAIL",
             cleanup && off ? "PASS" : "FAIL", run.passes, run.failures);
}
