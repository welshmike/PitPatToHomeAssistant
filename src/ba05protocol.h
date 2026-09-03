#pragma once
#include <stddef.h>
#include "TreadmillData.h"

namespace BA05Protocol {

    struct ParsedData {
        bool valid = false;
        // Raw notification length. Kept so consumers can tell a full (>=50 byte)
        // frame from a 20-byte status frame without re-inspecting the bytes —
        // packetType is read from pData[3] in both cases and is meaningless for
        // the short form, so it cannot be used as the discriminator.
        size_t length = 0;
        uint8_t packetType = 0;
        float speedFeedback = 0.0f;
        float speedCmd = 0.0f;
        float speedMax = 0.0f;
        uint16_t distanceM = 0;
        uint8_t calories = 0;
        uint32_t durationSec = 0;
        TreadMillData::Status status = TreadMillData::STOPPED;
        // Raw status/flags byte the status was derived from (pData[45] on 0x2F,
        // pData[28] on 0x34). Diagnostic only — logged on status transitions.
        uint8_t statusFlags = 0;
    };

    void makePacket(uint16_t speed, uint8_t cmd1, uint8_t mode, uint8_t seqCounter, uint8_t *outPacket);
    void makeKeepalive(uint8_t seqCounter, uint8_t *outPacket);
    ParsedData parsePacket(const uint8_t *pData, size_t length);

}
