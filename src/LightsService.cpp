#include "LightsService.h"

#if HAS_DIAL_UI

#include <Arduino.h>
#include <stdio.h>

namespace
{
constexpr const char kStateTopicFmt[] = "pacekeeper-dial/light/%s/state";
constexpr const char kSetTopicFmt[]   = "pacekeeper-dial/light/%s/set";
} // namespace

constexpr const char *LightsService::kRefreshTopic;

void LightsService::stateTopic(LightsModel::LightKey key, char *buf, size_t cap)
{
    snprintf(buf, cap, kStateTopicFmt, LightsModel::keyName(key));
}

void LightsService::setTopic(LightsModel::LightKey key, char *buf, size_t cap)
{
    snprintf(buf, cap, kSetTopicFmt, LightsModel::keyName(key));
}

bool LightsService::isStateTopic(const char *topic)
{
    LightsModel::LightKey key;
    return LightsModel::keyFromTopic(topic, key);
}

void LightsService::onStateMessage(const char *topic, const uint8_t *payload, size_t len)
{
    LightsModel::LightKey key;
    if (!LightsModel::keyFromTopic(topic, key))
    {
        log_w("LightsService: state message on unmatched topic '%s'", topic);
        return;
    }

    LightsModel::LightState state;
    if (!LightsModel::parseLightState(reinterpret_cast<const char *>(payload), len, state))
    {
        log_w("LightsService: %s state payload failed to parse (%u bytes)", LightsModel::keyName(key),
              (unsigned)len);
        return;
    }

    m_snapshot.modify([&](LightsModel::LightsSnapshot &s) {
        if (key == LightsModel::LightKey::OFFICE)
        {
            s.office = state;
        }
        else
        {
            s.lamp = state;
        }
    });

    log_i("LightsService: %s state=%s brightness=%u%%", LightsModel::keyName(key),
          !state.available ? "unavailable" : (state.on ? "on" : "off"), (unsigned)state.brightnessPct);
}

#endif // HAS_DIAL_UI
