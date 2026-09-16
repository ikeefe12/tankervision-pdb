#pragma once
#include <array>
#include <vector>
#include <stdint.h>
#include <stddef.h>

struct PdBusMock {
  struct Write { uint16_t address; std::vector<uint8_t> bytes; };
  std::array<uint8_t, 65536> registers{};
  std::vector<Write> writes;
  std::vector<uint8_t> tx, rx;
  uint16_t pointer = 0;
  size_t rxAt = 0;
  bool beginOk = true, pointerFails = false, dataFails = false;
  bool shortResponse = false, negativeRead = false, shortWrite = false;
  unsigned beginCount = 0, reads = 0, busIndex = 0;
  int sda = -1, scl = -1;
  unsigned frequency = 0, timeout = 0;
  void put(uint16_t address, uint32_t value, unsigned length = 4) {
    for (unsigned i = 0; i < length; ++i) registers[address + i] = uint8_t(value >> (8 * i));
  }
};
extern PdBusMock fakeBus;

class TwoWire {
 public:
  explicit TwoWire(unsigned index) { fakeBus.busIndex = index; }
  void setTimeOut(unsigned ms) { fakeBus.timeout = ms; }
  bool begin(int sda, int scl, unsigned frequency) {
    ++fakeBus.beginCount; fakeBus.sda = sda; fakeBus.scl = scl;
    fakeBus.frequency = frequency; return fakeBus.beginOk;
  }
  void beginTransmission(uint8_t address) { device_ = address; fakeBus.tx.clear(); }
  size_t write(uint8_t byte) { fakeBus.tx.push_back(byte); return 1; }
  size_t write(const uint8_t *data, size_t length) {
    if (fakeBus.shortWrite) return length ? length - 1 : 0;
    fakeBus.tx.insert(fakeBus.tx.end(), data, data + length); return length;
  }
  uint8_t endTransmission(bool = true) {
    if (device_ != 0x08 || fakeBus.tx.size() < 2) return 4;
    const uint16_t address = fakeBus.tx[0] | (uint16_t(fakeBus.tx[1]) << 8);
    if (fakeBus.tx.size() == 2) {
      if (fakeBus.pointerFails) return 4;
      fakeBus.pointer = address; return 0;
    }
    if (fakeBus.dataFails) return 4;
    const std::vector<uint8_t> bytes(fakeBus.tx.begin() + 2, fakeBus.tx.end());
    fakeBus.writes.push_back({address, bytes});
    for (size_t i = 0; i < bytes.size(); ++i) fakeBus.registers[address + i] = bytes[i];
    return 0;
  }
  size_t requestFrom(uint8_t address, size_t length, bool) {
    ++fakeBus.reads; fakeBus.rxAt = 0; fakeBus.rx.clear();
    if (address != 0x08) return 0;
    const size_t received = fakeBus.shortResponse && length ? length - 1 : length;
    for (size_t i = 0; i < received; ++i) fakeBus.rx.push_back(fakeBus.registers[fakeBus.pointer + i]);
    return received;
  }
  int available() { return int(fakeBus.rx.size() - fakeBus.rxAt); }
  int read() {
    if (fakeBus.negativeRead) return -1;
    return fakeBus.rxAt < fakeBus.rx.size() ? fakeBus.rx[fakeBus.rxAt++] : -1;
  }
 private:
  uint8_t device_ = 0;
};
