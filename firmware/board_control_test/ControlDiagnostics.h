#pragma once

#include "BoardControl.h"

// Tests 1/2/3/6: unloaded backup switching, expander IRQs, PD HPI, status LED.
// Starts and finishes with controlled outputs off. No supercap/load required.
void runControlDiagnostics(BoardControl &board, Stream &log);
