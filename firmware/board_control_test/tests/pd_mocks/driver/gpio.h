#pragma once
#include <Arduino.h>
constexpr int GPIO_NUM_39 = 39;
inline int gpio_get_level(int pin) { return digitalRead(pin); }
