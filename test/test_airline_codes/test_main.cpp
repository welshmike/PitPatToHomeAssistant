#include <unity.h>
#include <string.h>
#include "AirlineCodes.h"

void setUp(void) {}
void tearDown(void) {}

static void expectIata(const char* icao, const char* expectedIata)
{
    const char* iata = AirlineCodes::iataFromIcao(icao);
    TEST_ASSERT_NOT_NULL(iata);
    TEST_ASSERT_EQUAL_STRING(expectedIata, iata);
}

static void test_baw_isBritishAirways(void)
{
    expectIata("BAW", "BA");
}

static void test_lowercaseCallsignWithTrailingSpace(void)
{
    // Real callsign form: "BAW117 " (padded to 8 chars by some feeds).
    expectIata("baw117 ", "BA");
}

static void test_callsignUsesFirstThreeLetters(void)
{
    expectIata("VIR359", "VS");
}

static void test_easyJet(void)
{
    expectIata("EZY8461", "U2");
}

static void test_moreMappings(void)
{
    expectIata("RYR", "FR");
    expectIata("EIN", "EI");
    expectIata("AAL", "AA");
    expectIata("UAL", "UA");
    expectIata("DAL", "DL");
    expectIata("DLH", "LH");
    expectIata("AFR", "AF");
    expectIata("KLM", "KL");
    expectIata("UAE", "EK");
    expectIata("QTR", "QR");
    expectIata("SIA", "SQ");
    expectIata("CPA", "CX");
    expectIata("JAL", "JL");
    expectIata("ANA", "NH");
    expectIata("QFA", "QF");
    expectIata("ACA", "AC");
    expectIata("IBE", "IB");
    expectIata("VLG", "VY");
    expectIata("TAP", "TP");
    expectIata("SAS", "SK");
    expectIata("FIN", "AY");
    expectIata("SWR", "LX");
    expectIata("WZZ", "W6");
    expectIata("THY", "TK");
    expectIata("ELY", "LY");
    expectIata("AIC", "AI");
    expectIata("SWA", "WN");
    expectIata("ASA", "AS");
    expectIata("JBU", "B6");
    expectIata("EJU", "U2");
    expectIata("UPS", "5X");
    expectIata("FDX", "FX");
}

static void test_unknownIcao_returnsNull(void)
{
    TEST_ASSERT_NULL(AirlineCodes::iataFromIcao("XYZ"));
}

static void test_emptyString_returnsNull(void)
{
    TEST_ASSERT_NULL(AirlineCodes::iataFromIcao(""));
}

static void test_shortString_returnsNull(void)
{
    TEST_ASSERT_NULL(AirlineCodes::iataFromIcao("BA"));
}

static void test_nullptr_returnsNull(void)
{
    TEST_ASSERT_NULL(AirlineCodes::iataFromIcao(nullptr));
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_baw_isBritishAirways);
    RUN_TEST(test_lowercaseCallsignWithTrailingSpace);
    RUN_TEST(test_callsignUsesFirstThreeLetters);
    RUN_TEST(test_easyJet);
    RUN_TEST(test_moreMappings);
    RUN_TEST(test_unknownIcao_returnsNull);
    RUN_TEST(test_emptyString_returnsNull);
    RUN_TEST(test_shortString_returnsNull);
    RUN_TEST(test_nullptr_returnsNull);
    return UNITY_END();
}
