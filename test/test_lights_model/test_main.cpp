#include <unity.h>
#include <string.h>
#include "LightsModel.h"
#include "../fixtures/fixtures.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// parseLightState: real HA payloads recorded 2026-09-04
// ---------------------------------------------------------------------------

static void test_parseLightState_officeFixture_offNoColor(void)
{
    LightsModel::LightState s;

    bool ok = LightsModel::parseLightState(kFixtureHaLightOffice, strlen(kFixtureHaLightOffice), s);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(s.valid);
    TEST_ASSERT_TRUE(s.available);
    TEST_ASSERT_FALSE(s.on);
    TEST_ASSERT_EQUAL_UINT8(0, s.brightnessPct);
    TEST_ASSERT_EQUAL_UINT16(2202, s.minKelvin);
    TEST_ASSERT_EQUAL_UINT16(4000, s.maxKelvin);
    TEST_ASSERT_FALSE(s.supportsColor);
    TEST_ASSERT_TRUE(LightsModel::ColorMode::NONE == s.mode);
}

static void test_parseLightState_lampFixture_supportsColorWideRange(void)
{
    LightsModel::LightState s;

    bool ok = LightsModel::parseLightState(kFixtureHaLightLamp, strlen(kFixtureHaLightLamp), s);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(s.valid);
    TEST_ASSERT_TRUE(s.supportsColor);
    TEST_ASSERT_EQUAL_UINT16(2000, s.minKelvin);
    TEST_ASSERT_EQUAL_UINT16(6535, s.maxKelvin);
}

// ---------------------------------------------------------------------------
// parseLightState: on / color_temp
// ---------------------------------------------------------------------------

static void test_parseLightState_onColorTemp_modeTempAndKelvin(void)
{
    static const char* kJson =
        "{\"state\":\"on\",\"brightness_pct\":65,\"color_mode\":\"color_temp\","
        "\"color_temp_kelvin\":2700,\"hs_color\":null,\"min_kelvin\":2000,\"max_kelvin\":6535,"
        "\"supports_color\":true}";

    LightsModel::LightState s;
    bool ok = LightsModel::parseLightState(kJson, strlen(kJson), s);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(s.valid);
    TEST_ASSERT_TRUE(s.available);
    TEST_ASSERT_TRUE(s.on);
    TEST_ASSERT_EQUAL_UINT8(65, s.brightnessPct);
    TEST_ASSERT_TRUE(LightsModel::ColorMode::TEMP == s.mode);
    TEST_ASSERT_EQUAL_UINT16(2700, s.kelvin);
}

// ---------------------------------------------------------------------------
// parseLightState: on / hs
// ---------------------------------------------------------------------------

static void test_parseLightState_onHs_modeHsAndHueSat(void)
{
    static const char* kJson =
        "{\"state\":\"on\",\"brightness_pct\":80,\"color_mode\":\"hs\","
        "\"color_temp_kelvin\":null,\"hs_color\":[310.5,72.0],\"min_kelvin\":2000,\"max_kelvin\":6535,"
        "\"supports_color\":true}";

    LightsModel::LightState s;
    bool ok = LightsModel::parseLightState(kJson, strlen(kJson), s);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(s.on);
    TEST_ASSERT_TRUE(LightsModel::ColorMode::HS == s.mode);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 310.5f, s.hue);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 72.0f, s.sat);
}

static void test_parseLightState_xyColorMode_mapsToHs(void)
{
    static const char* kJson =
        "{\"state\":\"on\",\"brightness_pct\":50,\"color_mode\":\"xy\","
        "\"color_temp_kelvin\":null,\"hs_color\":[120.0,55.0],\"min_kelvin\":2000,\"max_kelvin\":6535,"
        "\"supports_color\":true}";

    LightsModel::LightState s;
    bool ok = LightsModel::parseLightState(kJson, strlen(kJson), s);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(LightsModel::ColorMode::HS == s.mode);
}

static void test_parseLightState_rgbColorMode_mapsToHs(void)
{
    static const char* kJson =
        "{\"state\":\"on\",\"brightness_pct\":50,\"color_mode\":\"rgbww\","
        "\"color_temp_kelvin\":null,\"hs_color\":[45.0,90.0],\"min_kelvin\":2000,\"max_kelvin\":6535,"
        "\"supports_color\":true}";

    LightsModel::LightState s;
    bool ok = LightsModel::parseLightState(kJson, strlen(kJson), s);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(LightsModel::ColorMode::HS == s.mode);
}

// ---------------------------------------------------------------------------
// parseLightState: availability
// ---------------------------------------------------------------------------

static void test_parseLightState_unavailable_notAvailableButValid(void)
{
    static const char* kJson = "{\"state\":\"unavailable\"}";

    LightsModel::LightState s;
    bool ok = LightsModel::parseLightState(kJson, strlen(kJson), s);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(s.valid);
    TEST_ASSERT_FALSE(s.available);
}

static void test_parseLightState_unknown_notAvailableButValid(void)
{
    static const char* kJson = "{\"state\":\"unknown\"}";

    LightsModel::LightState s;
    bool ok = LightsModel::parseLightState(kJson, strlen(kJson), s);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(s.valid);
    TEST_ASSERT_FALSE(s.available);
}

static void test_parseLightState_missingState_notAvailableButValid(void)
{
    static const char* kJson = "{}";

    LightsModel::LightState s;
    bool ok = LightsModel::parseLightState(kJson, strlen(kJson), s);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(s.valid);
    TEST_ASSERT_FALSE(s.available);
}

// ---------------------------------------------------------------------------
// parseLightState: clamping, defaults, malformed
// ---------------------------------------------------------------------------

static void test_parseLightState_brightnessOver100_clamped(void)
{
    static const char* kJson = "{\"state\":\"on\",\"brightness_pct\":150}";

    LightsModel::LightState s;
    bool ok = LightsModel::parseLightState(kJson, strlen(kJson), s);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT8(100, s.brightnessPct);
}

static void test_parseLightState_missingMinMaxKelvin_keepsDefaults(void)
{
    static const char* kJson = "{\"state\":\"on\"}";

    LightsModel::LightState s;
    bool ok = LightsModel::parseLightState(kJson, strlen(kJson), s);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT16(2000, s.minKelvin);
    TEST_ASSERT_EQUAL_UINT16(6500, s.maxKelvin);
}

static void test_parseLightState_malformedJson_returnsFalse(void)
{
    static const char* kJson = "{not json";

    LightsModel::LightState s;
    bool ok = LightsModel::parseLightState(kJson, strlen(kJson), s);

    TEST_ASSERT_FALSE(ok);
}

// ---------------------------------------------------------------------------
// keyFromTopic / keyName
// ---------------------------------------------------------------------------

static void test_keyName_officeAndLamp(void)
{
    TEST_ASSERT_EQUAL_STRING("office", LightsModel::keyName(LightsModel::LightKey::OFFICE));
    TEST_ASSERT_EQUAL_STRING("lamp", LightsModel::keyName(LightsModel::LightKey::LAMP));
}

static void test_keyFromTopic_officeTopic_matches(void)
{
    LightsModel::LightKey k;
    bool ok = LightsModel::keyFromTopic("pacekeeper-dial/light/office/state", k);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(LightsModel::LightKey::OFFICE == k);
}

static void test_keyFromTopic_lampTopic_matches(void)
{
    LightsModel::LightKey k;
    bool ok = LightsModel::keyFromTopic("pacekeeper-dial/light/lamp/state", k);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(LightsModel::LightKey::LAMP == k);
}

static void test_keyFromTopic_nonMatchingTopic_returnsFalse(void)
{
    LightsModel::LightKey k;
    bool ok = LightsModel::keyFromTopic("pacekeeper-dial/light/kitchen/state", k);

    TEST_ASSERT_FALSE(ok);
}

// ---------------------------------------------------------------------------
// formatCommand
// ---------------------------------------------------------------------------

static void test_formatCommand_powerOn_byteExact(void)
{
    LightsModel::Command c;
    c.type = LightsModel::Command::Type::POWER;
    c.on = true;

    char buf[64];
    size_t n = LightsModel::formatCommand(c, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_STRING("{\"state\":\"ON\"}", buf);
    TEST_ASSERT_EQUAL_UINT(strlen(buf), n);
}

static void test_formatCommand_powerOff_byteExact(void)
{
    LightsModel::Command c;
    c.type = LightsModel::Command::Type::POWER;
    c.on = false;

    char buf[64];
    size_t n = LightsModel::formatCommand(c, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_STRING("{\"state\":\"OFF\"}", buf);
    TEST_ASSERT_EQUAL_UINT(strlen(buf), n);
}

static void test_formatCommand_bright_byteExact(void)
{
    LightsModel::Command c;
    c.type = LightsModel::Command::Type::BRIGHT;
    c.pct = 42;

    char buf[64];
    size_t n = LightsModel::formatCommand(c, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_STRING("{\"state\":\"ON\",\"brightness_pct\":42}", buf);
    TEST_ASSERT_EQUAL_UINT(strlen(buf), n);
}

static void test_formatCommand_temp_byteExact(void)
{
    LightsModel::Command c;
    c.type = LightsModel::Command::Type::TEMP;
    c.kelvin = 2700;

    char buf[64];
    size_t n = LightsModel::formatCommand(c, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_STRING("{\"state\":\"ON\",\"color_temp_kelvin\":2700}", buf);
    TEST_ASSERT_EQUAL_UINT(strlen(buf), n);
}

static void test_formatCommand_hue_byteExact_roundedIntegers(void)
{
    LightsModel::Command c;
    c.type = LightsModel::Command::Type::HUE;
    c.hue = 310.6f;
    c.sat = 71.5f;

    char buf[64];
    size_t n = LightsModel::formatCommand(c, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_STRING("{\"state\":\"ON\",\"hs_color\":[311,72]}", buf);
    TEST_ASSERT_EQUAL_UINT(strlen(buf), n);
}

static void test_formatCommand_none_returnsZeroAndUntouchedIsFine(void)
{
    LightsModel::Command c; // default Type::NONE

    char buf[64];
    buf[0] = '\1'; // sentinel
    size_t n = LightsModel::formatCommand(c, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT(0, n);
    // Nothing written: the caller's buffer is left exactly as it was.
    TEST_ASSERT_EQUAL_HEX8('\1', buf[0]);
}

static void test_formatCommand_overflow_returnsZero(void)
{
    LightsModel::Command c;
    c.type = LightsModel::Command::Type::BRIGHT;
    c.pct = 42;

    char buf[8]; // too small for {"state":"ON","brightness_pct":42}
    size_t n = LightsModel::formatCommand(c, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT(0, n);
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();

    RUN_TEST(test_parseLightState_officeFixture_offNoColor);
    RUN_TEST(test_parseLightState_lampFixture_supportsColorWideRange);
    RUN_TEST(test_parseLightState_onColorTemp_modeTempAndKelvin);
    RUN_TEST(test_parseLightState_onHs_modeHsAndHueSat);
    RUN_TEST(test_parseLightState_xyColorMode_mapsToHs);
    RUN_TEST(test_parseLightState_rgbColorMode_mapsToHs);
    RUN_TEST(test_parseLightState_unavailable_notAvailableButValid);
    RUN_TEST(test_parseLightState_unknown_notAvailableButValid);
    RUN_TEST(test_parseLightState_missingState_notAvailableButValid);
    RUN_TEST(test_parseLightState_brightnessOver100_clamped);
    RUN_TEST(test_parseLightState_missingMinMaxKelvin_keepsDefaults);
    RUN_TEST(test_parseLightState_malformedJson_returnsFalse);
    RUN_TEST(test_keyName_officeAndLamp);
    RUN_TEST(test_keyFromTopic_officeTopic_matches);
    RUN_TEST(test_keyFromTopic_lampTopic_matches);
    RUN_TEST(test_keyFromTopic_nonMatchingTopic_returnsFalse);
    RUN_TEST(test_formatCommand_powerOn_byteExact);
    RUN_TEST(test_formatCommand_powerOff_byteExact);
    RUN_TEST(test_formatCommand_bright_byteExact);
    RUN_TEST(test_formatCommand_temp_byteExact);
    RUN_TEST(test_formatCommand_hue_byteExact_roundedIntegers);
    RUN_TEST(test_formatCommand_none_returnsZeroAndUntouchedIsFine);
    RUN_TEST(test_formatCommand_overflow_returnsZero);

    return UNITY_END();
}
