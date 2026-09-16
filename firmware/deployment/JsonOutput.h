#pragma once
#include "Protocol.h"
#include <USBCDC.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

class JsonOutput {
 public:
  void clear() { size_ = 0; ok_ = true; data_[0] = 0; }
  void add(const char *format, ...) {
    if (!ok_) return;
    va_list args; va_start(args, format);
    const int n = vsnprintf(data_ + size_, sizeof(data_) - size_, format, args);
    va_end(args);
    if (n < 0 || size_t(n) >= sizeof(data_) - size_) { ok_ = false; return; }
    size_ += size_t(n);
  }
  void quoted(const char *s) {
    add("\"");
    for (; *s; ++s) {
      const unsigned char c = *s;
      if (c == '"' || c == '\\') add("\\%c", c);
      else if (c < 32) add("\\u%04x", unsigned(c));
      else add("%c", c);
    }
    add("\"");
  }
  const char *data() const { return data_; }
  size_t size() const { return size_; }
  bool ok() const { return ok_; }
 private:
  char data_[Protocol::kMaxMessage] = {};
  size_t size_ = 0;
  bool ok_ = true;
};

// Whole JSON frames, no interleaving of a partially written line. Reserve queue
// capacity for replies/events; telemetry drops never hold up the power manager.
class UsbOutput {
 public:
  bool enqueue(const JsonOutput &message, bool telemetry = false) {
    if (!message.ok() || !connected_ || count_ >= (telemetry ? 2u : kSlots)) {
      ++dropped_; if (telemetry) ++telemetryDropped_; return false;
    }
    Slot &slot = slots_[(head_ + count_) % kSlots];
    memcpy(slot.data, message.data(), message.size());
    slot.length = message.size(); slot.at = 0; ++count_; return true;
  }
  void service(USBCDC &port) {
    const bool connected = bool(port);
    if (!connected) {
      if (connected_) { dropped_ += count_; count_ = 0; head_ = 0; }
      connected_ = false; separator_ = true; return;
    }
    connected_ = true;
    if (separator_) { if (port.write(uint8_t('\n')) == 1) separator_ = false; return; }
    if (!count_) return;
    const int available = port.availableForWrite();
    if (available <= 0) return;
    Slot &slot = slots_[head_];
    size_t n = slot.length - slot.at;
    if (n > size_t(available)) n = available;
    if (n > 256) n = 256;
    slot.at += port.write(reinterpret_cast<const uint8_t *>(slot.data + slot.at), n);
    if (slot.at == slot.length) { head_ = (head_ + 1) % kSlots; --count_; }
  }
  uint32_t dropped() const { return dropped_; }
  uint32_t telemetryDropped() const { return telemetryDropped_; }
  unsigned queued() const { return count_; }
  bool connected() const { return connected_; }
  bool roomForReply() const { return connected_ && count_ <= kSlots - 2; }
 private:
  static constexpr unsigned kSlots = 6;
  struct Slot { char data[Protocol::kMaxMessage]; size_t length = 0, at = 0; } slots_[kSlots];
  unsigned head_ = 0, count_ = 0;
  uint32_t dropped_ = 0, telemetryDropped_ = 0;
  bool connected_ = false, separator_ = true;
};
