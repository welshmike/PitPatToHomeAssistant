#include <unity.h>
#include <string.h>
#include "FlightsModel.h"
#include "../fixtures/fixtures.h"

void setUp(void) {}
void tearDown(void) {}

// kFixtureDialFlights (test/fixtures/dial_flights.json): a hand-written HA
// payload matching the spec 4.11 contract, 3 aircraft in payload order:
//   [0] BAW123 - full route/airline/operator/city known, airborne
//   [1] RYR45D - empty fr/to/al/an/fc/tc (route/airline not yet known to HA)
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
    TEST_ASSERT_EQUAL_STRING("London", a.fromCity);
    TEST_ASSERT_EQUAL_STRING("New York", a.toCity);
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
    TEST_ASSERT_EQUAL_STRING("", a.fromCity);
    TEST_ASSERT_EQUAL_STRING("", a.toCity);
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

// Payloads published before HA gained the fc/tc keys (backward compatibility):
// no "fc"/"tc" in the JSON at all should still parse cleanly, with the new
// city fields left empty rather than causing a parse failure.
static void test_parseDialFlights_noCityKeys_parsesWithEmptyCities(void)
{
    static const char* kJson =
        "{\"ts\":1,\"ac\":["
        "{\"cs\":\"BAW123\",\"fl\":\"BA123\",\"ty\":\"A320\",\"al\":\"BA\","
        "\"an\":\"British Airways\",\"fr\":\"LHR\",\"to\":\"JFK\","
        "\"alt\":12000,\"gs\":450,\"di\":2.3,\"br\":135,\"gnd\":0}"
        "]}";

    FlightsModel::FlightsSnapshot snap;
    memset(&snap, 0, sizeof(snap));

    bool ok = FlightsModel::parseDialFlights(kJson, strlen(kJson), snap);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT8(1, snap.count);
    TEST_ASSERT_EQUAL_STRING("LHR", snap.ac[0].fromIata);
    TEST_ASSERT_EQUAL_STRING("JFK", snap.ac[0].toIata);
    TEST_ASSERT_EQUAL_STRING("", snap.ac[0].fromCity);
    TEST_ASSERT_EQUAL_STRING("", snap.ac[0].toCity);
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

// Bearing arrives from HA as degrees and is normalised into [0,359] so
// Geo::compass8() never sees an out-of-range angle: 360 is due north (0),
// and a negative bearing wraps forward rather than indexing backwards.
static void test_parseDialFlights_bearing360_normalisesToZero(void)
{
    static const char* kJson = "{\"ts\":1,\"ac\":[{\"cs\":\"BAW1\",\"br\":360}]}";

    FlightsModel::FlightsSnapshot snap;
    memset(&snap, 0, sizeof(snap));

    TEST_ASSERT_TRUE(FlightsModel::parseDialFlights(kJson, strlen(kJson), snap));
    TEST_ASSERT_EQUAL_UINT8(1, snap.count);
    TEST_ASSERT_EQUAL_INT(0, snap.ac[0].bearing);
}

static void test_parseDialFlights_negativeBearing_wrapsForward(void)
{
    static const char* kJson = "{\"ts\":1,\"ac\":[{\"cs\":\"BAW1\",\"br\":-10}]}";

    FlightsModel::FlightsSnapshot snap;
    memset(&snap, 0, sizeof(snap));

    TEST_ASSERT_TRUE(FlightsModel::parseDialFlights(kJson, strlen(kJson), snap));
    TEST_ASSERT_EQUAL_UINT8(1, snap.count);
    TEST_ASSERT_EQUAL_INT(350, snap.ac[0].bearing);
}

// "gnd" is optional and HA has published it both as 0/1 and as a JSON
// boolean; an entry without it is airborne, not on the ground (the card
// draws "on ground" off this flag, so a wrong default is visible).
static void test_parseDialFlights_missingGnd_isNotOnGround(void)
{
    static const char* kJson = "{\"ts\":1,\"ac\":[{\"cs\":\"BAW1\",\"alt\":12000}]}";

    FlightsModel::FlightsSnapshot snap;
    memset(&snap, 0, sizeof(snap));

    TEST_ASSERT_TRUE(FlightsModel::parseDialFlights(kJson, strlen(kJson), snap));
    TEST_ASSERT_EQUAL_UINT8(1, snap.count);
    TEST_ASSERT_FALSE(snap.ac[0].onGround);
}

static void test_parseDialFlights_jsonBooleanGnd_isOnGround(void)
{
    static const char* kJson = "{\"ts\":1,\"ac\":[{\"cs\":\"GABCD\",\"gnd\":true}]}";

    FlightsModel::FlightsSnapshot snap;
    memset(&snap, 0, sizeof(snap));

    TEST_ASSERT_TRUE(FlightsModel::parseDialFlights(kJson, strlen(kJson), snap));
    TEST_ASSERT_EQUAL_UINT8(1, snap.count);
    TEST_ASSERT_TRUE(snap.ac[0].onGround);
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
    RUN_TEST(test_parseDialFlights_noCityKeys_parsesWithEmptyCities);
    RUN_TEST(test_parseDialFlights_sevenEntries_capsAtSix);
    RUN_TEST(test_parseDialFlights_malformedJson_returnsFalse);
    RUN_TEST(test_parseDialFlights_missingAc_returnsFalse);
    RUN_TEST(test_parseDialFlights_emptyAircraftList_isZeroCountAndTrue);
    RUN_TEST(test_parseDialFlights_bearing360_normalisesToZero);
    RUN_TEST(test_parseDialFlights_negativeBearing_wrapsForward);
    RUN_TEST(test_parseDialFlights_missingGnd_isNotOnGround);
    RUN_TEST(test_parseDialFlights_jsonBooleanGnd_isOnGround);
    return UNITY_END();
}
