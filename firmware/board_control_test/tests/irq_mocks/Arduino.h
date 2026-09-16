#pragma once
#include <cstddef>
#include <cstdint>

#define ARDUINO_ISR_ATTR
constexpr int LOW = 0, HIGH = 1, INPUT = 1, OUTPUT = 3, ADC_11db = 3, CHANGE = 4;
class Stream {
 public:
  void println(const char *) {}
};
uint32_t micros();
void pinMode(int, int);
void attachInterruptArg(int, void (*)(void *), void *, int);
void detachInterrupt(int);
