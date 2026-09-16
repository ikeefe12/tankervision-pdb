#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#define ARDUINO_ISR_ATTR
constexpr int LOW = 0, HIGH = 1, INPUT = 1, CHANGE = 3;
inline int pdMockInterruptLevel = LOW;
inline bool pdMockHandlerAttached = false;
inline uint32_t pdMockMillis = 0;
class Stream {
 public:
  std::string text;
  void println(const char *value = "") { text += value; text += '\n'; }
  void print(const char *value) { text += value; }
  template<typename... Args> void printf(const char *format, Args... args) {
    char buffer[768];
    std::snprintf(buffer, sizeof(buffer), format, args...);
    text += buffer;
  }
};
inline uint32_t millis() { return pdMockMillis; }
inline void delay(uint32_t duration) { pdMockMillis += duration; }
inline void pinMode(int, int) {}
inline int digitalRead(int) { return pdMockInterruptLevel; }
inline void attachInterruptArg(int, void (*)(void *), void *, int) { pdMockHandlerAttached = true; }
inline void detachInterrupt(int) { pdMockHandlerAttached = false; }
