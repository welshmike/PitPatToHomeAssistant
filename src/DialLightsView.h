#pragma once
#include "board.h"
#if HAS_DIAL_UI

#include <M5Dial.h>
#include <stdint.h>

#include "DialTheme.h"
#include "LightCardState.h"

// The two Lights cards' faces (spec 4.12 "Lights v2"), drawn from
// LightCardState's view/page and LightLayout's geometry — so what is painted
// and what DialUi hit-tests are the same numbers. Free functions over the
// destination and the theme: no DialUi dependency.
//
// Faces, in the order drawLightCard() decides between them:
//   MQTT down        "waiting for HA", nothing else (no pages, no power circle)
//   no light state   "no data", likewise
//   light off        the r 44 power circle and "tap to switch on"
//   light on         page dots, the showing page, and the small power glyph
void drawLightCard(LovyanGFX& gfx, const DialTheme& theme, const LightCardState& card,
                   const char* name, bool mqttUp);

// Whether the Colour page's true-colour circles (the hue ring, its marker and
// the centre disc) are on screen for this card. Single source of truth for the
// two halves of that trick: drawLightCard() reserves exactly these circles as
// the TRANSPARENT palette index, and DialUi calls paintLightTrueColour() after
// the canvas push to fill the same circles on the display itself — a palette
// canvas has 16 colours and no room for a continuous hue ring.
bool lightTrueColourVisible(const LightCardState& card, bool mqttUp);

// Paints the hue ring (72 five-degree segments between LightLayout's inner and
// outer radii), the centre disc (kCentreDiscR) and the marker (kHueMarkerR
// plus its outline — white, amber while settling) in true colour straight
// onto the display. Only call it when lightTrueColourVisible() agrees, and
// only after the frame has been pushed.
void paintLightTrueColour(LGFX_Device& display, const LightCardState& card);

#endif // HAS_DIAL_UI
