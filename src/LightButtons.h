#pragma once

// Pure geometry + hit-testing for the Lights card's on-screen buttons.
// Header-only, Arduino-free: DialUi draws with geom() and drives
// LightCardState::tapButton() with whatever hitTest() returns for a tap
// position. Coordinates are card-local pixels (240x240 round display).
//
// Lamp (hasColour): POWER / BRIGHT / COLOUR, three buttons.
// Office (!hasColour): POWER / BRIGHT only; COLOUR is never geometry'd or
// hit-tested for it.

enum class Button : uint8_t { NONE, POWER, BRIGHT, COLOUR };

struct Geom
{
    int cx, cy, r;
};

// Button centre + radius for the given layout. Returns a zero geom
// (cx=cy=r=0) for COLOUR when !hasColour, or for NONE.
inline Geom geom(Button b, bool hasColour)
{
    constexpr int kRadius = 22;
    if (hasColour)
    {
        switch (b)
        {
        case Button::POWER:
            return Geom{56, 186, kRadius};
        case Button::BRIGHT:
            return Geom{120, 206, kRadius};
        case Button::COLOUR:
            return Geom{184, 186, kRadius};
        default:
            return Geom{0, 0, 0};
        }
    }

    switch (b)
    {
    case Button::POWER:
        return Geom{84, 200, kRadius};
    case Button::BRIGHT:
        return Geom{156, 200, kRadius};
    default:
        return Geom{0, 0, 0};
    }
}

// Which button (if any) contains (x, y), with a 6px margin added to each
// button's radius. COLOUR is never returned when !hasColour. Returns
// Button::NONE when the point hits no button (or hits more than one only in
// their overlap, in which case POWER/BRIGHT/COLOUR precedence order wins).
inline Button hitTest(int x, int y, bool hasColour)
{
    constexpr int kMargin = 6;
    Button candidates[3] = {Button::POWER, Button::BRIGHT, Button::COLOUR};
    for (Button b : candidates)
    {
        if (b == Button::COLOUR && !hasColour)
        {
            continue;
        }
        Geom g = geom(b, hasColour);
        long dx = x - g.cx;
        long dy = y - g.cy;
        long limit = g.r + kMargin;
        if (dx * dx + dy * dy <= limit * limit)
        {
            return b;
        }
    }
    return Button::NONE;
}

// "Power" / "Bright" / "Colour" / "" (for NONE).
inline const char* label(Button b)
{
    switch (b)
    {
    case Button::POWER:
        return "Power";
    case Button::BRIGHT:
        return "Bright";
    case Button::COLOUR:
        return "Colour";
    default:
        return "";
    }
}
