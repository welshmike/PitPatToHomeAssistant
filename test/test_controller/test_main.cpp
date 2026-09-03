#include <unity.h>
#include <math.h>
#include "TreadmillController.h"
#include "TreadmillData.h"

// ---------------------------------------------------------------------------
// Test doubles
// ---------------------------------------------------------------------------

// Records every call the controller makes on the link, in order, so a test can
// assert both "what was sent" and "how many times".
class FakeLink : public ITreadmillLink
{
public:
    enum class CallType { START, PAUSE, STOP, SET_SPEED, CONNECT, DISCONNECT, PUBLISH };

    struct Call
    {
        CallType type;
        uint16_t raw; // only meaningful for SET_SPEED
    };

    static constexpr int MAX_CALLS = 32;
    Call     calls[MAX_CALLS];
    int      callCount = 0;

    bool          connected = false;
    TreadMillData data;                 // settable snapshot the controller reads
    TreadMillData lastOptimistic;       // last value passed to publishOptimistic()

    // --- ITreadmillLink ---
    bool isConnected() const override { return connected; }
    void start() override { record(CallType::START, 0); }
    void pause() override { record(CallType::PAUSE, 0); }
    void stop()  override { record(CallType::STOP, 0); }
    void setSpeedRaw(uint16_t raw) override { record(CallType::SET_SPEED, raw); }
    bool requestConnect() override { record(CallType::CONNECT, 0); return true; }
    bool requestDisconnect() override { record(CallType::DISCONNECT, 0); return true; }
    TreadMillData snapshot() const override { return data; }
    void publishOptimistic(const TreadMillData& d) override
    {
        lastOptimistic = d;
        data = d; // the real link writes its snapshot, so mirror that here
        record(CallType::PUBLISH, 0);
    }

    // --- helpers ---
    int countOf(CallType t) const
    {
        int n = 0;
        for (int i = 0; i < callCount; ++i)
            if (calls[i].type == t) n++;
        return n;
    }

    // First SET_SPEED raw value, or 0xFFFF if there wasn't one.
    uint16_t firstSpeedRaw() const
    {
        for (int i = 0; i < callCount; ++i)
            if (calls[i].type == CallType::SET_SPEED) return calls[i].raw;
        return 0xFFFF;
    }

private:
    void record(CallType t, uint16_t raw)
    {
        if (callCount < MAX_CALLS) calls[callCount++] = Call{t, raw};
    }
};

class RecordingObserver : public ISnapshotObserver
{
public:
    int           snapshotCount = 0;
    TreadMillData lastSnapshot;
    int           targetCount = 0;
    float         lastTargetMph = 0.0f;
    bool          lastTargetPending = false;

    void onSnapshot(const TreadMillData& d) override
    {
        snapshotCount++;
        lastSnapshot = d;
    }
    void onTargetSpeed(float mph, bool pending) override
    {
        targetCount++;
        lastTargetMph = mph;
        lastTargetPending = pending;
    }
};

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// setSpeedMph — clamping and the "slider at 0 means stop" rule
// ---------------------------------------------------------------------------

static void test_setSpeedMph_clampsAtLowEnd(void)
{
    FakeLink link;
    link.connected = true;
    link.data.status = TreadMillData::RUNNING;
    TreadmillController c(link);

    c.setSpeedMph(0.57f); // above the stop threshold but below SPEED_MIN_MPH

    TEST_ASSERT_EQUAL_INT(1, link.countOf(FakeLink::CallType::SET_SPEED));
    TEST_ASSERT_EQUAL_UINT16(960, link.firstSpeedRaw()); // 0.6 * 1600
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.6f, link.lastOptimistic.speedCmd);
}

static void test_setSpeedMph_clampsAtHighEnd(void)
{
    FakeLink link;
    link.connected = true;
    link.data.status = TreadMillData::RUNNING;
    TreadmillController c(link);

    c.setSpeedMph(9.0f);

    TEST_ASSERT_EQUAL_INT(1, link.countOf(FakeLink::CallType::SET_SPEED));
    TEST_ASSERT_EQUAL_UINT16(SPEED_RAW_MAX, link.firstSpeedRaw());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.8f, link.lastOptimistic.speedCmd);
}

static void test_setSpeedMph_zeroStopsBelt(void)
{
    FakeLink link;
    link.connected = true;
    link.data.status = TreadMillData::RUNNING;
    TreadmillController c(link);

    c.setSpeedMph(0.0f);

    TEST_ASSERT_EQUAL_INT(1, link.countOf(FakeLink::CallType::STOP));
    TEST_ASSERT_EQUAL_INT(0, link.countOf(FakeLink::CallType::SET_SPEED));
    TEST_ASSERT_EQUAL_INT(TreadMillData::STOPPED, (int)link.lastOptimistic.status);
}

static void test_setSpeedMph_whileStopped_publishesCountdown(void)
{
    FakeLink link;
    link.connected = true;
    link.data.status = TreadMillData::STOPPED;
    TreadmillController c(link);

    c.setSpeedMph(2.0f);

    TEST_ASSERT_EQUAL_UINT16(3200, link.firstSpeedRaw());
    TEST_ASSERT_EQUAL_INT(TreadMillData::COUNTDOWN, (int)link.lastOptimistic.status);
}

static void test_setSpeedMph_whileRunning_keepsStatus(void)
{
    FakeLink link;
    link.connected = true;
    link.data.status = TreadMillData::RUNNING;
    TreadmillController c(link);

    c.setSpeedMph(2.0f);

    TEST_ASSERT_EQUAL_INT(TreadMillData::RUNNING, (int)link.lastOptimistic.status);
}

// ---------------------------------------------------------------------------
// Optimistic status per command
// ---------------------------------------------------------------------------

static void test_start_publishesCountdown(void)
{
    FakeLink link;
    TreadmillController c(link);

    c.start();

    TEST_ASSERT_EQUAL_INT(1, link.countOf(FakeLink::CallType::START));
    TEST_ASSERT_EQUAL_INT(TreadMillData::COUNTDOWN, (int)link.lastOptimistic.status);
}

static void test_pause_publishesPaused(void)
{
    FakeLink link;
    link.data.status = TreadMillData::RUNNING;
    TreadmillController c(link);

    c.pause();

    TEST_ASSERT_EQUAL_INT(1, link.countOf(FakeLink::CallType::PAUSE));
    TEST_ASSERT_EQUAL_INT(TreadMillData::PAUSED, (int)link.lastOptimistic.status);
}

static void test_stop_publishesStopped(void)
{
    FakeLink link;
    link.data.status = TreadMillData::RUNNING;
    TreadmillController c(link);

    c.stop();

    TEST_ASSERT_EQUAL_INT(1, link.countOf(FakeLink::CallType::STOP));
    TEST_ASSERT_EQUAL_INT(TreadMillData::STOPPED, (int)link.lastOptimistic.status);
}

// ---------------------------------------------------------------------------
// resume() — speed comes from the snapshot's speedCmd, else START_SPEED_RAW
// ---------------------------------------------------------------------------

static void test_resume_usesSnapshotSpeedCmd(void)
{
    FakeLink link;
    link.connected = true;
    link.data.status = TreadMillData::PAUSED;
    link.data.speedCmd = 2.0f;
    TreadmillController c(link);

    c.resume();

    TEST_ASSERT_EQUAL_UINT16(3200, link.firstSpeedRaw());
}

static void test_resume_belowMinFallsBackToStartSpeed(void)
{
    FakeLink link;
    link.connected = true;
    link.data.status = TreadMillData::PAUSED;
    link.data.speedCmd = 0.0f;
    TreadmillController c(link);

    c.resume();

    TEST_ASSERT_EQUAL_UINT16(START_SPEED_RAW, link.firstSpeedRaw());
}

static void test_resume_doesNotAssumeRunning(void)
{
    FakeLink link;
    link.connected = true;
    link.data.status = TreadMillData::PAUSED;
    link.data.speedCmd = 2.0f;
    TreadmillController c(link);

    c.resume();

    // Status is left for the belt to report; only speedCmd is optimistic.
    TEST_ASSERT_EQUAL_INT(TreadMillData::PAUSED, (int)link.lastOptimistic.status);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, link.lastOptimistic.speedCmd);
}

// ---------------------------------------------------------------------------
// toggleStartPause — one row per status
// ---------------------------------------------------------------------------

static void test_toggleStartPause_disconnectedStarts(void)
{
    FakeLink link;
    link.data.status = TreadMillData::DISCONNECTED;
    TreadmillController c(link);

    c.toggleStartPause();

    TEST_ASSERT_EQUAL_INT(1, link.countOf(FakeLink::CallType::START));
}

static void test_toggleStartPause_stoppedStarts(void)
{
    FakeLink link;
    link.connected = true;
    link.data.status = TreadMillData::STOPPED;
    TreadmillController c(link);

    c.toggleStartPause();

    TEST_ASSERT_EQUAL_INT(1, link.countOf(FakeLink::CallType::START));
}

static void test_toggleStartPause_runningPauses(void)
{
    FakeLink link;
    link.connected = true;
    link.data.status = TreadMillData::RUNNING;
    TreadmillController c(link);

    c.toggleStartPause();

    TEST_ASSERT_EQUAL_INT(1, link.countOf(FakeLink::CallType::PAUSE));
}

static void test_toggleStartPause_pausedResumes(void)
{
    FakeLink link;
    link.connected = true;
    link.data.status = TreadMillData::PAUSED;
    link.data.speedCmd = 1.5f;
    TreadmillController c(link);

    c.toggleStartPause();

    TEST_ASSERT_EQUAL_INT(1, link.countOf(FakeLink::CallType::SET_SPEED));
    TEST_ASSERT_EQUAL_UINT16(2400, link.firstSpeedRaw()); // 1.5 * 1600
}

static void test_toggleStartPause_countdownStops(void)
{
    FakeLink link;
    link.connected = true;
    link.data.status = TreadMillData::COUNTDOWN;
    TreadmillController c(link);

    c.toggleStartPause();

    TEST_ASSERT_EQUAL_INT(1, link.countOf(FakeLink::CallType::STOP));
}

// ---------------------------------------------------------------------------
// nudgeSpeed / tick — settle-then-send
// ---------------------------------------------------------------------------

static void test_threeNudgesWithinSettleWindow_sendOneSetSpeed(void)
{
    FakeLink link;
    link.connected = true;
    link.data.status = TreadMillData::RUNNING;
    link.data.speedCmd = 2.0f;
    TreadmillController c(link);
    RecordingObserver obs;
    c.addObserver(obs);

    c.nudgeSpeed(1, 1000);
    c.tick(1050);
    c.nudgeSpeed(1, 1100);
    c.tick(1200);
    c.nudgeSpeed(1, 1250);
    c.tick(1400); // still before 1250 + 400

    TEST_ASSERT_EQUAL_INT(0, link.countOf(FakeLink::CallType::SET_SPEED));
    TEST_ASSERT_TRUE(obs.lastTargetPending);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.3f, c.targetSpeedMph());

    c.tick(1650); // deadline reached

    TEST_ASSERT_EQUAL_INT(1, link.countOf(FakeLink::CallType::SET_SPEED));
    TEST_ASSERT_EQUAL_UINT16(3680, link.firstSpeedRaw()); // 2.3 * 1600
    TEST_ASSERT_FALSE(obs.lastTargetPending);

    c.tick(5000); // does not fire twice

    TEST_ASSERT_EQUAL_INT(1, link.countOf(FakeLink::CallType::SET_SPEED));
}

static void test_nudgeWhileDisconnected_seedsFromMinAndSendsAfterSettle(void)
{
    FakeLink link;
    link.connected = false;
    link.data.status = TreadMillData::DISCONNECTED;
    link.data.speedCmd = 0.0f;
    TreadmillController c(link);

    c.nudgeSpeed(1, 1000);
    TEST_ASSERT_EQUAL_INT(0, link.countOf(FakeLink::CallType::SET_SPEED));

    c.tick(1400);

    // Seeded from SPEED_MIN_MPH (0.6) + one step = 0.7 mph. The link queues it.
    TEST_ASSERT_EQUAL_INT(1, link.countOf(FakeLink::CallType::SET_SPEED));
    TEST_ASSERT_EQUAL_UINT16(1120, link.firstSpeedRaw());
}

static void test_nudgeDown_clampsAtMin(void)
{
    FakeLink link;
    link.connected = true;
    link.data.status = TreadMillData::RUNNING;
    link.data.speedCmd = 0.7f;
    TreadmillController c(link);

    c.nudgeSpeed(-5, 1000);
    c.tick(1400);

    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.6f, c.targetSpeedMph());
    TEST_ASSERT_EQUAL_UINT16(960, link.firstSpeedRaw());
}

static void test_nudgeUp_clampsAtMax(void)
{
    FakeLink link;
    link.connected = true;
    link.data.status = TreadMillData::RUNNING;
    link.data.speedCmd = 3.7f;
    TreadmillController c(link);

    c.nudgeSpeed(5, 1000);
    c.tick(1400);

    TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.8f, c.targetSpeedMph());
    TEST_ASSERT_EQUAL_UINT16(SPEED_RAW_MAX, link.firstSpeedRaw());
}

static void test_nudge_notifiesTargetPendingThenSettled(void)
{
    FakeLink link;
    link.connected = true;
    link.data.status = TreadMillData::RUNNING;
    link.data.speedCmd = 2.0f;
    TreadmillController c(link);
    RecordingObserver obs;
    c.addObserver(obs);

    c.nudgeSpeed(1, 1000);
    TEST_ASSERT_EQUAL_INT(1, obs.targetCount);
    TEST_ASSERT_TRUE(obs.lastTargetPending);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.1f, obs.lastTargetMph);

    c.tick(1400);
    TEST_ASSERT_EQUAL_INT(2, obs.targetCount);
    TEST_ASSERT_FALSE(obs.lastTargetPending);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.1f, obs.lastTargetMph);
}

static void test_tick_withNoPendingNudge_doesNothing(void)
{
    FakeLink link;
    link.connected = true;
    link.data.status = TreadMillData::RUNNING;
    TreadmillController c(link);
    RecordingObserver obs;
    c.addObserver(obs);

    c.tick(999999);

    TEST_ASSERT_EQUAL_INT(0, link.callCount);
    TEST_ASSERT_EQUAL_INT(0, obs.targetCount);
}

// ---------------------------------------------------------------------------
// Observers
// ---------------------------------------------------------------------------

static void test_eachCommandNotifiesObserverExactlyOnce(void)
{
    FakeLink link;
    link.connected = true;
    link.data.status = TreadMillData::RUNNING;
    link.data.speedCmd = 2.0f;
    TreadmillController c(link);
    RecordingObserver obs;
    c.addObserver(obs);

    c.start();
    TEST_ASSERT_EQUAL_INT(1, obs.snapshotCount);
    c.pause();
    TEST_ASSERT_EQUAL_INT(2, obs.snapshotCount);
    c.resume();
    TEST_ASSERT_EQUAL_INT(3, obs.snapshotCount);
    c.setSpeedMph(2.5f);
    TEST_ASSERT_EQUAL_INT(4, obs.snapshotCount);
    c.stop();
    TEST_ASSERT_EQUAL_INT(5, obs.snapshotCount);

    // Each command wrote exactly one optimistic snapshot through the link too.
    TEST_ASSERT_EQUAL_INT(5, link.countOf(FakeLink::CallType::PUBLISH));
}

static void test_multipleObserversAllReceiveSnapshots(void)
{
    FakeLink link;
    link.data.status = TreadMillData::STOPPED;
    TreadmillController c(link);
    RecordingObserver a, b;
    c.addObserver(a);
    c.addObserver(b);

    c.start();

    TEST_ASSERT_EQUAL_INT(1, a.snapshotCount);
    TEST_ASSERT_EQUAL_INT(1, b.snapshotCount);
    TEST_ASSERT_EQUAL_INT(TreadMillData::COUNTDOWN, (int)a.lastSnapshot.status);
}

static void test_publish_pushesLinkSnapshotToObservers(void)
{
    FakeLink link;
    link.data.status = TreadMillData::RUNNING;
    link.data.distanceKm = 1.25f;
    TreadmillController c(link);
    RecordingObserver obs;
    c.addObserver(obs);

    c.publish();

    TEST_ASSERT_EQUAL_INT(1, obs.snapshotCount);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.25f, obs.lastSnapshot.distanceKm);
    TEST_ASSERT_EQUAL_INT(0, link.callCount); // publish() sends no command
}

// ---------------------------------------------------------------------------
// Connect / disconnect pass-through
// ---------------------------------------------------------------------------

static void test_requestConnectAndDisconnect_forwardToLink(void)
{
    FakeLink link;
    TreadmillController c(link);

    c.requestConnect();
    TEST_ASSERT_EQUAL_INT(1, link.countOf(FakeLink::CallType::CONNECT));

    c.requestDisconnect();
    TEST_ASSERT_EQUAL_INT(1, link.countOf(FakeLink::CallType::DISCONNECT));
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();

    RUN_TEST(test_setSpeedMph_clampsAtLowEnd);
    RUN_TEST(test_setSpeedMph_clampsAtHighEnd);
    RUN_TEST(test_setSpeedMph_zeroStopsBelt);
    RUN_TEST(test_setSpeedMph_whileStopped_publishesCountdown);
    RUN_TEST(test_setSpeedMph_whileRunning_keepsStatus);

    RUN_TEST(test_start_publishesCountdown);
    RUN_TEST(test_pause_publishesPaused);
    RUN_TEST(test_stop_publishesStopped);

    RUN_TEST(test_resume_usesSnapshotSpeedCmd);
    RUN_TEST(test_resume_belowMinFallsBackToStartSpeed);
    RUN_TEST(test_resume_doesNotAssumeRunning);

    RUN_TEST(test_toggleStartPause_disconnectedStarts);
    RUN_TEST(test_toggleStartPause_stoppedStarts);
    RUN_TEST(test_toggleStartPause_runningPauses);
    RUN_TEST(test_toggleStartPause_pausedResumes);
    RUN_TEST(test_toggleStartPause_countdownStops);

    RUN_TEST(test_threeNudgesWithinSettleWindow_sendOneSetSpeed);
    RUN_TEST(test_nudgeWhileDisconnected_seedsFromMinAndSendsAfterSettle);
    RUN_TEST(test_nudgeDown_clampsAtMin);
    RUN_TEST(test_nudgeUp_clampsAtMax);
    RUN_TEST(test_nudge_notifiesTargetPendingThenSettled);
    RUN_TEST(test_tick_withNoPendingNudge_doesNothing);

    RUN_TEST(test_eachCommandNotifiesObserverExactlyOnce);
    RUN_TEST(test_multipleObserversAllReceiveSnapshots);
    RUN_TEST(test_publish_pushesLinkSnapshotToObservers);

    RUN_TEST(test_requestConnectAndDisconnect_forwardToLink);

    return UNITY_END();
}
