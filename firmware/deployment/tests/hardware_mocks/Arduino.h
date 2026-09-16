#pragma once
#include <cstddef>
#include <cstdint>
#define ARDUINO_ISR_ATTR
constexpr int LOW = 0, HIGH = 1, INPUT = 1, INPUT_PULLUP = 5, OUTPUT = 3, CHANGE = 3, ADC_11db = 3;
uint32_t millis();
void digitalWrite(int, int);
int digitalRead(int);
void pinMode(int, int);
void analogReadResolution(int);
void analogSetPinAttenuation(int, int);
uint16_t analogRead(int);
uint32_t analogReadMilliVolts(int);
void attachInterruptArg(int, void (*)(void *), void *, int);
