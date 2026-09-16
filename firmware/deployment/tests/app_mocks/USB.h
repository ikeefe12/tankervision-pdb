#pragma once
class USBMock {
 public:
  void productName(const char *) {}
  void manufacturerName(const char *) {}
  void serialNumber(const char *) {}
  void usbPower(unsigned) {}
  bool begin() { return true; }
};
extern USBMock USB;
