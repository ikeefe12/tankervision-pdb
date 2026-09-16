#pragma once
#include <array>
#include <cassert>
#include <cstdint>
#include <deque>
#include <vector>

inline std::array<uint8_t, 65536> pdMockRegisters;
inline std::vector<std::vector<uint8_t>> pdMockTransactions;
inline int pdMockShortAddress = -1, pdMockNackAddress = -1;
inline uint32_t pdMockTimeout = 0;
inline bool pdMockApplyRequest = true;
inline uint8_t pdMockCommandResponse = 2;
class TwoWire {
 public:
  explicit TwoWire(int controller) { assert(controller == 1); }
  void setTimeOut(uint32_t timeout) { pdMockTimeout = timeout; }
  bool begin(int sda, int scl, uint32_t frequency) {
    assert(sda == 40 && scl == 41 && frequency == 100000);
    return true;
  }
  void end() {}
  void beginTransmission(uint8_t address) { assert(address == 8); tx_.clear(); }
  size_t write(uint8_t byte) { tx_.push_back(byte); return 1; }
  uint8_t endTransmission(bool stop) {
    assert(tx_.size() >= 2 && stop == (tx_.size() > 2));
    pdMockTransactions.push_back(tx_);
    pointer_ = tx_[0] | (uint16_t(tx_[1]) << 8);
    if (pointer_ == pdMockNackAddress) return 2;
    if (tx_.size() > 2) {
      for (size_t i = 2; i < tx_.size(); ++i) pdMockRegisters[pointer_ + i - 2] = tx_[i];
      if (pointer_ == 0x1005) {
        pdMockRegisters[0x1400] = pdMockCommandResponse;
        if (pdMockApplyRequest) {
          const uint32_t current = pdMockRegisters[0x1808] |
                                   (uint32_t(pdMockRegisters[0x1809] & 3) << 8);
          const uint32_t rdo = (5UL << 28) | (current << 10) | current;
          for (int i = 0; i < 4; ++i) pdMockRegisters[0x1014 + i] = rdo >> (8 * i);
        }
      }
    }
    return 0;
  }
  size_t requestFrom(uint8_t address, size_t count, bool stop) {
    assert(address == 8 && stop);
    rx_.clear();
    if (pointer_ == pdMockShortAddress) --count;
    for (size_t i = 0; i < count; ++i) rx_.push_back(pdMockRegisters[pointer_ + i]);
    return count;
  }
  int available() const { return int(rx_.size()); }
  int read() {
    if (rx_.empty()) return -1;
    int value = rx_.front(); rx_.pop_front(); return value;
  }
 private:
  uint16_t pointer_ = 0;
  std::vector<uint8_t> tx_;
  std::deque<uint8_t> rx_;
};
