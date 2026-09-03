#include "ClockFace.h"

#include <math.h>

namespace ClockFace
{

void pointAt(float angleDeg, int cx, int cy, int r, int& x, int& y)
{
    const float rad = angleDeg * (float)M_PI / 180.0f;
    x = cx + (int)lroundf((float)r * sinf(rad));
    y = cy - (int)lroundf((float)r * cosf(rad));
}

float hourAngle(int h, int m)
{
    return (float)(h % 12) * 30.0f + (float)m * 0.5f;
}

float minuteAngle(int m, int s)
{
    return (float)m * 6.0f + (float)s * 0.1f;
}

float secondAngle(int s)
{
    return (float)s * 6.0f;
}

HandLine hand(float angleDeg, int cx, int cy, int len)
{
    HandLine line;
    line.x0 = cx;
    line.y0 = cy;
    pointAt(angleDeg, cx, cy, len, line.x1, line.y1);
    return line;
}

HandLine tick(int i, int cx, int cy, int r0, int r1)
{
    const float angleDeg = (float)i * 30.0f;
    HandLine line;
    pointAt(angleDeg, cx, cy, r0, line.x0, line.y0);
    pointAt(angleDeg, cx, cy, r1, line.x1, line.y1);
    return line;
}

} // namespace ClockFace
