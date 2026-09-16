#include "PowerDelivery.h"
#include <string.h>
namespace {
uint32_t le(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
void put(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = v >> (8 * i); }
}
bool PowerDelivery::fail(const char *message) { snprintf(error_, sizeof(error_), "%s", message); return false; }
bool PowerDelivery::begin() {
  bus_.setTimeOut(25); begun_ = bus_.begin(40, 41, 100000);
  return begun_ || fail("PD_BUS_BEGIN_FAILED");
}
bool PowerDelivery::read(uint16_t address, uint8_t *bytes, size_t n) {
  bus_.beginTransmission(0x08); bus_.write(uint8_t(address)); bus_.write(uint8_t(address >> 8));
  if (bus_.endTransmission(false)) return fail("PD_I2C_POINTER_FAILED");
  if (bus_.requestFrom(uint8_t(0x08), n, true) != n) {
    while (bus_.available()) bus_.read();
    return fail("PD_I2C_READ_FAILED");
  }
  for (size_t i = 0; i < n; ++i) { const int value = bus_.read(); if (value < 0) return fail("PD_I2C_SHORT_READ"); bytes[i] = value; }
  return true;
}
bool PowerDelivery::write(uint16_t address, const uint8_t *bytes, size_t n) {
  bus_.beginTransmission(0x08); bus_.write(uint8_t(address)); bus_.write(uint8_t(address >> 8));
  if (bus_.write(bytes, n) != n) return fail("PD_I2C_BUFFER_FAILED");
  return !bus_.endTransmission(true) || fail("PD_I2C_WRITE_FAILED");
}
bool PowerDelivery::poll(bool usbMainPresent) {
  reading_.valid = false;
  if (!begun_ || !usbMainPresent) return false;
  PdReading s; uint8_t b[4];
  if (!read(0x0000, &s.mode, 1) || !read(0x0002, b, 2)) return false;
  s.silicon = uint16_t(b[0]) | uint16_t(b[1]) << 8;
  if (!read(0x0010, s.versions, sizeof(s.versions)) || !read(0x1008, b, 4)) return false;
  s.status = le(b);
  if (!read(0x100c, &s.typeC, 1) || !read(0x1010, b, 4)) return false;
  s.pdo = le(b); if (!read(0x1014, b, 4)) return false; s.rdo = le(b);
  if (!read(0x0006, &s.interrupt, 1) || !read(0x1400, b, 4)) return false;
  s.response = le(b); s.atMs = millis(); s.valid = true; reading_ = s; return true;
}
bool PowerDelivery::sensible(const PdReading &s) const {
  return s.valid && s.mode == 0x95 && s.silicon == 0x2004 && s.attached() &&
    !(s.status & (1u << 8)) && !(s.pdo >> 30) && s.millivolts() == 20000 &&
    s.sourceMa() >= kRequestedMa && s.sourceMa() <= 5000 && ((s.rdo >> 28) & 7) &&
    !(s.rdo & ((1u << 26) | (1u << 27))) && s.operatingMa() <= s.sourceMa() &&
    s.maximumMa() >= s.operatingMa() && s.maximumMa() <= s.sourceMa();
}
bool PowerDelivery::matches(const PdReading &s) const {
  return sensible(s) && s.pdo == originalPdo_ && ((s.rdo >> 28) & 7) == originalObject_ && s.operatingMa() >= kRequestedMa;
}
bool PowerDelivery::startBudget() {
  error_[0] = 0; result_ = Result::Failed;
  if (!begun_ || !reading_.valid || millis() - reading_.atMs > 1500 || !sensible(reading_)) return fail("PD_20V_SOURCE_UNVERIFIED");
  originalPdo_ = reading_.pdo; originalObject_ = (reading_.rdo >> 28) & 7;
  wrote_ = !matches(reading_); responseOk_ = !wrote_; stable_ = false;
  stage_ = wrote_ ? 0 : 2; startAt_ = millis(); sampleAt_ = startAt_ - 25;
  result_ = Result::Running; return true;
}
void PowerDelivery::service() {
  if (result_ != Result::Running) return;
  const uint32_t now = millis();
  if (now - startAt_ >= 5000) { fail("PD_BUDGET_TIMEOUT"); result_ = Result::Failed; return; }
  if (stage_ == 0) {
    uint8_t payload[32] = {0x50, 0x4b, 0x4e, 0x53};
    put(payload + 4, (1u << 28) | (100u << 10) | 90u);
    put(payload + 8, (400u << 10) | (kRequestedMa / 10));
    if (!write(0x1800, payload, sizeof(payload))) { result_ = Result::Failed; return; }
    stage_ = 1; return;
  }
  if (stage_ == 1) {
    const uint8_t mask = 3;
    if (!write(0x1005, &mask, 1)) { result_ = Result::Failed; return; }
    stage_ = 2; return;
  }
  if (now - sampleAt_ < 25) return;
  sampleAt_ = now;
  if (!poll(true)) { result_ = Result::Failed; return; }
  if ((reading_.response & 255) == 2) responseOk_ = true;
  if (responseOk_ && matches(reading_)) {
    if (!stable_) { stable_ = true; stableAt_ = millis(); }
    if (millis() - stableAt_ >= 500) result_ = Result::Succeeded;
  } else stable_ = false;
}
