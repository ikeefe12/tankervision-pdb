#pragma once
#include <cstddef>
#include <cstdint>

constexpr int LOW = 0, HIGH = 1, INPUT = 1, OUTPUT = 3, ADC_11db = 3;
class Stream {
 public:
  void println(const char *) {}
};
uint32_t millis();
void delay(uint32_t);
void digitalWrite(int, int);
int digitalRead(int);
void pinMode(int, int);
void analogReadResolution(int);
void analogSetPinAttenuation(int, int);
uint32_t analogReadMilliVolts(int);
