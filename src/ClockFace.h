#pragma once

// Pure geometry for the Dial's analogue clock face (spec 4.8). No Arduino,
// no display library dependency — builds and is tested on the host. The
// renderer (DialUi::drawClock) turns these into drawWideLine()/drawLine()
// calls.

// Endpoints of a hand/tick line segment, in the gfx's own pixel space.
struct HandLine
{
    int x0, y0, x1, y1;
};

namespace ClockFace
{

// Endpoint at radius r from (cx, cy) along angleDeg, where 0 deg = 12
// o'clock (straight up) and angle increases clockwise. Rounded to the
// nearest pixel.
void pointAt(float angleDeg, int cx, int cy, int r, int& x, int& y);

// Hour hand angle: 30 deg per hour (12-hour face) plus 0.5 deg per minute so
// it creeps between hour ticks rather than jumping on the hour.
float hourAngle(int h, int m);

// Minute hand angle: 6 deg per minute plus 0.1 deg per second.
float minuteAngle(int m, int s);

// Second hand angle: 6 deg per second.
float secondAngle(int s);

// A hand from the face centre (cx, cy) out to radius `len` along angleDeg.
HandLine hand(float angleDeg, int cx, int cy, int len);

// One of the 12 hour ticks (i in 0..11, i=0 at 12 o'clock, clockwise),
// radial from r0 to r1.
HandLine tick(int i, int cx, int cy, int r0, int r1);

} // namespace ClockFace
