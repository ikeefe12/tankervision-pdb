#pragma once
#include "Arduino.h"
#include <deque>
#include <string>
namespace AppMock {
extern bool connected, rebootEnabled;
extern std::deque<uint8_t> rx;
extern std::string tx;
}
class USBCDC {
 public:
  void enableReboot(bool enabled) { AppMock::rebootEnabled = enabled; }
  void setTxTimeoutMs(uint32_t) {}
  void setRxBufferSize(size_t) {}
  void begin(unsigned) {}
  explicit operator bool() const { return AppMock::connected; }
  int available() const { return int(AppMock::rx.size()); }
  int read() {
    if (AppMock::rx.empty()) return -1;
    const auto value = AppMock::rx.front(); AppMock::rx.pop_front(); return value;
  }
  int availableForWrite() const { return AppMock::connected ? 256 : 0; }
  size_t write(uint8_t value) { return write(&value, 1); }
  size_t write(const uint8_t *data, size_t size) {
    if (!AppMock::connected) return 0;
    AppMock::tx.append(reinterpret_cast<const char *>(data), size); return size;
  }
};
