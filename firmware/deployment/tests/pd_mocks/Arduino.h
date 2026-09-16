#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
extern uint32_t fakeMs;
inline uint32_t millis() { return fakeMs; }
