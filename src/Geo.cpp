#include "Geo.h"

#include <math.h>

namespace Geo
{

namespace
{
constexpr float kEarthRadiusMiles = 3958.8f;

float toRadians(float deg)
{
    return deg * static_cast<float>(M_PI) / 180.0f;
}

float toDegrees(float rad)
{
    return rad * 180.0f / static_cast<float>(M_PI);
}

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

float distanceMiles(float lat1, float lon1, float lat2, float lon2)
{
    const float phi1 = toRadians(lat1);
    const float phi2 = toRadians(lat2);
    const float dPhi = toRadians(lat2 - lat1);
    const float dLambda = toRadians(lon2 - lon1);

    const float sinDPhi = sinf(dPhi / 2.0f);
    const float sinDLambda = sinf(dLambda / 2.0f);

    const float a = sinDPhi * sinDPhi + cosf(phi1) * cosf(phi2) * sinDLambda * sinDLambda;
    const float c = 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));

    return kEarthRadiusMiles * c;
}

float bearingDeg(float lat1, float lon1, float lat2, float lon2)
{
    const float phi1 = toRadians(lat1);
    const float phi2 = toRadians(lat2);
    const float dLambda = toRadians(lon2 - lon1);

    const float y = sinf(dLambda) * cosf(phi2);
    const float x = cosf(phi1) * sinf(phi2) - sinf(phi1) * cosf(phi2) * cosf(dLambda);

    return normaliseDeg(toDegrees(atan2f(y, x)));
}

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
