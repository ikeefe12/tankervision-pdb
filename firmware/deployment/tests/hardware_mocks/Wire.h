#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>
struct MockExpander {
  bool present = true;
  std::array<uint8_t, 8> reg = {0, 0, 0xff, 0xff, 0, 0, 0xff, 0xff};
  uint8_t forcedHigh = 0, ignoredOutput = 0, ignoredConfig = 0;
  uint8_t physical() const { return (reg[3] & ~reg[7]) | forcedHigh; }
};
struct MockWrite { uint8_t address, reg, value, physical; uint32_t at; };
class TwoWire {
 public:
  MockExpander internal, external;
  bool mainPg = true, boostPg = true, buckPg = true, automaticInputs = true;
  uint8_t internalInput = 0, externalInput = 0;
  std::vector<MockWrite> writes;
  bool begin(int, int, uint32_t) { return true; }
  void setTimeOut(uint32_t) {}
  void beginTransmission(uint8_t address) { address_ = address; tx_.clear(); }
  size_t write(uint8_t value) { tx_.push_back(value); return 1; }
  uint8_t endTransmission(bool = true);
  size_t requestFrom(uint8_t, uint8_t);
  int available() const { return rx_.size(); }
  int read() { int v = rx_.front(); rx_.pop_front(); return v; }
 private:
  uint8_t address_ = 0, pointer_ = 0;
  std::vector<uint8_t> tx_;
  std::deque<uint8_t> rx_;
};
extern TwoWire Wire;
