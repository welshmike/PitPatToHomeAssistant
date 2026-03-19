#pragma once
#include <Arduino.h>
#include "platform.h"

namespace BA05Protocol {

    struct ParsedData {
        bool valid = false;
        uint8_t packetType = 0;
        float speedFeedback = 0.0f;
        float speedCmd = 0.0f;
        float speedMax = 0.0f;
        uint16_t distanceM = 0;
        uint8_t calories = 0;
        uint32_t durationSec = 0;
        TreadMillData::Status status = TreadMillData::STOPPED;
    };

    void makePacket(uint16_t speed, uint8_t cmd1, uint8_t mode, uint8_t seqCounter, uint8_t *outPacket);
    void makeKeepalive(uint8_t seqCounter, uint8_t *outPacket);
    ParsedData parsePacket(const uint8_t *pData, size_t length);

}
