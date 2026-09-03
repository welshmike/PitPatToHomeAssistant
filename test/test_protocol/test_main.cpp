#include <unity.h>
#include <string.h>
#include "ba05protocol.h"
#include "TreadmillData.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// makeKeepalive
// ---------------------------------------------------------------------------

static void test_makeKeepalive_producesExpectedBytes(void)
{
    uint8_t out[9] = {0};
    BA05Protocol::makeKeepalive(0x07, out);

    const uint8_t expected[9] = {0x4D, 0x00, 0x07, 0x05, 0x6A, 0x05, 0xFD, 0xF8, 0x43};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, 9);
}

// ---------------------------------------------------------------------------
// makePacket
// ---------------------------------------------------------------------------

static void test_makePacket_producesExpectedBytes(void)
{
    uint8_t out[27] = {0};
    BA05Protocol::makePacket(994, 0x01, 0x0C, 7, out);

    // Header: start/seq/length/reserved
    const uint8_t expectedHeader[6] = {0x4D, 0x00, 0x07, 0x17, 0x6A, 0x17};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expectedHeader, out, 6);

    // Speed bytes (994 = 0x03E2), big-endian at [10..11]
    TEST_ASSERT_EQUAL_UINT8(0x03, out[10]);
    TEST_ASSERT_EQUAL_UINT8(0xE2, out[11]);

    // Command byte 1
    TEST_ASSERT_EQUAL_UINT8(0x01, out[12]);

    // Mode byte
    TEST_ASSERT_EQUAL_UINT8(0x0C, out[16]);

    // End byte
    TEST_ASSERT_EQUAL_UINT8(0x43, out[26]);

    // Checksum: byte 25 is the XOR of bytes 5..24
    uint8_t checksum = 0;
    for (int i = 5; i <= 24; ++i) {
        checksum ^= out[i];
    }
    TEST_ASSERT_EQUAL_UINT8(checksum, out[25]);
    // Concrete value, computed independently for this speed/cmd/mode combo.
    TEST_ASSERT_EQUAL_UINT8(0xAB, out[25]);
}

// ---------------------------------------------------------------------------
// parsePacket: recorded 0x34 notification (treadmill_log.txt line 345)
// ---------------------------------------------------------------------------
// Note: the brief describes this as a "57-byte" packet, but the recorded log
// entry ("Notification received, length: 56") and the hex dump below are
// both 56 bytes. Using the actual recorded bytes.

static const uint8_t kRecorded0x34Packet[56] = {
    0x4D, 0x04, 0x09, 0x34, 0x68, 0x34, 0x00, 0x12,
    0xC0, 0x12, 0xC0, 0x00, 0x00, 0x00, 0x83, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09,
    0x00, 0x01, 0x9E, 0x10, 0x7A, 0x1B, 0x8B, 0x17,
    0x70, 0x00, 0x05, 0x00, 0x74, 0x73, 0x49, 0x37,
    0x31, 0x36, 0x6E, 0x58, 0x50, 0x75, 0x57, 0x71,
    0x42, 0x36, 0x32, 0x37, 0x40, 0x00, 0xC3, 0x43,
};

static void test_parsePacket_recorded0x34Packet(void)
{
    BA05Protocol::ParsedData parsed =
        BA05Protocol::parsePacket(kRecorded0x34Packet, sizeof(kRecorded0x34Packet));

    TEST_ASSERT_TRUE(parsed.valid);
    TEST_ASSERT_EQUAL_UINT8(0x34, parsed.packetType);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.0f, parsed.speedFeedback);
    TEST_ASSERT_EQUAL_UINT16(0x0083, parsed.distanceM);

    // Flags byte for 0x34 packets is at offset 28: 0x7A here, which is none
    // of the recognised special values (0x00/0x18/0x10), so the parser's
    // default branch reports RUNNING. Since current speed (0x12C0) != 0,
    // the 0x34-specific "downgrade to STOPPED" rule does not apply, and the
    // status stays RUNNING.
    TEST_ASSERT_EQUAL(TreadMillData::RUNNING, parsed.status);
}

// ---------------------------------------------------------------------------
// parsePacket: synthetic 50-byte 0x2F packets, status-flag byte at [45]
// ---------------------------------------------------------------------------

static void buildSynthetic0x2FPacket(uint8_t *pkt, size_t len, uint8_t statusFlags)
{
    memset(pkt, 0, len);
    pkt[3] = 0x2F;
    // Non-zero speed so a RUNNING result (were it produced) would be
    // unambiguous; irrelevant to the STOPPED-downgrade rule, which only
    // applies to packet type 0x34.
    pkt[7] = 0x06;
    pkt[8] = 0x40; // current speed = 0x0640 = 1600 -> 1.0 mph
    pkt[45] = statusFlags;
}

static void test_parsePacket_synthetic0x2F_countdownFlag(void)
{
    uint8_t pkt[50];
    buildSynthetic0x2FPacket(pkt, sizeof(pkt), 0x18);

    BA05Protocol::ParsedData parsed = BA05Protocol::parsePacket(pkt, sizeof(pkt));

    TEST_ASSERT_TRUE(parsed.valid);
    TEST_ASSERT_EQUAL(TreadMillData::COUNTDOWN, parsed.status);
}

static void test_parsePacket_synthetic0x2F_pausedFlag(void)
{
    uint8_t pkt[50];
    buildSynthetic0x2FPacket(pkt, sizeof(pkt), 0x10);

    BA05Protocol::ParsedData parsed = BA05Protocol::parsePacket(pkt, sizeof(pkt));

    TEST_ASSERT_TRUE(parsed.valid);
    TEST_ASSERT_EQUAL(TreadMillData::PAUSED, parsed.status);
}

static void test_parsePacket_synthetic0x2F_stoppedFlag(void)
{
    uint8_t pkt[50];
    buildSynthetic0x2FPacket(pkt, sizeof(pkt), 0x00);

    BA05Protocol::ParsedData parsed = BA05Protocol::parsePacket(pkt, sizeof(pkt));

    TEST_ASSERT_TRUE(parsed.valid);
    TEST_ASSERT_EQUAL(TreadMillData::STOPPED, parsed.status);
}

// ---------------------------------------------------------------------------
// parsePacket: short packets
// ---------------------------------------------------------------------------

static void test_parsePacket_20BytePacket_parsesSpeedAndIsValid(void)
{
    uint8_t pkt[20] = {0};
    pkt[3] = 0x99; // packet type irrelevant to the 20-byte branch
    pkt[9] = 0x03;
    pkt[10] = 0x20; // current speed = 0x0320 = 800 -> 0.5 mph

    BA05Protocol::ParsedData parsed = BA05Protocol::parsePacket(pkt, sizeof(pkt));

    TEST_ASSERT_TRUE(parsed.valid);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, parsed.speedFeedback);
}

static void test_parsePacket_10BytePacket_isInvalid(void)
{
    uint8_t pkt[10] = {0};
    pkt[3] = 0x34;

    BA05Protocol::ParsedData parsed = BA05Protocol::parsePacket(pkt, sizeof(pkt));

    TEST_ASSERT_FALSE(parsed.valid);
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();

    RUN_TEST(test_makeKeepalive_producesExpectedBytes);
    RUN_TEST(test_makePacket_producesExpectedBytes);
    RUN_TEST(test_parsePacket_recorded0x34Packet);
    RUN_TEST(test_parsePacket_synthetic0x2F_countdownFlag);
    RUN_TEST(test_parsePacket_synthetic0x2F_pausedFlag);
    RUN_TEST(test_parsePacket_synthetic0x2F_stoppedFlag);
    RUN_TEST(test_parsePacket_20BytePacket_parsesSpeedAndIsValid);
    RUN_TEST(test_parsePacket_10BytePacket_isInvalid);

    return UNITY_END();
}
