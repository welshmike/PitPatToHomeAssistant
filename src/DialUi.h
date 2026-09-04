#pragma once
#include "board.h"
#if HAS_DIAL_UI

#include <M5Dial.h>
#include <stdint.h>

#include "TreadmillController.h"
#include "NetStatus.h"
#include "DialInput.h"
#include "SpeedSelector.h"
#include "CardRing.h"
#include "TimeService.h"
#include "FlightsService.h"
#include "FlightsModel.h"
#include "LightsService.h"
#include "LightsModel.h"
#include "LightCardState.h"
#include "LightButtons.h"
#include "NetTask.h"

// Loop-task ISnapshotObserver that renders treadmill/net state to the Dial's
// round 240x240 display and drives the controller from the encoder, touch
// screen and side button. tick() pumps M5Dial.update() and DialInput::tick()
// every call (so debouncing/click/hold state machines stay healthy at full
// loop rate), independent of the render throttle below.
class DialUi : public ISnapshotObserver
{
public:
    // timeService backs the clock card (spec 4.8) — kept as a reference, not
    // a copy, so the clock always reads the live wall clock TimeService.tick()
    // is updating in main.cpp's loop(). net backs the Flights card (spec 4.9)
    // and the two Lights cards (Plan 6): m_flights/m_lights are bound to
    // NetTask's own FlightsService/LightsService members here, which is safe
    // before NetTask::begin() runs (see NetTask::flights()/lights()), and
    // m_net itself is only used to enqueue outgoing light commands.
    DialUi(TreadmillController& controller, const TimeService& timeService,
           NetTask& net);

    // Must run FIRST in setup() on the Dial — M5Unified owns display/I2C/
    // Serial init via M5Dial.begin(). Creates the render canvas (4bpp
    // palette, 240x240, ~28.8 KB); falls back to drawing straight onto
    // M5Dial.Display if the sprite allocation fails (logs ESP.getFreeHeap()
    // before/after either way).
    void begin();

    // Call once per loop() iteration, after controller.tick(). Reads inputs
    // and drives the controller every call; renders at most once every
    // kRenderIntervalMs.
    void tick(uint32_t nowMs);

    void onSnapshot(const TreadMillData& d) override;
    void onTargetSpeed(float mph, bool pending) override;
    void onNetStatus(NetStatus s) override;

private:
    static constexpr uint32_t kRenderIntervalMs = 50;

    // Display brightness levels, applied via M5Dial.Display.setBrightness()
    // only when DialInput's backlight state changes. No OFF level — the
    // backlight dims but never turns off (spec 4.8).
    static constexpr uint8_t kBrightFull = 255;
    static constexpr uint8_t kBrightDim  = 50;

    // Palette for the 4-bit (16-colour, ~28.8 KB) canvas sprite created in
    // begin() — every colour the UI draws, reduced to <= 16 distinct RGB
    // values (2026-09-04: down from an 8bpp/RGB332 canvas to save another
    // ~28.8 KB of heap for WiFi). See col()'s definition in DialUi.cpp for
    // how LovyanGFX interprets a draw call's colour argument for a palette
    // destination, with citations.
    enum class Col : uint8_t
    {
        BG = 0,     // screen background
        TEXT,       // primary text/foreground (also clock hands, ring track lit state's text)
        DIM,        // secondary/caption text, clock ticks, unlit ring track
        BLE_ON,     // BLE status dot when connected
        NET_ON,     // WiFi/MQTT status dots when up
        SPEED,      // speed ring/centre value while not pending (cyan)
        PENDING,    // speed ring/centre value while a nudge is settling, and the selector (amber)
        RED,        // long-press-to-stop progress arc
        SECOND,     // clock second hand — a distinct red shade from RED
        DIM_DIM,    // paused-dim shade of DIM (also the paused-dim shade of
                    // TEXT: dimColor565(TFT_WHITE) == TFT_DARKGREY exactly,
                    // so TEXT's paused shade is DIM itself, not DIM_DIM)
        SPEED_DIM,  // paused-dim shade of SPEED
        PENDING_DIM,// paused-dim shade of PENDING
        TRANSPARENT = 15, // canvas-only: skipped by pushSprite(transp) so the display keeps what is under it (the full-colour logo)
    };
    // Returns the value to pass as a "colour" to any LovyanGFX draw call on
    // gfx: the raw palette index (0-15) while drawing into the 4bpp
    // m_canvas (m_useCanvas), or the real 24-bit RGB for the direct-to-
    // M5Dial.Display fallback used when the sprite allocation fails.
    uint32_t col(Col c) const;

    void render(uint32_t nowMs);
    void draw(LovyanGFX& gfx, uint32_t nowMs);
    void drawStatusDots(LovyanGFX& gfx);
    void drawDisconnected(LovyanGFX& gfx);
    void drawClock(LovyanGFX& gfx);
    void drawConnecting(LovyanGFX& gfx);
    void drawStarting(LovyanGFX& gfx, uint32_t nowMs);
    void drawRunning(LovyanGFX& gfx, bool paused, uint32_t nowMs);
    // Flights card (spec 4.9): nearest aircraft, logo/route/altitude/speed.
    void drawFlights(LovyanGFX& gfx);
    // Lights cards (Plan 6, spec 4.10): title, value ring, brightness/colour
    // readout and the two/three on-screen buttons for one HA light.
    void drawLight(LovyanGFX& gfx, LightsModel::LightKey key);
    // The Power/Bright/Colour circles for drawLight(): one filled PENDING
    // circle for the engaged control, DIM outlines for the ones that can be
    // tapped, DIM_DIM outlines for the ones that can't.
    void drawLightButtons(LovyanGFX& gfx, const LightCardState& card, bool hasColour,
                          bool mqttUp);
    // Start-speed picker (spec 4.7), opened by a tap on Disconnected.
    void drawSelector(LovyanGFX& gfx);
    // Centre speed overlay (target speed in Font7 amber + "mph" caption, and
    // the amber target ring) — called from draw() for whichever screen is
    // showing, on top of it, while the overlay window (m_speedOverlayUntilMs)
    // is open. Shared by every screen, not just Running (I3).
    void drawSpeedOverlay(LovyanGFX& gfx, bool paused);
    // Long-press-to-stop progress ring — called from draw() for whichever
    // screen is showing, on top of it, whenever a hold is in progress (I4).
    void drawHoldArc(LovyanGFX& gfx);

    // Which of the six top-level screens is showing right now. Selection
    // order: controller.isConnecting() -> Connecting (checked BEFORE the
    // COUNTDOWN test below — a start() queued while disconnected makes the
    // controller publish an optimistic COUNTDOWN even though nothing is
    // actually counting down yet, so Connecting must win that race or the
    // Starting screen shows with no way to cancel it); COUNTDOWN -> Starting;
    // (RUNNING or paused) -> Running/Paused; m_selector.isOpen() -> Selector;
    // else the current card (m_cards.current(), spec 4.8): TREADMILL ->
    // Disconnected (the Treadmill card), CLOCK -> Clock, FLIGHTS -> Flights
    // (spec 4.9). Connecting/Starting/Running all win over an open selector —
    // handleInput() closes it as soon as any of those becomes true, so a
    // stale selector can't resurface once they clear; they also win over the
    // card ring, which simply resumes on its last card once the belt screens
    // clear. `paused` is passed in rather than recomputed so callers that
    // already have it (draw(), handleInput()) don't pay for isPausedState()
    // twice in the same tick.
    enum class Screen : uint8_t { DISCONNECTED, CLOCK, FLIGHTS, CONNECTING, STARTING, RUNNING,
                                  SELECTOR, LIGHT_OFFICE, LIGHT_LAMP };
    Screen currentScreen(bool paused) const;

    void handleInput(uint32_t nowMs);
    // Flights card housekeeping (spec 4.9), called from tick() every call
    // while screen == FLIGHTS: pulls a fresh snapshot at most every
    // kFlightsSnapIntervalMs, clamps m_flightIdx to the (possibly changed)
    // aircraft count, and tells FlightsService which airline logo the net
    // task should be fetching for the currently-selected aircraft.
    void tickFlights(uint32_t nowMs);
    // Lights card housekeeping (Plan 6), called from tick() every call while
    // the screen is a light card: pulls a fresh LightsSnapshot at most every
    // kLightsSnapIntervalMs and sync()s BOTH cards from it, then polls the
    // visible card's settle/idle timers and publishes whatever command
    // tick() hands back.
    void tickLights(uint32_t nowMs);
    // Looks up the LightCardState for a light screen (LIGHT_OFFICE or
    // LIGHT_LAMP) — centralises the screen/CardId -> LightKey -> m_lightCards[]
    // lookup that used to be open-coded at each call site. Undefined which
    // card is returned for a non-light Screen; every caller only passes one
    // after checking screen == LIGHT_OFFICE || screen == LIGHT_LAMP.
    LightCardState& lightCardFor(Screen screen);
    const LightCardState& lightCardFor(Screen screen) const;
    // Formats `cmd` into a LIGHT_CMD PublishItem and hands it to the net
    // task; the loop task never touches MQTT itself.
    void publishLightCommand(LightsModel::LightKey key, const LightsModel::Command& cmd);
    // Drops any engagement on both light cards, silently (a pending settle is
    // discarded, not sent). Called whenever the card ring leaves a light card
    // — by a knob scroll or the side button's home gesture — so a card left
    // mid-edit doesn't come back engaged, and no command fires later for an
    // edit the user walked away from.
    void releaseLightCards();
    void applyBrightness();
    // Plays the accepted/refused tone pair for a stop-or-cancel gesture
    // (Connecting-screen cancel, hold-to-stop, side button). No-op when
    // DIAL_SOUND is off.
    void playStopBeep(uint32_t nowMs, bool accepted);
    // Plays the tap-style accepted/refused tone (a single short beep, either
    // pitch) used for Selector open/confirm/cancel and start/pause toggling —
    // distinct from playStopBeep()'s two-tone stop/cancel pattern. No-op when
    // DIAL_SOUND is off.
    void playAcceptBeep(bool accepted);

    // Redraw-skip key for the render() throttle below: every field that can
    // change what drawRunning()/drawIdle() puts on screen, coarsened to the
    // granularity that's actually visible (tenths of mph, hundredths of a
    // km, 1/20ths of hold progress) so float jitter below that doesn't
    // trigger a redraw. POD, compared field-by-field — no heap, no float
    // comparison.
    struct FrameKey
    {
        uint8_t  status        = 0;
        uint8_t  screen        = 0;
        bool     paused        = false;
        uint32_t durationSec   = 0;
        int32_t  speedTenths   = 0;
        int32_t  distanceCenti = 0;
        uint32_t steps         = 0;
        int32_t  targetTenths  = 0;
        bool     pending       = false;
        bool     overlayActive = false;
        uint8_t  netStatus     = 0;
        int32_t  holdUnits     = 0;
        uint8_t  pulsePhase    = 0;
        uint16_t connectAttempts   = 0;
        uint32_t sessionDurationSec = 0;
        int32_t  sessionDistanceCenti = 0;
        uint32_t sessionSteps        = 0;
        bool     selectorOpen   = false;
        int32_t  selectorTenths = 0;
        uint8_t  cardId         = 0; // CardId of m_cards.current() (spec 4.8)
        // Seconds-of-day from the clock card's local time (hh*3600+mm*60+ss),
        // or -1 when TimeService isn't valid yet — makes drawClock() redraw
        // once a second while the second hand is moving, and once when
        // validity itself flips (--:-- <-> a real face).
        int32_t  clockSec       = -1;

        // Flights card (spec 4.9): the fields drawFlights() actually reads.
        // flightHash is a 16-bit FNV-1a hash of the current aircraft's
        // callsign plus coarsened altFt/gsKt (see buildFrameKey()) — cheap
        // stand-in for hashing the whole Aircraft struct field-by-field, and
        // catches an in-place snapshot update (same idx/count, aircraft data
        // moved) that idx/count alone would miss.
        uint8_t  flightIdx      = 0;
        uint8_t  flightCount    = 0;
        bool     flightStale    = false;
        bool     flightOffline  = false;
        uint16_t flightHash     = 0;

        // Lights cards (Plan 6): lightHash is a 16-bit FNV-1a over
        // everything drawLight() reads off the *visible* card (key, valid,
        // available, on, brightnessPct, kelvin, hue, engaged, settling,
        // supportsColor) — same idiom as flightHash above; 0 while no light
        // card is showing. lightMqttUp gates the "waiting for HA" state,
        // which netStatus already covers, but is cheap and keeps the
        // dependency explicit.
        uint16_t lightHash      = 0;
        bool     lightMqttUp    = false;

        bool operator==(const FrameKey& o) const
        {
            return status == o.status && screen == o.screen && paused == o.paused &&
                   durationSec == o.durationSec && speedTenths == o.speedTenths &&
                   distanceCenti == o.distanceCenti && steps == o.steps &&
                   targetTenths == o.targetTenths && pending == o.pending &&
                   overlayActive == o.overlayActive && netStatus == o.netStatus &&
                   holdUnits == o.holdUnits && pulsePhase == o.pulsePhase &&
                   connectAttempts == o.connectAttempts &&
                   sessionDurationSec == o.sessionDurationSec &&
                   sessionDistanceCenti == o.sessionDistanceCenti &&
                   sessionSteps == o.sessionSteps &&
                   selectorOpen == o.selectorOpen && selectorTenths == o.selectorTenths &&
                   cardId == o.cardId && clockSec == o.clockSec &&
                   flightIdx == o.flightIdx && flightCount == o.flightCount &&
                   flightStale == o.flightStale && flightOffline == o.flightOffline &&
                   flightHash == o.flightHash &&
                   lightHash == o.lightHash && lightMqttUp == o.lightMqttUp;
        }
    };
    FrameKey buildFrameKey(uint32_t nowMs) const;
    bool isPausedState() const;

    // render() runs at most every kRenderIntervalMs (tick()'s own throttle,
    // <= 20 Hz) but only actually redraws+pushes when the FrameKey changed or
    // kFrameElapsedMs has passed since the last real draw (so the 1 Hz PAUSED
    // pulse still animates while nothing else is changing).
    static constexpr uint32_t kFrameElapsedMs = 250;
    FrameKey m_lastFrame;
    bool     m_haveLastFrame  = false;
    uint32_t m_lastFrameDrawMs = 0;

    TreadmillController& m_controller;
    const TimeService& m_time;
    DialInput m_input;
    SpeedSelector m_selector;
    // Which desk-mode card is showing while the belt is idle (spec 4.8);
    // boots on CLOCK and is driven by knob detents in handleInput().
    CardRing m_cards;
    DialInput::Backlight m_lastBacklight = DialInput::Backlight::FULL;
    float m_holdProgress = 0.0f; // last tick's hold-in-progress fraction [0,1]; drives the long-press ring

    // Flights card (spec 4.9). m_flights is NetTask's own FlightsService
    // member, bound at construction (see the DialUi() comment above) — every
    // call here is loop-task-safe by FlightsService's own contract.
    FlightsService& m_flights;
    static constexpr uint32_t kFlightsSnapIntervalMs = 250;
    FlightsModel::FlightsSnapshot m_flightsSnap{};
    bool m_haveFlightsSnap = false;
    uint32_t m_lastFlightsSnapMs = 0;
    uint8_t m_flightIdx = 0; // index into m_flightsSnap.ac[], cycled by tap
    // Minor: last IATA passed to m_flights.setWantedLogo(), so tickFlights()
    // only calls it again when the wanted airline actually changes rather
    // than every call while screen == FLIGHTS.
    char m_lastWantedIata[3] = {0};

    // Lights cards (Plan 6). m_lights is NetTask's own LightsService member
    // (loop-task-safe through snapshot() alone); m_net is only ever used for
    // enqueuePublish() of a LIGHT_CMD item. m_lightCards is indexed by
    // LightsModel::LightKey: [OFFICE] has no colour button, [LAMP] does.
    NetTask& m_net;
    LightsService& m_lights;
    LightCardState m_lightCards[2];
    static_assert(static_cast<uint8_t>(LightsModel::LightKey::COUNT) == 2,
                  "m_lightCards is indexed by LightKey and sized for exactly two lights");
    static constexpr uint32_t kLightsSnapIntervalMs = 250;
    // The fresh LightsSnapshot itself is written once by tickLights() (at
    // most every kLightsSnapIntervalMs) and consumed immediately, in the same
    // call, to sync() both cards — nothing else reads it, so it's a local in
    // tickLights() rather than a member; only the "do we have one yet / when
    // did we last pull one" bookkeeping needs to persist across calls.
    bool m_haveLightsSnap = false;
    uint32_t m_lastLightsSnapMs = 0;

    // Airline logos are decoded from LittleFS straight onto the display after
    // the frame is pushed (the palette canvas would posterise them). Set by
    // drawFlights() for the current aircraft, consumed by render().
    char m_logoToDraw[3] = {0};
    // IATA of the logo currently painted on the display (empty = none). The
    // logo is only re-decoded when this differs from m_logoToDraw; meanwhile
    // frames are pushed with the logo rectangle transparent so it survives.
    char m_logoOnScreen[3] = {0};

    // I4: small ring of IATA codes whose drawPngFile() decode has already
    // failed once this session (a corrupt/unsupported PNG that
    // FlightsService's own PNG-signature check let through, or a
    // LovyanGFX decode failure) — checked before re-attempting a decode so
    // a bad file doesn't get re-parsed on every card redraw. 4 entries is
    // generous for a single session's worth of distinct airlines seen on
    // the card; round-robin overwrite if that's ever exceeded. Cleared only
    // by a reboot — there's no signal here that a re-cached logo file (a
    // fresh download replacing a corrupt one) would even be different, so
    // this deliberately doesn't try to self-heal mid-session.
    static constexpr uint8_t kLogoFailedSize = 4;
    char m_logoFailed[kLogoFailedSize][3] = {{0}};
    uint8_t m_logoFailedCount = 0;
    uint8_t m_logoFailedNext  = 0;
    bool isLogoDecodeFailed(const char* iata) const;
    void markLogoDecodeFailed(const char* iata);
    // Airlines whose cached logo failed once and was re-downloaded; a second
    // failure goes to m_logoFailed (permanent for the session).
    char m_logoRetried[kLogoFailedSize][3] = {{0}};
    uint8_t m_logoRetriedCount = 0;
    uint8_t m_logoRetriedNext  = 0;
    bool isLogoRetried(const char* iata) const;
    void markLogoRetried(const char* iata);

#if DIAL_SOUND
    bool m_secondBeepPending = false;
    uint32_t m_secondBeepDueMs = 0;
#endif

    // Default-constructed (no parent bound at construction time): `dialUi` is
    // a global in main.cpp, and M5Dial.Display is a reference bound inside a
    // library global's own constructor — global init order across
    // translation units is unspecified, so binding to it here would be a
    // static-init-order hazard. The sprite is pushed to an explicit
    // destination (&M5Dial.Display) in render() instead.
    M5Canvas m_canvas;
    bool m_useCanvas = false;
    uint32_t m_lastRenderMs = 0;

    // Last state pushed by the controller. Defaults mirror a fresh boot:
    // disconnected belt, no network yet — so the very first render (before
    // any observer callback fires) already reads correctly.
    TreadMillData m_snapshot;
    float m_targetSpeedMph = 0.0f;
    bool m_targetPending = false;
    NetStatus m_netStatus = NetStatus::WIFI_DOWN;

    // Deadline for the centre speed overlay: nowMs + DIAL_SPEED_OVERLAY_MS,
    // (re)armed each time a nudge lands a fresh onTargetSpeed(pending=true).
    // onTargetSpeed() itself isn't handed nowMs (the ISnapshotObserver
    // interface is shared with mqttview and the host tests), so the deadline
    // is captured at the one call site that can trigger pending=true —
    // handleInput()'s nudgeSpeed() call — right after the synchronous
    // notifyTarget() callback has run. 0 means "never armed"; compared via
    // signed subtraction so it stays correct across a millis() wrap.
    uint32_t m_speedOverlayUntilMs = 0;
};

#endif // HAS_DIAL_UI
