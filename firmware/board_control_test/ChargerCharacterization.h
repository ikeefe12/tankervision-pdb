#pragma once
#include "BoardControl.h"

// Bounded bench characterization with CN1 disconnected. Always requests cleanup.
// Does not arm backup or external outputs; temperature selections change CE-low.
void runChargerCharacterization(BoardControl &board, Stream &log);
