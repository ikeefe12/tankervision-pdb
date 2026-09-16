#include "PowerDelivery.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

uint32_t fakeMs = 100;
PdBusMock fakeBus;
static unsigned scenarios = 0;
#define CHECK(expression) do { if (!(expression)) { \
  std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); std::exit(1); \
} } while (0)

static uint32_t fixedPdo(unsigned millivolts = 20000, unsigned milliamps = 4700) {
  return ((millivolts / 50) << 10) | (milliamps / 10);
}
static uint32_t fixedRdo(unsigned operatingMa, unsigned maximumMa, unsigned object = 4) {
  return (object << 28) | (1u << 23) | ((operatingMa / 10) << 10) | (maximumMa / 10);
}
static void grant(unsigned operatingMa, unsigned maximumMa = 3000) {
  fakeBus.put(0x1014, fixedRdo(operatingMa, maximumMa));
}
static void reset(unsigned operatingMa = 0, unsigned maximumMa = 100) {
  fakeMs = 100; fakeBus = {};
  fakeBus.put(0x0000, 0x95, 1); fakeBus.put(0x0002, 0x2004, 2);
  fakeBus.put(0x1008, 0, 4); fakeBus.put(0x100c, 1, 1);
  fakeBus.put(0x1010, fixedPdo()); grant(operatingMa, maximumMa);
}
static void beginAndStart(PowerDelivery &pd) {
  CHECK(pd.begin()); CHECK(pd.poll(true)); CHECK(pd.startBudget());
  CHECK(pd.result() == PowerDelivery::Result::Running); CHECK(!pd.budgetValid());
}
static uint32_t le(const std::vector<uint8_t> &bytes, size_t offset) {
  return uint32_t(bytes[offset]) | uint32_t(bytes[offset + 1]) << 8 |
         uint32_t(bytes[offset + 2]) << 16 | uint32_t(bytes[offset + 3]) << 24;
}
static void tick(PowerDelivery &pd, uint32_t advance = 25) { fakeMs += advance; pd.service(); }
static void confirmExisting(PowerDelivery &pd) {
  pd.service();
  tick(pd, 475); CHECK(pd.result() == PowerDelivery::Result::Running);
  tick(pd, 25); CHECK(pd.result() == PowerDelivery::Result::Succeeded); CHECK(pd.budgetValid());
}

static void readOnlyMonitoring() {
  reset(); PowerDelivery pd; CHECK(pd.begin());
  CHECK(fakeBus.busIndex == 1 && fakeBus.sda == 40 && fakeBus.scl == 41);
  CHECK(fakeBus.frequency == 100000 && fakeBus.timeout == 25);
  CHECK(!pd.poll(false)); CHECK(fakeBus.reads == 0); CHECK(pd.poll(true));
  CHECK(pd.reading().millivolts() == 20000 && pd.reading().sourceMa() == 4700);
  CHECK(pd.reading().operatingMa() == 0 && pd.reading().maximumMa() == 100);
  CHECK(fakeBus.writes.empty()); CHECK(!pd.budgetValid()); ++scenarios;
}
static void payloadAndVerifiedGrant() {
  reset(); PowerDelivery pd; beginAndStart(pd);
  static_assert(PowerDelivery::kRequestedMa == 3000, "Deployment budget must request 3 A");
  pd.service(); CHECK(fakeBus.writes.size() == 1);
  const auto &payload = fakeBus.writes[0]; CHECK(payload.address == 0x1800 && payload.bytes.size() == 32);
  CHECK(le(payload.bytes, 0) == 0x534e4b50); // CYPD3177 SNKP signature, little endian.
  CHECK(le(payload.bytes, 4) == ((1u << 28) | (100u << 10) | 90u)); // 5V/900mA fallback.
  CHECK(le(payload.bytes, 8) == ((400u << 10) | 300u)); // Fixed20V/3A.
  for (size_t i = 12; i < payload.bytes.size(); ++i) CHECK(payload.bytes[i] == 0);
  pd.service(); CHECK(fakeBus.writes.size() == 2);
  CHECK(fakeBus.writes[1].address == 0x1005 && fakeBus.writes[1].bytes == std::vector<uint8_t>{3});
  grant(3000); fakeBus.put(0x1400, 2);
  confirmExisting(pd); CHECK(fakeBus.writes.size() == 2); ++scenarios;
}
static void existingGrantIsReadOnlyAndStable() {
  reset(3000, 3000); PowerDelivery pd; beginAndStart(pd);
  confirmExisting(pd); CHECK(fakeBus.writes.empty()); ++scenarios;
}
static void insufficientStaleSuccessDoesNotAuthorize() {
  reset(); fakeBus.put(0x1400, 2); PowerDelivery pd; beginAndStart(pd);
  pd.service(); pd.service();
  for (unsigned i = 0; i < 199; ++i) { tick(pd); CHECK(!pd.budgetValid()); }
  CHECK(pd.result() == PowerDelivery::Result::Running);
  tick(pd); CHECK(pd.result() == PowerDelivery::Result::Failed);
  CHECK(!std::strcmp(pd.error(), "PD_BUDGET_TIMEOUT")); CHECK(fakeBus.writes.size() == 2); ++scenarios;
}
static void grantWithoutSuccessfulResponseDoesNotAuthorize() {
  reset(); PowerDelivery pd; beginAndStart(pd); pd.service(); pd.service(); grant(3000);
  for (unsigned i = 0; i < 20; ++i) tick(pd);
  CHECK(pd.result() == PowerDelivery::Result::Running); CHECK(!pd.budgetValid());
  fakeBus.put(0x1400, 2); tick(pd);
  tick(pd, 475); CHECK(!pd.budgetValid()); tick(pd, 25); CHECK(pd.budgetValid()); ++scenarios;
}
static void unstableGrantRestartsTheHalfSecond() {
  reset(3000, 3000); PowerDelivery pd; beginAndStart(pd); pd.service(); tick(pd, 475);
  grant(2500, 3000); tick(pd); CHECK(!pd.budgetValid());
  grant(3000); tick(pd); tick(pd, 475); CHECK(!pd.budgetValid()); tick(pd, 25); CHECK(pd.budgetValid()); ++scenarios;
}
static void sourceAndObjectMustStayTheSame() {
  for (unsigned changed = 0; changed < 2; ++changed) {
    reset(); PowerDelivery pd; beginAndStart(pd); pd.service(); pd.service(); fakeBus.put(0x1400, 2);
    if (changed == 0) { fakeBus.put(0x1010, fixedPdo(20000, 4000)); grant(3000); }
    else fakeBus.put(0x1014, fixedRdo(3000, 3000, 3));
    tick(pd, 1000); CHECK(!pd.budgetValid());
    tick(pd, 4000); CHECK(pd.result() == PowerDelivery::Result::Failed); ++scenarios;
  }
}
static void invalidStartingSourcesNeverWrite() {
  for (unsigned invalid = 0; invalid < 10; ++invalid) {
    reset(); PowerDelivery pd; CHECK(pd.begin());
    switch (invalid) {
      case 0: fakeBus.put(0x0000, 0x94, 1); break;
      case 1: fakeBus.put(0x0002, 0x2003, 2); break;
      case 2: fakeBus.put(0x100c, 0, 1); break;
      case 3: fakeBus.put(0x1010, fixedPdo(15000)); break;
      case 4: fakeBus.put(0x1010, fixedPdo(20000, 2900)); break;
      case 5: fakeBus.put(0x1008, 1u << 8); break;
      case 6: fakeBus.put(0x1010, fixedPdo() | (3u << 30)); break;
      case 7: fakeBus.put(0x1014, fixedRdo(3000, 2000)); break;
      case 8: fakeBus.put(0x1014, fixedRdo(3000, 3000) | (1u << 26)); break;
      case 9: fakeBus.put(0x1014, fixedRdo(0, 100, 0)); break;
    }
    CHECK(pd.poll(true)); CHECK(!pd.startBudget()); CHECK(fakeBus.writes.empty()); ++scenarios;
  }
}
static void failedAndStaleInitialReadsNeverWrite() {
  reset(); PowerDelivery pd; CHECK(!pd.startBudget()); CHECK(pd.begin());
  fakeBus.pointerFails = true; CHECK(!pd.poll(true)); CHECK(!pd.startBudget());
  fakeBus.pointerFails = false; CHECK(pd.poll(true)); fakeMs += 1501;
  CHECK(!pd.startBudget()); CHECK(fakeBus.writes.empty()); ++scenarios;
  reset(); fakeBus.beginOk = false; PowerDelivery absent; CHECK(!absent.begin());
  CHECK(!absent.poll(true)); CHECK(fakeBus.reads == 0); ++scenarios;
}
static void ioFailuresStopNegotiation() {
  for (unsigned failure = 0; failure < 6; ++failure) {
    reset(); PowerDelivery pd; beginAndStart(pd);
    switch (failure) {
      case 0: fakeBus.shortWrite = true; pd.service(); break;
      case 1: fakeBus.dataFails = true; pd.service(); break;
      case 2: pd.service(); fakeBus.dataFails = true; pd.service(); break;
      default:
        pd.service(); pd.service();
        if (failure == 3) fakeBus.pointerFails = true;
        if (failure == 4) fakeBus.shortResponse = true;
        if (failure == 5) fakeBus.negativeRead = true;
        pd.service(); break;
    }
    CHECK(pd.result() == PowerDelivery::Result::Failed); CHECK(!pd.budgetValid());
    const auto writes = fakeBus.writes.size(); tick(pd, 6000); CHECK(fakeBus.writes.size() == writes); ++scenarios;
  }
}
static void successInvalidatesOnStaleOrChangedEvidence() {
  for (unsigned change = 0; change < 7; ++change) {
    reset(3000, 3000); PowerDelivery pd; beginAndStart(pd); confirmExisting(pd);
    switch (change) {
      case 0: fakeMs += 1500; CHECK(pd.budgetValid()); ++fakeMs; break;
      case 1: fakeBus.shortResponse = true; CHECK(!pd.poll(true)); break;
      case 2: CHECK(!pd.poll(false)); break;
      case 3: grant(2500); CHECK(pd.poll(true)); break;
      case 4: fakeBus.put(0x1010, fixedPdo(15000)); CHECK(pd.poll(true)); break;
      case 5: fakeBus.put(0x100c, 0, 1); CHECK(pd.poll(true)); break;
      case 6: pd.cancel(); break;
    }
    CHECK(!pd.budgetValid()); ++scenarios;
  }
}
static void rolloverTimingAndCancellation() {
  reset(3000, 3000); fakeMs = std::numeric_limits<uint32_t>::max() - 200;
  PowerDelivery pd; beginAndStart(pd); confirmExisting(pd); CHECK(fakeMs == 299);
  CHECK(fakeBus.writes.empty()); ++scenarios;
  reset(); PowerDelivery pending; beginAndStart(pending); pending.service(); pending.cancel();
  tick(pending, 100); CHECK(fakeBus.writes.size() == 1); CHECK(!pending.budgetValid()); ++scenarios;
}

int main() {
  readOnlyMonitoring(); payloadAndVerifiedGrant(); existingGrantIsReadOnlyAndStable();
  insufficientStaleSuccessDoesNotAuthorize(); grantWithoutSuccessfulResponseDoesNotAuthorize();
  unstableGrantRestartsTheHalfSecond(); sourceAndObjectMustStayTheSame();
  invalidStartingSourcesNeverWrite(); failedAndStaleInitialReadsNeverWrite();
  ioFailuresStopNegotiation(); successInvalidatesOnStaleOrChangedEvidence(); rolloverTimingAndCancellation();
  std::printf("PowerDelivery: %u host scenarios passed\n", scenarios);
}
