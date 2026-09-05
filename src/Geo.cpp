#include "Geo.h"

#include <math.h>

namespace Geo
{

namespace
{
// Normalises a degree value to [0, 360).
float normaliseDeg(float deg)
{
    float d = fmodf(deg, 360.0f);
    if (d < 0.0f)
    {
        d += 360.0f;
    }
    return d;
}
} // namespace

const char* compass8(float deg)
{
    static const char* kPoints[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    const float d = normaliseDeg(deg);
    // Shift by half a sector so each point's sector is centred on it, then
    // pick which 45deg wedge it falls in.
    const int index = static_cast<int>(floorf(normaliseDeg(d + 22.5f) / 45.0f)) & 7;
    return kPoints[index];
}

} // namespace Geo
