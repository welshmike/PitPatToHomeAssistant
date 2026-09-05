#include "AirlineCodes.h"

#include <ctype.h>
#include <string.h>

namespace AirlineCodes
{

namespace
{

struct Entry
{
    const char* icao; // 3-letter ICAO airline designator, upper case
    const char* iata; // 2-3 char IATA airline code
};

// Common UK/EU/US majors and low-cost carriers, plus a handful of
// long-haul/international and all-cargo operators likely to be seen
// overhead. Only mappings we're confident are current are included.
static const Entry kTable[] = {
    {"BAW", "BA"}, // British Airways
    {"SHT", "BA"}, // BA Shuttle (domestic)
    {"VIR", "VS"}, // Virgin Atlantic
    {"EZY", "U2"}, // easyJet
    {"EZS", "DS"}, // easyJet Switzerland
    {"EJU", "U2"}, // easyJet Europe
    {"RYR", "FR"}, // Ryanair
    {"RUK", "RK"}, // Ryanair UK
    {"EXS", "LS"}, // Jet2
    {"TOM", "BY"}, // TUI Airways
    {"EIN", "EI"}, // Aer Lingus
    {"AAL", "AA"}, // American Airlines
    {"UAL", "UA"}, // United Airlines
    {"DAL", "DL"}, // Delta Air Lines
    {"DLH", "LH"}, // Lufthansa
    {"AFR", "AF"}, // Air France
    {"KLM", "KL"}, // KLM
    {"UAE", "EK"}, // Emirates
    {"QTR", "QR"}, // Qatar Airways
    {"ETD", "EY"}, // Etihad Airways
    {"SIA", "SQ"}, // Singapore Airlines
    {"CPA", "CX"}, // Cathay Pacific
    {"JAL", "JL"}, // Japan Airlines
    {"ANA", "NH"}, // All Nippon Airways
    {"QFA", "QF"}, // Qantas
    {"ACA", "AC"}, // Air Canada
    {"IBE", "IB"}, // Iberia
    {"VLG", "VY"}, // Vueling
    {"TAP", "TP"}, // TAP Air Portugal
    {"SAS", "SK"}, // SAS
    {"FIN", "AY"}, // Finnair
    {"SWR", "LX"}, // Swiss Intl Air Lines
    {"AUA", "OS"}, // Austrian Airlines
    {"BEL", "SN"}, // Brussels Airlines
    {"LOT", "LO"}, // LOT Polish Airlines
    {"WZZ", "W6"}, // Wizz Air
    {"NOZ", "DY"}, // Norwegian Air Norway
    {"NSZ", "D8"}, // Norwegian Air Sweden
    {"THY", "TK"}, // Turkish Airlines
    {"ELY", "LY"}, // El Al
    {"AIC", "AI"}, // Air India
    {"ETH", "ET"}, // Ethiopian Airlines
    {"KQA", "KQ"}, // Kenya Airways
    {"SAA", "SA"}, // South African Airways
    {"AMX", "AM"}, // Aeromexico
    {"LAN", "LA"}, // LATAM Airlines (Chile)
    {"JBU", "B6"}, // JetBlue
    {"SWA", "WN"}, // Southwest Airlines
    {"ASA", "AS"}, // Alaska Airlines
    {"WJA", "WS"}, // WestJet
    {"ICE", "FI"}, // Icelandair
    {"TRA", "HV"}, // Transavia
    {"EWG", "EW"}, // Eurowings
    {"CFG", "DE"}, // Condor
    {"LOG", "LM"}, // Loganair
    {"BEE", "BE"}, // Flybe
    {"PGT", "PC"}, // Pegasus Airlines
    {"AEE", "A3"}, // Aegean Airlines
    {"CSA", "OK"}, // Czech Airlines
    {"TVF", "TO"}, // Transavia France
    {"VOE", "V7"}, // Volotea
    {"GEC", "LH"}, // Lufthansa Cargo
    {"CLX", "CV"}, // Cargolux
    {"UPS", "5X"}, // UPS Airlines
    {"FDX", "FX"}, // FedEx Express
};

constexpr size_t kTableSize = sizeof(kTable) / sizeof(kTable[0]);

} // namespace

// Plan 7 Task 2 removes this (dead once FlightsService is MQTT-fed)
const char* iataFromIcao(const char* icao)
{
    if (icao == nullptr)
    {
        return nullptr;
    }

    char key[4] = {0, 0, 0, 0};
    for (int i = 0; i < 3; ++i)
    {
        const char c = icao[i];
        if (c == '\0' || !isalpha(static_cast<unsigned char>(c)))
        {
            return nullptr;
        }
        key[i] = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    }

    for (size_t i = 0; i < kTableSize; ++i)
    {
        if (strncmp(key, kTable[i].icao, 3) == 0)
        {
            return kTable[i].iata;
        }
    }

    return nullptr;
}

} // namespace AirlineCodes
