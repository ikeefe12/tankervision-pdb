#include "PdDiagnostics.h"
#include <cassert>
#include <cstdio>
#include <cstring>

void set32(uint16_t address, uint32_t value) {
  for (int i = 0; i < 4; ++i) pdMockRegisters[address + i] = value >> (8 * i);
}
void resetFixture() {
  pdMockRegisters.fill(0);
  pdMockTransactions.clear();
  pdMockShortAddress = pdMockNackAddress = -1;
  pdMockApplyRequest = true;
  pdMockCommandResponse = 2;
  pdMockMillis = 0;
  pdMockInterruptLevel = LOW;
  pdMockRegisters[0] = 0x95;
  pdMockRegisters[2] = 4;
  pdMockRegisters[3] = 0x20;
  pdMockRegisters[6] = 1;
  for (int i = 0; i < 16; ++i) pdMockRegisters[0x10 + i] = i;
  set32(0x1008, 0x000da400);
  pdMockRegisters[0x100c] = 0x89;
  set32(0x1010, (400UL << 10) | 300);  // 20 V, 3 A.
  set32(0x1014, (5UL << 28) | (300UL << 10) | 300);
}

int main() {
  Stream log;
  resetFixture();
  assert(runPdDiagnostics(log));
  assert(!pdMockHandlerAttached && pdMockTimeout == 25);
  assert(pdMockTransactions.size() == 24);
  const std::array<uint16_t, 8> expected = {0, 2, 0x10, 0x1008, 0x100c, 0x1010, 0x1014, 6};
  for (size_t i = 0; i < pdMockTransactions.size(); ++i) {
    const auto &tx = pdMockTransactions[i];
    assert(tx.size() == 2);
    assert(tx[0] == (expected[i % 8] & 255) && tx[1] == (expected[i % 8] >> 8));
  }

  resetFixture();
  {
    PdDiagnostics pd;
    assert(pd.begin());
    PdSnapshot s;
    assert(pd.readSnapshot(s));
    assert(s.selectedMillivolts() == 20000 && s.sourceMilliamps() == 3000);
    assert(s.operatingMilliamps() == 3000 && s.objectPosition() == 5);
    const PdSnapshot saved = s;
    pdMockShortAddress = 0x1014;
    assert(!pd.readSnapshot(s));
    assert(strstr(pd.error(), "short") != nullptr);
    assert(s.currentRdo == saved.currentRdo && s.pdStatus == saved.pdStatus);
    pdMockShortAddress = -1;
    pdMockNackAddress = 0x1008;
    assert(!pd.readSnapshot(s));
    assert(strstr(pd.error(), "I2C error 2") != nullptr);
  }
  assert(!pdMockHandlerAttached);  // Destruction releases ISR ownership.

  resetFixture(); pdMockRegisters[0] = 0xff; assert(!runPdDiagnostics(log));
  resetFixture(); set32(0x1010, (100UL << 10) | 300); assert(!runPdDiagnostics(log));
  resetFixture(); set32(0x1014, (5UL << 28) | (400UL << 10) | 400); assert(!runPdDiagnostics(log));
  resetFixture(); set32(0x1014, (300UL << 10) | 300); assert(!runPdDiagnostics(log));
  resetFixture(); set32(0x1014, (5UL << 28) | (1UL << 26) | (300UL << 10) | 300); assert(!runPdDiagnostics(log));
  resetFixture(); pdMockInterruptLevel = HIGH; assert(!runPdDiagnostics(log));
  resetFixture(); set32(0x1014, (5UL << 28) | (1UL << 27) | (300UL << 10) | 100); assert(runPdDiagnostics(log));
  resetFixture(); set32(0x1014, (5UL << 28) | (1UL << 27) | (100UL << 10) | 300); assert(!runPdDiagnostics(log));
  // Actual grounded-ISNK board result: valid communication/20 V source data,
  // but no validated nonzero operating-current allowance for board loads.
  Stream actualLog;
  resetFixture(); set32(0x1010, 0x000641d6); set32(0x1014, 0x4080000a); assert(!runPdDiagnostics(actualLog));
  assert(actualLog.text.find("PD COMMUNICATION PASS") != std::string::npos);
  assert(actualLog.text.find("PD POWER_REQUEST UNVALIDATED") != std::string::npos);
  assert(actualLog.text.find("PD SUMMARY PARTIAL") != std::string::npos);
  assert(actualLog.text.find("PD FAIL") == std::string::npos);
  {
    resetFixture();
    PdDiagnostics pd;
    assert(pd.begin());
    PdSnapshot accepted;
    // Already adequate contract is verified without changing PD configuration.
    assert(pd.requestFixed20VCurrent(2000, log, accepted));
    for (const auto &tx : pdMockTransactions) assert(tx.size() == 2);
    assert(accepted.operatingMilliamps() == 3000 && pdMockMillis >= 500);
  }
  {
    resetFixture();
    set32(0x1014, (5UL << 28) | 10);
    PdDiagnostics pd;
    assert(pd.begin());
    PdSnapshot accepted;
    assert(pd.requestFixed20VCurrent(2000, log, accepted));
    assert(accepted.operatingMilliamps() == 2000 && accepted.limitMilliamps() == 2000);
    unsigned writes = 0;
    for (const auto &tx : pdMockTransactions) {
      if (tx.size() == 2) continue;
      if (writes++ == 0) {
        assert(tx.size() == 34 && tx[0] == 0 && tx[1] == 0x18);
        const uint8_t expectedPayload[] = {0x50, 0x4b, 0x4e, 0x53,
          0x5a, 0x90, 0x01, 0x10, 0xc8, 0x40, 0x06, 0};
        assert(memcmp(tx.data() + 2, expectedPayload, sizeof(expectedPayload)) == 0);
        for (unsigned i = 14; i < 34; ++i) assert(tx[i] == 0);
      } else assert(tx.size() == 3 && tx[0] == 5 && tx[1] == 0x10 && tx[2] == 3);
    }
    assert(writes == 2);
  }
  {
    resetFixture();
    set32(0x1014, (5UL << 28) | 10);
    PdDiagnostics pd;
    assert(pd.begin());
    PdSnapshot accepted;
    accepted.currentRdo = 0xdeadbeef;
    // Stale SUCCESS never establishes a current allowance without a new RDO.
    pdMockApplyRequest = false;
    assert(!pd.requestFixed20VCurrent(2000, log, accepted));
    assert(strstr(pd.error(), "timed out") && accepted.currentRdo == 0xdeadbeef);
    assert(pdMockMillis >= 5000 && pdMockMillis < 5100);
  }
  {
    resetFixture();
    PdDiagnostics pd;
    assert(pd.begin());
    PdSnapshot accepted;
    assert(!pd.requestFixed20VCurrent(5000, log, accepted));
    assert(!pd.requestFixed20VCurrent(2001, log, accepted));
    assert(!pd.requestFixed20VCurrent(0, log, accepted));
    assert(pdMockTransactions.empty());
    set32(0x1010, (400UL << 10) | 150);
    set32(0x1014, (5UL << 28) | 10);
    assert(!pd.requestFixed20VCurrent(2000, log, accepted));
    for (const auto &tx : pdMockTransactions) assert(tx.size() == 2);
  }
  {
    resetFixture();
    set32(0x1014, (5UL << 28) | 10);
    pdMockNackAddress = 0x1800;
    PdDiagnostics pd;
    assert(pd.begin());
    PdSnapshot accepted;
    assert(!pd.requestFixed20VCurrent(2000, log, accepted));
    for (const auto &tx : pdMockTransactions)
      assert(!(tx.size() > 2 && tx[0] == 5 && tx[1] == 0x10));
  }
  std::puts("PASS PD transport/read-only pointers, decoding, short/NACK isolation, identity/contract bounds, IRQ mismatch, GiveBack and ISR lifetime");
  std::puts("PASS PD current request: exact wire payload/mask, stable active RDO, adequate-contract no-write path, stale response timeout, argument/source budget and NACK guards");
}
