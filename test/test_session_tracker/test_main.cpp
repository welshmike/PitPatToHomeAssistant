// Unit tests for SessionTracker — the pure session-accounting core extracted from
// TreadmillHandler. These run on the host (env:native, -DNATIVE_TEST) with no
// Arduino or NimBLE dependencies.
//
// Every behaviour asserted here mirrors what TreadmillHandler::notifyCallback(),
// onDisconnect(), stop(), pause() and the handle() pause-timeout block did before
// the extraction. See doc/Q1_BLE_NOTES.md ("Session Delta Accounting").

#include <unity.h>
#include <vector>

#include "TreadmillData.h"
#include "ba05protocol.h"
#include "SessionTracker.h"

// ---------------------------------------------------------------------------
// Fakes
// ---------------------------------------------------------------------------

// Step length is fixed at 0.5 m so step maths is trivial: steps = km * 2000.
static constexpr float FAKE_STEP_LENGTH_M = 0.5f;

class FakeTotals : public ITotalsStore
{
public:
    float    m_dist = 0.0f;
    uint32_t m_steps = 0;
    uint32_t m_cal = 0;
    uint32_t m_dur = 0;

    std::vector<SessionDelta> calls;

    float    totalDistanceKm()  const override { return m_dist; }
    uint32_t totalSteps()       const override { return m_steps; }
    uint32_t totalCalories()    const override { return m_cal; }
    uint32_t totalDurationSec() const override { return m_dur; }
    float    stepLengthM(float /*speedMph*/) const override { return FAKE_STEP_LENGTH_M; }

    void addSession(const SessionDelta& d) override
    {
        // Case 9: a delta must never be negative, in any scenario.
        TEST_ASSERT_TRUE_MESSAGE(d.distKm >= 0.0f, "addSession got a negative distance delta");
        calls.push_back(d);
        m_dist += d.distKm;
        m_steps += d.steps;
        m_cal += d.calories;
        m_dur += d.durationSec;
    }
};

class FakeEvents : public ISessionEvents
{
public:
    int started = 0, ended = 0, paused = 0, resumed = 0, settledIdle = 0;

    void onSessionStarted() override { started++; }
    void onSessionEnded()   override { ended++; }
    void onPaused()         override { paused++; }
    void onResumed()        override { resumed++; }
    void onSettledIdle()    override { settledIdle++; }
};

static FakeTotals    *g_totals  = nullptr;
static FakeEvents    *g_events  = nullptr;
static SessionTracker *g_tracker = nullptr;

void setUp(void)
{
    g_totals  = new FakeTotals();
    g_events  = new FakeEvents();
    g_tracker = new SessionTracker(*g_totals, *g_events);
}

void tearDown(void)
{
    delete g_tracker; g_tracker = nullptr;
    delete g_events;  g_events  = nullptr;
    delete g_totals;  g_totals  = nullptr;
}

// Fabricate a full (>=50 byte) BA05 frame's parsed form directly — the tracker
// never sees raw bytes, so no packet building is needed.
static BA05Protocol::ParsedData mkFull(uint8_t type,
                                       TreadMillData::Status status,
                                       float speedMph,
                                       uint16_t distM,
                                       uint8_t cal = 0,
                                       uint32_t durSec = 0)
{
    BA05Protocol::ParsedData p;
    p.valid         = true;
    p.length        = 50;
    p.packetType    = type;
    p.speedFeedback = speedMph;
    p.speedCmd      = speedMph;
    p.speedMax      = 3.8f;
    p.distanceM     = distM;
    p.calories      = cal;
    p.durationSec   = durSec;
    p.status        = status;
    p.statusFlags   = (status == TreadMillData::STOPPED) ? 0x00 : 0xBC;
    return p;
}

// ---------------------------------------------------------------------------
// 1. First packet captures the connection baseline
// ---------------------------------------------------------------------------

static void test_firstPacketCapturesBaseline(void)
{
    TreadMillData d;
    g_tracker->onConnected(0);

    // Belt already shows 500 m on its odometer from a previous walk.
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::STOPPED, 0.0f, 500), d, 100);
    // Belt starts moving; odometer now 700 m => 200 m of new distance.
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::RUNNING, 2.0f, 700), d, 200);

    TEST_ASSERT_FLOAT_WITHIN(0.0001f, g_totals->totalDistanceKm() + 0.2f, d.totalDistanceKm);
    TEST_ASSERT_EQUAL_INT(1, g_events->started);
    TEST_ASSERT_TRUE(g_tracker->sessionActive());
    TEST_ASSERT_EQUAL_size_t(0, g_totals->calls.size());
}

// ---------------------------------------------------------------------------
// 2. Belt odometer reset after START zeroes the bases
// ---------------------------------------------------------------------------

static void test_odometerResetZeroesBases(void)
{
    TreadMillData d;
    g_tracker->onConnected(0);

    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::STOPPED, 0.0f, 500), d, 100);
    // User pressed START: belt cleared its odometer, now reports 50 m.
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::RUNNING, 2.0f, 50), d, 200);

    // Full 50 m counts, not a negative delta against the 500 m base.
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.05f, d.totalDistanceKm);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.05f, d.distanceKm);
}

// ---------------------------------------------------------------------------
// 3. Clean stop commits the delta exactly once
// ---------------------------------------------------------------------------

static void test_cleanStopCommitsDeltaOnce(void)
{
    TreadMillData d;
    g_tracker->onConnected(0);

    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::STOPPED, 0.0f, 500, 10, 60), d, 100);
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::RUNNING, 2.0f, 700, 15, 120), d, 200);
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::STOPPED, 0.0f, 700, 15, 120), d, 300);

    TEST_ASSERT_EQUAL_size_t(1, g_totals->calls.size());
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.2f, g_totals->calls[0].distKm);
    // 399, not 400: (0.7f - 0.5f) is 0.199999988, and the step count is truncated,
    // not rounded. This is the firmware's existing arithmetic, preserved verbatim.
    TEST_ASSERT_EQUAL_UINT32(399, g_totals->calls[0].steps);
    TEST_ASSERT_EQUAL_UINT32(5, g_totals->calls[0].calories);
    TEST_ASSERT_EQUAL_UINT32(60, g_totals->calls[0].durationSec);

    // Session summary published, live fields cleared.
    TEST_ASSERT_TRUE(d.sessionComplete);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.7f, d.sessionDistanceKm);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, d.distanceKm);
    TEST_ASSERT_EQUAL_UINT32(0, d.steps);
    TEST_ASSERT_EQUAL_UINT32(0, d.durationSec);

    TEST_ASSERT_EQUAL_INT(1, g_events->ended);
    TEST_ASSERT_FALSE(g_tracker->sessionActive());
}

// ---------------------------------------------------------------------------
// 4. Pause / resume: no commit until the belt really stops
// ---------------------------------------------------------------------------

static void test_pauseResumeDefersCommitUntilRealStop(void)
{
    TreadMillData d;
    g_tracker->onConnected(0);

    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::STOPPED, 0.0f, 0), d, 100);
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::RUNNING, 2.0f, 1000), d, 200);

    // User presses pause; the belt then reports STOPPED with distance intact.
    g_tracker->onPauseCommand();
    TEST_ASSERT_TRUE(g_tracker->isPaused());
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::STOPPED, 0.0f, 1000), d, 300);

    TEST_ASSERT_EQUAL_size_t(0, g_totals->calls.size());
    TEST_ASSERT_EQUAL_INT(1, g_events->paused);
    // Totals are held at the paused snapshot so HA's utility_meter doesn't dip.
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0f, d.totalDistanceKm);
    TEST_ASSERT_EQUAL_UINT32(2000, d.totalSteps);

    // Belt resumes from where it left off — session continues, still no commit.
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::RUNNING, 2.0f, 1000), d, 400);
    TEST_ASSERT_EQUAL_INT(1, g_events->resumed);
    TEST_ASSERT_FALSE(g_tracker->isPaused());
    TEST_ASSERT_EQUAL_size_t(0, g_totals->calls.size());

    // Belt's spurious startup STOPPED (distance unchanged) must be suppressed.
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::STOPPED, 0.0f, 1000), d, 500);
    TEST_ASSERT_EQUAL_size_t(0, g_totals->calls.size());

    // Real walking resumes, then a real stop — one commit of the whole delta.
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::RUNNING, 2.0f, 1500), d, 600);
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::STOPPED, 0.0f, 1500), d, 700);

    TEST_ASSERT_EQUAL_size_t(1, g_totals->calls.size());
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.5f, g_totals->calls[0].distKm);
    TEST_ASSERT_EQUAL_UINT32(3000, g_totals->calls[0].steps);
}

// ---------------------------------------------------------------------------
// 5. Pause, then the belt resets its odometer on resume
// ---------------------------------------------------------------------------

static void test_pauseThenBeltResetCommitsSnapshot(void)
{
    TreadMillData d;
    g_tracker->onConnected(0);

    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::STOPPED, 0.0f, 0), d, 100);
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::RUNNING, 2.0f, 1000, 20, 300), d, 200);

    g_tracker->onPauseCommand();
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::STOPPED, 0.0f, 1000, 20, 300), d, 300);
    TEST_ASSERT_EQUAL_size_t(0, g_totals->calls.size());

    // Belt reset its odometer while paused — commit the stored snapshot.
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::RUNNING, 2.0f, 0), d, 400);

    TEST_ASSERT_EQUAL_size_t(1, g_totals->calls.size());
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0f, g_totals->calls[0].distKm);
    TEST_ASSERT_EQUAL_UINT32(2000, g_totals->calls[0].steps);
    TEST_ASSERT_EQUAL_UINT32(20, g_totals->calls[0].calories);
    TEST_ASSERT_EQUAL_UINT32(300, g_totals->calls[0].durationSec);

    TEST_ASSERT_TRUE(d.sessionComplete);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0f, d.sessionDistanceKm);
    TEST_ASSERT_FALSE(g_tracker->isPaused());
}

// ---------------------------------------------------------------------------
// 6. Mid-session disconnect commits the connection delta
// ---------------------------------------------------------------------------

static void test_midSessionDisconnectCommitsDelta(void)
{
    TreadMillData d;
    g_tracker->onConnected(0);

    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::STOPPED, 0.0f, 200, 5, 30), d, 100);
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::RUNNING, 2.0f, 1200, 25, 630), d, 200);

    g_tracker->onDisconnected(d);

    TEST_ASSERT_EQUAL_size_t(1, g_totals->calls.size());
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0f, g_totals->calls[0].distKm);
    TEST_ASSERT_EQUAL_UINT32(2000, g_totals->calls[0].steps);
    TEST_ASSERT_EQUAL_UINT32(20, g_totals->calls[0].calories);
    TEST_ASSERT_EQUAL_UINT32(600, g_totals->calls[0].durationSec);
}

static void test_disconnectWhilePausedCommitsSnapshot(void)
{
    TreadMillData d;
    g_tracker->onConnected(0);

    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::STOPPED, 0.0f, 0), d, 100);
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::RUNNING, 2.0f, 800), d, 200);
    g_tracker->onPauseCommand();
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::STOPPED, 0.0f, 800), d, 300);

    g_tracker->onDisconnected(d);

    TEST_ASSERT_EQUAL_size_t(1, g_totals->calls.size());
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.8f, g_totals->calls[0].distKm);
    TEST_ASSERT_FALSE(g_tracker->isPaused());
}

// A mid-session drop commits what was walked so far; the reconnect recaptures a
// baseline at the belt's unchanged odometer, so the next commit is only the new
// distance. Without the recapture the second commit would repeat the first.
static void test_reconnectMidSessionDoesNotDoubleCount(void)
{
    TreadMillData d;
    g_tracker->onConnected(0);

    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::STOPPED, 0.0f, 200), d, 100);
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::RUNNING, 2.0f, 1200), d, 200);

    g_tracker->onDisconnected(d);
    TEST_ASSERT_EQUAL_size_t(1, g_totals->calls.size());
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0f, g_totals->calls[0].distKm);

    // Reconnect: the belt odometer is where we left it (1200 m), so the new
    // baseline must be 1200, not 200.
    g_tracker->onConnected(1000);
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::RUNNING, 2.0f, 1200), d, 1100);
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::RUNNING, 2.0f, 1500), d, 1200);
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::STOPPED, 0.0f, 1500), d, 1300);

    TEST_ASSERT_EQUAL_size_t(2, g_totals->calls.size());
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.3f, g_totals->calls[1].distKm);

    float committed = 0.0f;
    for (const SessionDelta& c : g_totals->calls) committed += c.distKm;
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.3f, committed);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.3f, g_totals->totalDistanceKm());
}

// ---------------------------------------------------------------------------
// 7. onStopCommand
// ---------------------------------------------------------------------------

static void test_stopCommandWhilePausedCommitsAndReturnsTrue(void)
{
    TreadMillData d;
    g_tracker->onConnected(0);

    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::STOPPED, 0.0f, 0), d, 100);
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::RUNNING, 2.0f, 600), d, 200);
    g_tracker->onPauseCommand();
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::STOPPED, 0.0f, 600), d, 300);

    TEST_ASSERT_TRUE(g_tracker->onStopCommand());
    TEST_ASSERT_EQUAL_size_t(1, g_totals->calls.size());
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.6f, g_totals->calls[0].distKm);
    TEST_ASSERT_FALSE(g_tracker->sessionActive());
    TEST_ASSERT_FALSE(g_tracker->isPaused());
}

static void test_stopCommandWhenNotPausedCommitsNothing(void)
{
    TreadMillData d;
    g_tracker->onConnected(0);

    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::STOPPED, 0.0f, 0), d, 100);
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::RUNNING, 2.0f, 600), d, 200);

    TEST_ASSERT_FALSE(g_tracker->onStopCommand());
    TEST_ASSERT_EQUAL_size_t(0, g_totals->calls.size());
    // Session is untouched — the belt's own STOPPED packet will commit it.
    TEST_ASSERT_TRUE(g_tracker->sessionActive());
}

// ---------------------------------------------------------------------------
// 8. onSettledIdle fires once, on 0x2F only
// ---------------------------------------------------------------------------

static void test_settledIdleFiresOnceOn0x2FOnly(void)
{
    TreadMillData d;
    g_tracker->onConnected(0);

    // Kicking phase: 0x34 packets must not arm the idle timer.
    d = g_tracker->onPacket(mkFull(0x34, TreadMillData::STOPPED, 0.0f, 0), d, 100);
    d = g_tracker->onPacket(mkFull(0x34, TreadMillData::STOPPED, 0.0f, 0), d, 200);
    TEST_ASSERT_EQUAL_INT(0, g_events->settledIdle);

    // Device settles to 0x2F with no session — arm once.
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::STOPPED, 0.0f, 0), d, 300);
    TEST_ASSERT_EQUAL_INT(1, g_events->settledIdle);

    // Further 0x2F packets are not a type change — no repeat.
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::STOPPED, 0.0f, 0), d, 400);
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::STOPPED, 0.0f, 0), d, 500);
    TEST_ASSERT_EQUAL_INT(1, g_events->settledIdle);
}

static void test_settledIdleNotFiredWhenSessionActive(void)
{
    TreadMillData d;
    g_tracker->onConnected(0);

    d = g_tracker->onPacket(mkFull(0x34, TreadMillData::STOPPED, 0.0f, 0), d, 100);
    d = g_tracker->onPacket(mkFull(0x34, TreadMillData::RUNNING, 2.0f, 500), d, 200);
    TEST_ASSERT_TRUE(g_tracker->sessionActive());

    // Type change to 0x2F while a session is live must not arm the idle timer.
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::STOPPED, 0.0f, 500), d, 300);
    TEST_ASSERT_EQUAL_INT(0, g_events->settledIdle);

    // Side effect of that last packet: RUNNING -> STOPPED with dist>0 and an active,
    // unpaused session is a clean stop, which commits exactly one session.
    TEST_ASSERT_EQUAL_size_t(1, g_totals->calls.size());
}

// ---------------------------------------------------------------------------
// Pause timeout
// ---------------------------------------------------------------------------

static void test_pauseTimeoutCommitsAndReturnsSummary(void)
{
    TreadMillData d;
    g_tracker->onConnected(0);

    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::STOPPED, 0.0f, 0), d, 100);
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::RUNNING, 2.0f, 900, 30, 450), d, 200);
    g_tracker->onPauseCommand();
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::STOPPED, 0.0f, 900, 30, 450), d, 300);

    TreadMillData summary = g_tracker->onPauseTimeout(d);

    TEST_ASSERT_EQUAL_size_t(1, g_totals->calls.size());
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.9f, g_totals->calls[0].distKm);

    TEST_ASSERT_TRUE(summary.sessionComplete);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.9f, summary.sessionDistanceKm);
    TEST_ASSERT_EQUAL_UINT32(1800, summary.sessionSteps);
    TEST_ASSERT_EQUAL_UINT32(30, summary.sessionCalories);
    TEST_ASSERT_EQUAL_UINT32(450, summary.sessionDurationSec);
    TEST_ASSERT_EQUAL_INT((int)TreadMillData::STOPPED, (int)summary.status);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, summary.distanceKm);
    TEST_ASSERT_EQUAL_UINT32(0, summary.steps);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.9f, summary.totalDistanceKm);

    TEST_ASSERT_FALSE(g_tracker->sessionActive());
    TEST_ASSERT_FALSE(g_tracker->isPaused());
}

// ---------------------------------------------------------------------------
// Short (20-byte) frames only update speed feedback
// ---------------------------------------------------------------------------

static void test_shortPacketOnlyUpdatesSpeedFeedback(void)
{
    TreadMillData d;
    g_tracker->onConnected(0);

    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::STOPPED, 0.0f, 0), d, 100);
    d = g_tracker->onPacket(mkFull(0x2F, TreadMillData::RUNNING, 2.0f, 400), d, 200);

    // A 20-byte status frame whose type byte happens to look like a full frame:
    // it must not be treated as a full frame or it would fake a stopped session.
    BA05Protocol::ParsedData shortPkt;
    shortPkt.valid         = true;
    shortPkt.length        = 20;
    shortPkt.packetType    = 0x2F;
    shortPkt.speedFeedback = 1.5f;

    TreadMillData after = g_tracker->onPacket(shortPkt, d, 300);

    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.5f, after.speedFeedback);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.4f, after.distanceKm);
    TEST_ASSERT_EQUAL_INT((int)TreadMillData::RUNNING, (int)after.status);
    TEST_ASSERT_EQUAL_size_t(0, g_totals->calls.size());
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();

    RUN_TEST(test_firstPacketCapturesBaseline);
    RUN_TEST(test_odometerResetZeroesBases);
    RUN_TEST(test_cleanStopCommitsDeltaOnce);
    RUN_TEST(test_pauseResumeDefersCommitUntilRealStop);
    RUN_TEST(test_pauseThenBeltResetCommitsSnapshot);
    RUN_TEST(test_midSessionDisconnectCommitsDelta);
    RUN_TEST(test_disconnectWhilePausedCommitsSnapshot);
    RUN_TEST(test_reconnectMidSessionDoesNotDoubleCount);
    RUN_TEST(test_stopCommandWhilePausedCommitsAndReturnsTrue);
    RUN_TEST(test_stopCommandWhenNotPausedCommitsNothing);
    RUN_TEST(test_settledIdleFiresOnceOn0x2FOnly);
    RUN_TEST(test_settledIdleNotFiredWhenSessionActive);
    RUN_TEST(test_pauseTimeoutCommitsAndReturnsSummary);
    RUN_TEST(test_shortPacketOnlyUpdatesSpeedFeedback);

    return UNITY_END();
}
