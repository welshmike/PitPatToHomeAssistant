#include <unity.h>
#include <string.h>
#include "FlightsModel.h"
#include "../fixtures/fixtures.h"

void setUp(void) {}
void tearDown(void) {}

// Home point used to fetch the London fixture: central London,
// 51.5074,-0.1278 (see test/fixtures/adsbfi_point_london_2026-09-04.json).
static const float kHomeLat = 51.5074f;
static const float kHomeLon = -0.1278f;

// Expected nearest-first order, computed independently with a Python
// haversine/initial-bearing script against kHomeLat/kHomeLon (WGS-84
// spherical, R = 3958.8 mi, matching Geo::distanceMiles):
//   VIR359   dist=2.899 mi
//   SHT3T    dist=3.583 mi
//   SAS84F   dist=5.406 mi
//   RWD710   dist=5.528 mi
//   SHT17Q   dist=5.619 mi
//   SWR3VB   dist=7.442 mi   <- 6th, caps the list
//   KLC982   dist=7.546 mi   <- dropped by the cap
//   CFE23DB  dist=7.740 mi   <- dropped by the cap
// These distances are also corroborated by the fixture's own "dst" (nm) and
// "dir" (deg) fields once dst is converted to statute miles (dst * 1.15078),
// which line up with the Python figures to within ~0.02 mi.

static void test_parseAdsbFi_londonFixture_capsAtSixNearestFirst(void)
{
    FlightsModel::FlightsSnapshot snap;
    memset(&snap, 0, sizeof(snap));

    bool ok = FlightsModel::parseAdsbFi(kFixtureAdsbFiLondon, strlen(kFixtureAdsbFiLondon), kHomeLat, kHomeLon, snap);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT8(6, snap.count);
    TEST_ASSERT_EQUAL_STRING("VIR359", snap.ac[0].callsign);
    TEST_ASSERT_EQUAL_STRING("SHT3T", snap.ac[1].callsign);

    for (uint8_t i = 1; i < snap.count; i++)
    {
        TEST_ASSERT_TRUE(snap.ac[i].distMi >= snap.ac[i - 1].distMi);
    }
}

static void test_parseAdsbFi_londonFixture_firstEntryAltAndSpeed(void)
{
    FlightsModel::FlightsSnapshot snap;
    memset(&snap, 0, sizeof(snap));

    bool ok = FlightsModel::parseAdsbFi(kFixtureAdsbFiLondon, strlen(kFixtureAdsbFiLondon), kHomeLat, kHomeLon, snap);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("VIR359", snap.ac[0].callsign);
    TEST_ASSERT_EQUAL_INT(3750, snap.ac[0].altFt); // VIR359 alt_baro in the fixture
    TEST_ASSERT_EQUAL_INT(144, snap.ac[0].gsKt);    // VIR359 gs in the fixture
}

static void test_parseAdsbFi_altBaroGround_mapsToZero(void)
{
    static const char* kJson =
        "{\"aircraft\":[{\"hex\":\"abc123\",\"flight\":\"TEST1   \",\"t\":\"A320\","
        "\"alt_baro\":\"ground\",\"lat\":51.5,\"lon\":-0.1}]}";

    FlightsModel::FlightsSnapshot snap;
    memset(&snap, 0, sizeof(snap));

    bool ok = FlightsModel::parseAdsbFi(kJson, strlen(kJson), kHomeLat, kHomeLon, snap);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT8(1, snap.count);
    TEST_ASSERT_EQUAL_STRING("TEST1", snap.ac[0].callsign);
    TEST_ASSERT_EQUAL_INT(0, snap.ac[0].altFt);
    TEST_ASSERT_EQUAL_INT(0, snap.ac[0].gsKt);  // missing "gs" -> 0
    TEST_ASSERT_EQUAL_INT(0, snap.ac[0].track); // missing "track" -> 0
}

static void test_parseAdsbFi_missingLat_isSkipped(void)
{
    static const char* kJson =
        "{\"aircraft\":["
        "{\"hex\":\"abc123\",\"flight\":\"NOPOS\",\"t\":\"A320\",\"alt_baro\":1000,\"lon\":-0.1},"
        "{\"hex\":\"def456\",\"flight\":\"HASPOS\",\"t\":\"A320\",\"alt_baro\":2000,\"lat\":51.5,\"lon\":-0.1}"
        "]}";

    FlightsModel::FlightsSnapshot snap;
    memset(&snap, 0, sizeof(snap));

    bool ok = FlightsModel::parseAdsbFi(kJson, strlen(kJson), kHomeLat, kHomeLon, snap);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT8(1, snap.count);
    TEST_ASSERT_EQUAL_STRING("HASPOS", snap.ac[0].callsign);
}

static void test_parseAdsbFi_emptyFlightIsSkipped(void)
{
    static const char* kJson =
        "{\"aircraft\":["
        "{\"hex\":\"abc123\",\"flight\":\"   \",\"t\":\"A320\",\"alt_baro\":1000,\"lat\":51.5,\"lon\":-0.1},"
        "{\"hex\":\"def456\",\"flight\":\"REAL1\",\"t\":\"A320\",\"alt_baro\":2000,\"lat\":51.5,\"lon\":-0.1}"
        "]}";

    FlightsModel::FlightsSnapshot snap;
    memset(&snap, 0, sizeof(snap));

    bool ok = FlightsModel::parseAdsbFi(kJson, strlen(kJson), kHomeLat, kHomeLon, snap);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT8(1, snap.count);
    TEST_ASSERT_EQUAL_STRING("REAL1", snap.ac[0].callsign);
}

static void test_parseAdsbFi_emptyAircraftList_isZeroCount(void)
{
    static const char* kJson = "{\"aircraft\":[],\"resultCount\":0}";

    FlightsModel::FlightsSnapshot snap;
    memset(&snap, 0, sizeof(snap));

    bool ok = FlightsModel::parseAdsbFi(kJson, strlen(kJson), kHomeLat, kHomeLon, snap);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT8(0, snap.count);
}

static void test_parseAdsbFi_malformedJson_returnsFalse(void)
{
    static const char* kJson = "{not valid json";

    FlightsModel::FlightsSnapshot snap;
    memset(&snap, 0, sizeof(snap));

    bool ok = FlightsModel::parseAdsbFi(kJson, strlen(kJson), kHomeLat, kHomeLon, snap);

    TEST_ASSERT_FALSE(ok);
}

static void test_parseHexdbRoute_fixture_splitsIcaoCodes(void)
{
    char from[5];
    char to[5];
    bool ok = FlightsModel::parseHexdbRoute(kFixtureHexdbRouteVIR359, strlen(kFixtureHexdbRouteVIR359), from, to);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("VABB", from);
    TEST_ASSERT_EQUAL_STRING("EGLL", to);
}

static void test_parseHexdbRoute_unknown_returnsFalse(void)
{
    static const char* kJson = "{\"route\":\"unknown\"}";
    char from[5];
    char to[5];

    bool ok = FlightsModel::parseHexdbRoute(kJson, strlen(kJson), from, to);

    TEST_ASSERT_FALSE(ok);
}

static void test_parseHexdbRoute_missing_returnsFalse(void)
{
    static const char* kJson = "{}";
    char from[5];
    char to[5];

    bool ok = FlightsModel::parseHexdbRoute(kJson, strlen(kJson), from, to);

    TEST_ASSERT_FALSE(ok);
}

static void test_parseHexdbAirport_fixture_returnsIata(void)
{
    char iata[5];
    bool ok = FlightsModel::parseHexdbAirport(kFixtureHexdbAirportEGLL, strlen(kFixtureHexdbAirportEGLL), iata);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("LHR", iata);
}

static void test_parseHexdbAircraft_fixture_returnsOperator(void)
{
    char icao[4];
    char name[32];
    bool ok = FlightsModel::parseHexdbAircraft(kFixtureHexdbAircraft407131, strlen(kFixtureHexdbAircraft407131), icao, name);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("VIR", icao);
    TEST_ASSERT_EQUAL_STRING("Virgin Atlantic Airways", name);
}

static void test_parseHexdbAircraft_longName_truncatesSafely(void)
{
    static const char* kJson =
        "{\"OperatorFlagCode\":\"XXX\","
        "\"RegisteredOwners\":\"A Really Really Long Made Up Airline Name That Goes On For Ages\"}";
    char icao[4];
    char name[32];

    bool ok = FlightsModel::parseHexdbAircraft(kJson, strlen(kJson), icao, name);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("XXX", icao);
    TEST_ASSERT_EQUAL_UINT(31, strlen(name)); // truncated into a 32-byte buffer, still null-terminated
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_parseAdsbFi_londonFixture_capsAtSixNearestFirst);
    RUN_TEST(test_parseAdsbFi_londonFixture_firstEntryAltAndSpeed);
    RUN_TEST(test_parseAdsbFi_altBaroGround_mapsToZero);
    RUN_TEST(test_parseAdsbFi_missingLat_isSkipped);
    RUN_TEST(test_parseAdsbFi_emptyFlightIsSkipped);
    RUN_TEST(test_parseAdsbFi_emptyAircraftList_isZeroCount);
    RUN_TEST(test_parseAdsbFi_malformedJson_returnsFalse);
    RUN_TEST(test_parseHexdbRoute_fixture_splitsIcaoCodes);
    RUN_TEST(test_parseHexdbRoute_unknown_returnsFalse);
    RUN_TEST(test_parseHexdbRoute_missing_returnsFalse);
    RUN_TEST(test_parseHexdbAirport_fixture_returnsIata);
    RUN_TEST(test_parseHexdbAircraft_fixture_returnsOperator);
    RUN_TEST(test_parseHexdbAircraft_longName_truncatesSafely);
    return UNITY_END();
}
