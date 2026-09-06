#pragma once
#include "board.h"
#if HAS_DIAL_UI

#include <M5Dial.h>
#include <stdint.h>

#include "DialTheme.h"
#include "FlightsModel.h"

// The Flights card's face (spec 4.9/4.11) and its airline-logo decode, split
// out of DialUi (Plan 8 Task 3) to keep DialUi.cpp to the input/state work.
// Free functions over the destination and the theme.

// Draws the card for aircraft `flightIdx` of `snap` (offline/empty states
// included). `haveLogo` is the caller's decision that a logo will be pushed
// straight onto the display after the frame: when true, the logo rectangle is
// reserved as the TRANSPARENT palette index instead of being painted over.
void drawFlightsCard(LovyanGFX& gfx, const DialTheme& theme,
                     const FlightsModel::FlightsSnapshot& snap, uint8_t flightIdx,
                     bool haveLogo);

// Reads /logos/<iata>.png off LittleFS, decodes it into a temporary 16-bit
// sprite and pushes it onto the display at the card's logo rectangle. Returns
// false if the file is missing/oversized, the sprite could not be allocated or
// the PNG did not decode. The caller decides when there is enough heap to try
// (the decoder needs ~45 KB of workspace) and what a failure means.
bool dialDrawAirlineLogo(const char* iata);

#endif // HAS_DIAL_UI
