# Plan 15: Hue ring — 15° detents and finger scrub — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Colour page's knob step perceptible (15° per detent) and let the user drag a finger around the hue ring to scrub the hue live.

**Architecture:** `DialInput` (pure) gains a per-tick finger report (`touchHeld`, `touchX/Y`, `touchStartX/Y`) and a `claimTouch()` that mutes swipe/long-press/tap for the current gesture. `DialUi` uses it on the Lamp Colour page: a touch that starts in the ring's annulus is claimed and, every tick it is held, `LightCardState::selectHue(LightLayout::hueAt(x, y))` re-arms the existing 300 ms settle. `HUE_STEP` becomes 15. No change to `LightCardState`'s command flow or to HA.

**Tech Stack:** PlatformIO (`pio` at `~/Library/Python/3.9/bin/pio`), envs `native` (Unity) and `dial-ota`. Spec: `docs/superpowers/specs/2026-09-03-m5dial-migration-design.md` §4.18 and its 2026-09-06 amendment.

## Global Constraints

- `HUE_STEP = 15` (24 detents per revolution, wrapping). Settle 300 ms, confirm hold, `PAGE_IDLE_MS` 10 s unchanged.
- Scrub: claimed only when the touch-down point satisfies `LightLayout::hitHueRing(startX, startY)` on the Lamp card's Colour page while `lightCardLive()`. While claimed and held, hue = `LightLayout::hueAt(touchX, touchY)` regardless of radius. A claimed gesture yields no `swipe`, no `longPress`, `holdProgress == 0`, and no `tap` on release. Unclaimed gestures are unchanged.
- `DialEvents` additions: `bool touchHeld` (finger down this tick and the gesture is not wake-swallowed), `int touchX, touchY` (this tick's position, valid when `touchHeld`), `int touchStartX, touchStartY` (the gesture's touch-down point, valid when `touchHeld`).
- Payload unchanged. No new heap.
- Never stage `src/config.h`. Commit messages end with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`. Tests: `~/Library/Python/3.9/bin/pio test -e native` (301 now). Build: `~/Library/Python/3.9/bin/pio run -e dial-ota`; do not upload.

---

### Task 1: `DialInput` finger report and `claimTouch()`; `HUE_STEP` 15

**Files:**
- Modify: `src/DialInput.h` (struct `DialEvents`, class API, private state)
- Modify: `src/DialInput.cpp` (`tick()` touch block ~line 100–160; new `claimTouch()`)
- Modify: `src/LightCardState.h:51` (`HUE_STEP`)
- Test: `test/test_dial_input/test_main.cpp`, `test/test_light_card_state/test_main.cpp`

**Interfaces:**
- Produces: `DialEvents::touchHeld/touchX/touchY/touchStartX/touchStartY`; `void DialInput::claimTouch()`; `LightCardState::HUE_STEP == 15`.

- [ ] **Step 1: Failing tests — `test/test_dial_input/test_main.cpp`**

Add before the `// Wraparound-safe time maths` section:

```cpp
// ---------------------------------------------------------------------------
// Finger report + claimTouch (hue-ring scrub, spec 4.18 amendment)
// ---------------------------------------------------------------------------

static void test_touchHeld_reportsPositionEveryTickAndStartPoint(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    DialEvents eDown = input.tick(0, true, 120, 20, false, false, false, t0);
    TEST_ASSERT_TRUE(eDown.touchHeld);
    TEST_ASSERT_EQUAL_INT(120, eDown.touchX);
    TEST_ASSERT_EQUAL_INT(20, eDown.touchY);
    TEST_ASSERT_EQUAL_INT(120, eDown.touchStartX);
    TEST_ASSERT_EQUAL_INT(20, eDown.touchStartY);

    DialEvents eMove = input.tick(0, true, 200, 60, false, false, false, t0 + 100);
    TEST_ASSERT_TRUE(eMove.touchHeld);
    TEST_ASSERT_EQUAL_INT(200, eMove.touchX);
    TEST_ASSERT_EQUAL_INT(60, eMove.touchY);
    TEST_ASSERT_EQUAL_INT(120, eMove.touchStartX); // start point is sticky
    TEST_ASSERT_EQUAL_INT(20, eMove.touchStartY);

    DialEvents eUp = input.tick(0, false, 0, 0, false, false, false, t0 + 200);
    TEST_ASSERT_FALSE(eUp.touchHeld);
}

static void test_touchHeld_falseWithNoTouch(void)
{
    DialInput input;
    DialEvents e = input.tick(0, false, 0, 0, false, false, false, 1000);
    TEST_ASSERT_FALSE(e.touchHeld);
}

// A touch that wakes a dimmed backlight is swallowed: no finger report for it.
static void test_touchHeld_falseWhileWakeSwallowed(void)
{
    DialInput input;
    uint32_t t0 = 1000;
    input.tick(0, false, 0, 0, false, false, false, t0);
    DialEvents eDim = input.tick(0, false, 0, 0, false, false, false, t0 + DialInput::DIM_AFTER_MS + 1);
    TEST_ASSERT_TRUE(DialInput::Backlight::DIM == input.backlight());
    (void)eDim;
    DialEvents eDown = input.tick(0, true, 100, 100, false, false, false, t0 + DialInput::DIM_AFTER_MS + 10);
    TEST_ASSERT_TRUE(eDown.wake);
    TEST_ASSERT_FALSE(eDown.touchHeld);
    DialEvents eHold = input.tick(0, true, 100, 100, false, false, false, t0 + DialInput::DIM_AFTER_MS + 200);
    TEST_ASSERT_FALSE(eHold.touchHeld);
}

// claimTouch(): the gesture keeps reporting the finger but produces no swipe,
// no long press (holdProgress 0) and no tap on release.
static void test_claimTouch_mutesSwipeLongPressAndTap(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    DialEvents eDown = input.tick(0, true, 50, 120, false, false, false, t0);
    TEST_ASSERT_TRUE(eDown.touchHeld);
    input.claimTouch();

    // 60 px to the right: would be a swipe if unclaimed.
    DialEvents eMove = input.tick(0, true, 110, 120, false, false, false, t0 + 100);
    TEST_ASSERT_TRUE(eMove.touchHeld);
    TEST_ASSERT_EQUAL_INT(110, eMove.touchX);
    TEST_ASSERT_EQUAL_INT(0, eMove.swipe);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, eMove.holdProgress);

    // Held well past HOLD_MS without moving: would be a long press if unclaimed.
    DialEvents eHeld = input.tick(0, true, 110, 120, false, false, false, t0 + DialInput::HOLD_MS + 200);
    TEST_ASSERT_FALSE(eHeld.longPress);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, eHeld.holdProgress);
    TEST_ASSERT_TRUE(eHeld.touchHeld);

    DialEvents eUp = input.tick(0, false, 0, 0, false, false, false, t0 + DialInput::HOLD_MS + 300);
    TEST_ASSERT_FALSE(eUp.tap);
    TEST_ASSERT_FALSE(eUp.touchHeld);
}

// A quick claimed touch is not a tap either: the claimer already acted on it.
static void test_claimTouch_quickReleaseIsNotATap(void)
{
    DialInput input;
    uint32_t t0 = 1000;
    input.tick(0, true, 50, 120, false, false, false, t0);
    input.claimTouch();
    DialEvents eUp = input.tick(0, false, 0, 0, false, false, false, t0 + 100);
    TEST_ASSERT_FALSE(eUp.tap);
}

// The claim is per gesture: the next touch behaves normally again.
static void test_claimTouch_clearsOnRelease(void)
{
    DialInput input;
    uint32_t t0 = 1000;
    input.tick(0, true, 50, 120, false, false, false, t0);
    input.claimTouch();
    input.tick(0, false, 0, 0, false, false, false, t0 + 100);

    input.tick(0, true, 60, 60, false, false, false, t0 + 500);
    DialEvents eUp = input.tick(0, false, 0, 0, false, false, false, t0 + 600);
    TEST_ASSERT_TRUE(eUp.tap);
    TEST_ASSERT_EQUAL_INT(60, eUp.tapX);
}

// claimTouch() with no gesture in progress is a harmless no-op.
static void test_claimTouch_withoutTouch_isNoOp(void)
{
    DialInput input;
    input.claimTouch();
    input.tick(0, true, 60, 60, false, false, false, 1000);
    DialEvents eUp = input.tick(0, false, 0, 0, false, false, false, 1100);
    TEST_ASSERT_TRUE(eUp.tap);
}
```

Register all seven in `main()` before `RUN_TEST(test_time_wraparoundSafe);`.

- [ ] **Step 2: Failing tests — `test/test_light_card_state/test_main.cpp`**

Update the three Colour-page detent tests and the settle test for `HUE_STEP = 15`:
- `test_detents_colourPage_stepsFiveDegreesPerDetent` → rename `test_detents_colourPage_stepsFifteenDegreesPerDetent`; expect `15.0f` after one detent and `60.0f` after `detents(3, 1050)`.
- `test_detents_colourPage_wrapsBothWays`: `detents(-1)` from 0 → `345.0f`; `detents(1)` back → `0.0f`.
- `test_detents_colourPage_fromTempMode_startsAtHaHue`: from 250 → `265.0f` (both asserts).
- `test_settle_colourPageEmitsHueCommandWithRoundedHueAndFullSaturation`: `detents(5)` from 0 → expect `cmd.hue == 75.0f`.
- `test_editHue_isThePendingOneWhileSettling`: `detents(2)` → expect `30.0f`.
Add `TEST_ASSERT_EQUAL_INT(15, LightCardState::HUE_STEP);` to `test_layout_pageConstants` (or a new one-line test) and update the `RUN_TEST` name.

- [ ] **Step 3: Run both suites, confirm they fail**

Run: `~/Library/Python/3.9/bin/pio test -e native -f test_dial_input 2>&1 | tail -5` → compile errors on `touchHeld`/`claimTouch`.
Run: `~/Library/Python/3.9/bin/pio test -e native -f test_light_card_state 2>&1 | tail -8` → the five detent/settle assertions fail (5 vs 15 etc.).

- [ ] **Step 4: `src/DialInput.h`**

Add to `struct DialEvents` after `btnHold`:

```cpp
    // Finger report (spec 4.18 amendment, hue-ring scrub): true on every tick
    // the finger is down and the gesture is not wake-swallowed, with this
    // tick's position and the gesture's touch-down point. Reported for claimed
    // gestures too — that is what a scrubber tracks.
    bool touchHeld = false;
    int touchX = 0;
    int touchY = 0;
    int touchStartX = 0;
    int touchStartY = 0;
```

Add to the public API after `consumeClick`:

```cpp
    // Called by the UI when it takes over the gesture in progress (a touch that
    // began on the hue ring). From then until release the gesture still
    // reports touchHeld/touchX/touchY but produces no swipe, no long press
    // (holdProgress stays 0) and no tap. No-op when no gesture is in progress;
    // cleared automatically on release.
    void claimTouch();
```

Add private state after `m_swipeFired`: `bool m_touchClaimed = false; // gesture taken over by the UI (claimTouch)`.

- [ ] **Step 5: `src/DialInput.cpp`**

Add:

```cpp
void DialInput::claimTouch()
{
    if (m_touchActive) {
        m_touchClaimed = true;
    }
}
```

In `tick()`, in the wake-gating `if (touchBeginning)` block add `m_touchClaimed = false;`. In the main touch block:
- In `if (!m_touchActive) { ... }` (touch begins) add `m_touchClaimed = false;`.
- Change `} else if (!m_touchSwallowed) {` to `} else if (!m_touchSwallowed && !m_touchClaimed) {` so a claimed gesture produces no swipe/long-press/hold progress (`ev.holdProgress` stays at its default 0).
- After that if/else-if chain but still inside `if (touchDown) { ... }`, add the report:

```cpp
        if (!m_touchSwallowed) {
            ev.touchHeld = true;
            ev.touchX = touchX;
            ev.touchY = touchY;
            ev.touchStartX = m_touchStartX;
            ev.touchStartY = m_touchStartY;
        }
```
- In the release branch, change `if (!m_touchSwallowed) {` (the tap decision) to `if (!m_touchSwallowed && !m_touchClaimed) {` and add `m_touchClaimed = false;` alongside the other resets.

- [ ] **Step 6: `src/LightCardState.h:51`** → `static constexpr int HUE_STEP = 15; // degrees per detent on the Colour page (24 per revolution)`.

- [ ] **Step 7: Run the suites**

Run: `~/Library/Python/3.9/bin/pio test -e native 2>&1 | tail -3` → `309 test cases: 309 succeeded` (301 + 7 + 1; if you added the HUE_STEP assert inside an existing test the count is 308 — fine).

- [ ] **Step 8: Commit**

```bash
git add src/DialInput.h src/DialInput.cpp src/LightCardState.h test/test_dial_input/test_main.cpp test/test_light_card_state/test_main.cpp
git commit -m "DialInput: per-tick finger report and claimTouch(); Colour knob step 15 deg

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: `DialUi` scrub on the Colour page; docs

**Files:**
- Modify: `src/DialUi.cpp` (after `m_holdProgress = ev.holdProgress;` ~line 249 and before `if (ev.tap)` ~line 284; `handleLightTap()` ~line 561–610)
- Modify: `README.md` (~line 286–292), `doc/AUDIT_2026-09-03.md` (Phase K), `docs/superpowers/specs/2026-09-03-m5dial-migration-design.md` (§4.18 Knob bullet: change "5°" to "15°" and "72 detents" to "24 detents" so the bullet matches the amendment)

**Interfaces:**
- Consumes (Task 1): `ev.touchHeld/touchX/touchY/touchStartX/touchStartY`, `m_input.claimTouch()`; existing `LightLayout::hitHueRing`, `LightLayout::hueAt`, `LightCardState::selectHue`, `editHue`, `lightCardLive(screen)`, `lightCardFor(screen)`.

- [ ] **Step 1: Scrub handling in `DialUi::update()`**

Insert immediately after the `const Screen screen = currentScreen(isPausedState());` line (so `screen` is known) and BEFORE `if (ev.tap)`:

```cpp
    // Hue-ring scrub (spec 4.18 amendment): a touch that began inside the
    // ring's annulus on the Lamp's Colour page is the ring's for the rest of
    // the gesture. Every tick it is held, the hue under the finger becomes the
    // edit hue (angle only — the finger may drift off the band), re-arming
    // the 300 ms settle, so the marker and disc follow live and the lamp gets
    // one command 300 ms after the finger pauses or lifts. claimTouch() mutes
    // the swipe/long-press/tap the same motion would otherwise produce.
    if (ev.touchHeld && screen == Screen::LIGHT_LAMP && lightCardLive(screen))
    {
        LightCardState& card = lightCardFor(screen);
        if (card.page() == LightCardState::Page::COLOUR && card.hasColour() &&
            LightLayout::hitHueRing(ev.touchStartX, ev.touchStartY))
        {
            m_input.claimTouch();
            const float hue = LightLayout::hueAt(ev.touchX, ev.touchY);
            // Skip sub-degree jitter: selectHue() re-arms the settle every call.
            float d = fabsf(hue - card.editHue());
            if (d > 180.0f)
            {
                d = 360.0f - d;
            }
            if (d >= 1.0f || !card.settling())
            {
                card.selectHue(hue, nowMs);
            }
        }
    }
```

Add `#include <math.h>` at the top of `src/DialUi.cpp` if `fabsf` is not already available there.

- [ ] **Step 2: Remove the tap path's ring branch in `handleLightTap()`**

The scrub path now covers a tap on the ring (the touch-down tick already selected the hue and claimed the gesture, so no `tap` event fires). Delete the `if (card.page() == LightCardState::Page::COLOUR && card.hasColour()) { if (LightLayout::hitHueRing(x, y)) { ... } }` block and its comment, replacing them with a two-line comment: `// Colour page: the hue ring is handled by the scrub path in update() (a tap is a one-tick scrub), and the power glyph is not drawn there.` Keep the `card.page() != LightCardState::Page::COLOUR && LightLayout::hitPowerGlyph(x, y)` block as is. Also remove the `playAcceptBeep(true)` that was in the deleted block — the scrub path is silent (a beep on every tick would be noise). Update `handleLightTap`'s doc comment in `src/DialUi.h` if it mentions the ring.

- [ ] **Step 3: Build**

Run: `~/Library/Python/3.9/bin/pio run -e dial-ota 2>&1 | grep -E "error|SUCCESS|FAILED" | tail -3` → SUCCESS.
Run: `~/Library/Python/3.9/bin/pio test -e native 2>&1 | tail -2` → unchanged from Task 1.

- [ ] **Step 4: Docs**

README (Colour bullet): "each knob detent moves 5° (72 detents per revolution, wrapping)" → "each knob detent moves 15° (24 detents per revolution, wrapping), or put a finger on the ring and drag it round: the marker follows live and the lamp gets one command 300 ms after the finger pauses or lifts".
AUDIT Phase K: three-detent line → `hs_color [135,100]` and "marker moves 45° clockwise"; add:
```
- [ ] Put a finger on the ring and drag it a quarter turn: the marker and centre disc track the
      finger with no page swipe and no hold-to-off arc; one `hs_color` command about 0.3 s after the
      finger stops, another after it lifts if it moved again.
- [ ] Drag from the ring inward across the centre disc: the marker keeps following the finger's
      angle (radius does not matter once the ring has the gesture).
```
Spec §4.18 Knob bullet: `±HUE_STEP = 15° per detent, wrapping at 360 (24 detents per revolution)` and the Success bullet's "send 105" → "send 135".

- [ ] **Step 5: Commit**

```bash
git add src/DialUi.cpp src/DialUi.h README.md doc/AUDIT_2026-09-03.md docs/superpowers/specs/2026-09-03-m5dial-migration-design.md
git commit -m "Lights: scrub the hue ring with a finger; docs for 15 deg detents

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```
