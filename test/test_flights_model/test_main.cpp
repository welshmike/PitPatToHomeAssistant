#include <unity.h>
#include <string.h>
#include "FlightsModel.h"
#include "../fixtures/fixtures.h"

void setUp(void) {}
void tearDown(void) {}

// kFixtureDialFlights (test/fixtures/dial_flights.json): a hand-written HA
// payload matching the spec 4.11 contract, 3 aircraft in payload order:
//   [0] BAW123 - full route/airline/operator known, airborne
//   [1] RYR45D - empty fr/to/al/an (route/airline not yet known to HA)
//   [2] GABCD  - on ground (gnd:1), no flight number

static void test_parseDialFlights_fixture_parsesThreeInPayloadOrder(void)
{
    FlightsModel::FlightsSnapshot snap;
    memset(&snap, 0, sizeof(snap));

    bool ok = FlightsModel::parseDialFlights(kFixtureDialFlights, strlen(kFixtureDialFlights), snap);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT8(3, snap.count);
    TEST_ASSERT_EQUAL_STRING("BAW123", snap.ac[0].callsign);
    TEST_ASSERT_EQUAL_STRING("RYR45D", snap.ac[1].callsign);
    TEST_ASSERT_EQUAL_STRING("GABCD", snap.ac[2].callsign);
}

static void test_parseDialFlights_fixture_firstEntryAllFields(void)
{
    FlightsModel::FlightsSnapshot snap;
    memset(&snap, 0, sizeof(snap));

    bool ok = FlightsModel::parseDialFlights(kFixtureDialFlights, strlen(kFixtureDialFlights), snap);

    TEST_ASSERT_TRUE(ok);
    const FlightsModel::Aircraft& a = snap.ac[0];
    TEST_ASSERT_EQUAL_STRING("BAW123", a.callsign);
    TEST_ASSERT_EQUAL_STRING("BA123", a.flightNumber);
    TEST_ASSERT_EQUAL_STRING("A320", a.type);
    TEST_ASSERT_EQUAL_STRING("BA", a.airlineIata);
    TEST_ASSERT_EQUAL_STRING("British Airways", a.operatorName);
    TEST_ASSERT_EQUAL_STRING("LHR", a.fromIata);
    TEST_ASSERT_EQUAL_STRING("JFK", a.toIata);
    TEST_ASSERT_EQUAL_INT(12000, a.altFt);
    TEST_ASSERT_EQUAL_INT(450, a.gsKt);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.3f, a.distMi);
    TEST_ASSERT_EQUAL_INT(135, a.bearing);
    TEST_ASSERT_FALSE(a.onGround);
    TEST_ASSERT_TRUE(a.routeKnown);
    TEST_ASSERT_TRUE(a.operatorKnown);
    TEST_ASSERT_EQUAL_STRING("", a.hex);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, a.lat);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, a.lon);
    TEST_ASSERT_EQUAL_INT(0, a.track);
}

static void test_parseDialFlights_fixture_emptyStringsPreserved(void)
{
    FlightsModel::FlightsSnapshot snap;
    memset(&snap, 0, sizeof(snap));

    bool ok = FlightsModel::parseDialFlights(kFixtureDialFlights, strlen(kFixtureDialFlights), snap);

    TEST_ASSERT_TRUE(ok);
    const FlightsModel::Aircraft& a = snap.ac[1];
    TEST_ASSERT_EQUAL_STRING("RYR45D", a.callsign);
    TEST_ASSERT_EQUAL_STRING("", a.airlineIata);
    TEST_ASSERT_EQUAL_STRING("", a.operatorName);
    TEST_ASSERT_EQUAL_STRING("", a.fromIata);
    TEST_ASSERT_EQUAL_STRING("", a.toIata);
    TEST_ASSERT_TRUE(a.routeKnown);
    TEST_ASSERT_TRUE(a.operatorKnown);
}

static void test_parseDialFlights_fixture_gndMapsToOnGround(void)
{
    FlightsModel::FlightsSnapshot snap;
    memset(&snap, 0, sizeof(snap));

    bool ok = FlightsModel::parseDialFlights(kFixtureDialFlights, strlen(kFixtureDialFlights), snap);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_FALSE(snap.ac[0].onGround);
    TEST_ASSERT_FALSE(snap.ac[1].onGround);
    TEST_ASSERT_TRUE(snap.ac[2].onGround);
    TEST_ASSERT_EQUAL_STRING("", snap.ac[2].flightNumber); // "fl":"" on the ground entry
}

static void test_parseDialFlights_sevenEntries_capsAtSix(void)
{
    static const char* kJson =
        "{\"ts\":1,\"ac\":["
        "{\"cs\":\"A1\"},{\"cs\":\"A2\"},{\"cs\":\"A3\"},{\"cs\":\"A4\"},"
        "{\"cs\":\"A5\"},{\"cs\":\"A6\"},{\"cs\":\"A7\"}"
        "]}";

    FlightsModel::FlightsSnapshot snap;
    memset(&snap, 0, sizeof(snap));

    bool ok = FlightsModel::parseDialFlights(kJson, strlen(kJson), snap);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT8(6, snap.count);
    TEST_ASSERT_EQUAL_STRING("A1", snap.ac[0].callsign);
    TEST_ASSERT_EQUAL_STRING("A6", snap.ac[5].callsign);
}

static void test_parseDialFlights_malformedJson_returnsFalse(void)
{
    static const char* kJson = "{not valid json";

    FlightsModel::FlightsSnapshot snap;
    memset(&snap, 0, sizeof(snap));

    bool ok = FlightsModel::parseDialFlights(kJson, strlen(kJson), snap);

    TEST_ASSERT_FALSE(ok);
}

static void test_parseDialFlights_missingAc_returnsFalse(void)
{
    static const char* kJson = "{\"ts\":1}";

    FlightsModel::FlightsSnapshot snap;
    memset(&snap, 0, sizeof(snap));

    bool ok = FlightsModel::parseDialFlights(kJson, strlen(kJson), snap);

    TEST_ASSERT_FALSE(ok);
}

static void test_parseDialFlights_emptyAircraftList_isZeroCountAndTrue(void)
{
    static const char* kJson = "{\"ts\":1,\"ac\":[]}";

    FlightsModel::FlightsSnapshot snap;
    memset(&snap, 0, sizeof(snap));

    bool ok = FlightsModel::parseDialFlights(kJson, strlen(kJson), snap);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT8(0, snap.count);
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_parseDialFlights_fixture_parsesThreeInPayloadOrder);
    RUN_TEST(test_parseDialFlights_fixture_firstEntryAllFields);
    RUN_TEST(test_parseDialFlights_fixture_emptyStringsPreserved);
    RUN_TEST(test_parseDialFlights_fixture_gndMapsToOnGround);
    RUN_TEST(test_parseDialFlights_sevenEntries_capsAtSix);
    RUN_TEST(test_parseDialFlights_malformedJson_returnsFalse);
    RUN_TEST(test_parseDialFlights_missingAc_returnsFalse);
    RUN_TEST(test_parseDialFlights_emptyAircraftList_isZeroCountAndTrue);
    return UNITY_END();
}
