#include "ba05protocol.h"

namespace BA05Protocol {

    // Helper functions to read big-endian types
    static uint16_t readU16(const uint8_t *data, int offset)
    {
        return ((uint16_t)data[offset] << 8) | data[offset + 1];
    }

    void makePacket(uint16_t speed, uint8_t cmd1, uint8_t mode, uint8_t seqCounter, uint8_t *outPacket)
    {
        // Header
        outPacket[0] = 0x4D;  // Start byte (BA05)
        outPacket[1] = 0x00;
        outPacket[2] = seqCounter;
        outPacket[3] = 0x17;  // Payload length
        outPacket[4] = 0x6A;
        outPacket[5] = 0x17;

        // Reserved
        outPacket[6] = 0x00;
        outPacket[7] = 0x00;
        outPacket[8] = 0x00;
        outPacket[9] = 0x00;

        // Speed
        outPacket[10] = (speed >> 8) & 0xFF;
        outPacket[11] = speed & 0xFF;

        // Command byte 1
        outPacket[12] = cmd1;

        outPacket[13] = 0x00;
        outPacket[14] = 0x50;
        outPacket[15] = 0x00;

        // Mode byte
        outPacket[16] = mode;

        // Reserved bytes
        for (int i = 17; i <= 24; i++) {
            outPacket[i] = 0x00;
        }

        // Checksum
        uint8_t checksum = 0;
        for (int i = 5; i <= 24; ++i)
        {
            checksum ^= outPacket[i];
        }
        outPacket[25] = checksum;

        // End byte
        outPacket[26] = 0x43;
    }

    void makeKeepalive(uint8_t seqCounter, uint8_t *outPacket)
    {
        outPacket[0] = 0x4D;
        outPacket[1] = 0x00;
        outPacket[2] = seqCounter;
        outPacket[3] = 0x05;
        outPacket[4] = 0x6A;
        outPacket[5] = 0x05;
        outPacket[6] = 0xFD;
        outPacket[7] = 0xF8;
        outPacket[8] = 0x43;
    }

    ParsedData parsePacket(const uint8_t *pData, size_t length)
    {
        ParsedData parsed;
        parsed.length = length;
        if (length < 20) {
            return parsed; // invalid
        }

        uint8_t packetType = pData[3];
        parsed.packetType = packetType;

        if (length >= 50 && (packetType == 0x2F || packetType == 0x34)) {
            uint16_t current_speed = readU16(pData, 7);
            uint16_t target_speed  = readU16(pData, 9);
            uint16_t distance_m    = readU16(pData, 13);
            uint8_t  calories      = pData[23];
            uint8_t  duration_min  = pData[25];
            uint16_t duration_ms   = readU16(pData, 26);

            uint32_t duration_total_sec = ((uint32_t)duration_min * 65536 + duration_ms) / 1000;

            parsed.speedFeedback = current_speed / (float)SPEED_RAW_PER_MPH;
            parsed.speedCmd      = target_speed  / (float)SPEED_RAW_PER_MPH;
            parsed.speedMax      = readU16(pData, 11) / (float)SPEED_RAW_PER_MPH;
            parsed.distanceM     = distance_m;
            parsed.calories      = calories;
            parsed.durationSec   = duration_total_sec;

            uint8_t flags = (packetType == 0x2F) ? pData[45] : pData[28];
            parsed.statusFlags = flags;
            if      (flags == 0x00) parsed.status = TreadMillData::STOPPED;
            else if (flags == 0x18) parsed.status = TreadMillData::COUNTDOWN;
            else if (flags == 0x10) parsed.status = TreadMillData::PAUSED;
            else                    parsed.status = TreadMillData::RUNNING;

            if (packetType == 0x34 && parsed.status == TreadMillData::RUNNING && current_speed == 0)
            {
                parsed.status = TreadMillData::STOPPED;
            }

            parsed.valid = true;
        } 
        else if (length == 20) {
            uint16_t current_speed = readU16(pData, 9);
            parsed.speedFeedback = current_speed / (float)SPEED_RAW_PER_MPH;
            parsed.valid = true;
        }
        
        return parsed;
    }
}
