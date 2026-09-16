#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <stdio.h>
#define ARDUINO_ISR_ATTR
#define ARDUINO_USB_MODE 0
#define ARDUINO_USB_CDC_ON_BOOT 0
uint32_t millis();
void delay(uint32_t milliseconds);
