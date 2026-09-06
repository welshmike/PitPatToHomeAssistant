# Plan 16: Colour page second, centre-disc tap leaves — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Lamp pages run Brightness → Colour → Kelvin, and a tap on the centre disc of the Colour page returns to Brightness.

**Architecture:** `LightCardState` keeps the `Page` enum but walks a per-card page list (`pageAt(i)`, `pageIndex()`, private `m_pageIdx`), so Office (no colour) is `{BRIGHT, KELVIN}` and Lamp is `{BRIGHT, COLOUR, KELVIN}`. `LightLayout::hitCentreDisc(x, y)` (radius 34) is the new tap target; `DialUi::handleLightTap()` calls `card.resetPage()` on it. Docs updated. Spec: §4.18 amendment "second hardware try" in `docs/superpowers/specs/2026-09-03-m5dial-migration-design.md`.

**Tech Stack:** PlatformIO (`pio` at `~/Library/Python/3.9/bin/pio`), envs `native` (320 tests now) and `dial-ota`.

## Global Constraints

- Lamp page sequence `{BRIGHT, COLOUR, KELVIN}`; Office `{BRIGHT, KELVIN}`. Every arrival on a card, switch-on and the 10 s idle still land on Brightness. Swipes do not wrap.
- Centre-disc hit radius 34 (`kCentreDiscHitR`), centred (120,120). A tap there on the Colour page → `resetPage()` + accept beep; on other pages nothing new (the disc is not drawn there).
- Ring scrub claim (annulus 70–120) unchanged; the centre disc tap is only reachable through the unclaimed tap path.
- Payload unchanged. Never stage `src/config.h`. Commit message ends with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`. Build `dial-ota` must succeed; do not upload.

---

### Task 1: Page list, centre-disc hit test, tap handling, tests, docs

**Files:**
- Modify: `src/LightCardState.h` (~line 34 enum, ~58 `pageCount()`, ~60 `page()`, ~201 `m_page`), `src/LightCardState.cpp` (`resetPage()` ~51, `swipe()` ~61, every `m_page` use)
- Modify: `src/LightLayout.h` (add `kCentreDiscHitR = 34` and `hitCentreDisc(int,int)` next to `hitPowerGlyph`)
- Modify: `src/DialLightsView.cpp:107` (page dot highlight uses `card.pageIndex()`), `src/DialUi.cpp` (`handleLightTap()` Colour-page comment block ~line 627: add the centre-disc branch)
- Modify: `README.md` (Lamp pages line and Colour bullet), `doc/AUDIT_2026-09-03.md` (Phase K), spec §4.12 "page dots" bullet (~line 296: `Lamp: Brightness → Kelvin → Colour`) gets `*(order changed by §4.18 amendment: Brightness → Colour → Kelvin)*`
- Test: `test/test_light_card_state/test_main.cpp`

- [ ] **Step 1: Failing tests**

In `test/test_light_card_state/test_main.cpp`:
- `onColourPageCard()` helper: ONE `swipe(1, 1000)` now reaches COLOUR. Add `static LightCardState onKelvinPageCard()` = `onCard()` + two swipes, and use it in every test that previously did one swipe to reach KELVIN (kelvin detent tests, `test_settle_kelvinPageEmitsTempCommand`, `test_settle_survivesAPageSwipeAndStillSendsTheEditedField` — there, swipe twice to Kelvin, detent, then swipe back once to Colour: the TEMP command must still go out, and the Colour page's `colourLive()` stays false until a colour edit). Grep for every `card.swipe(1, ` and re-check each test's intent against the new order.
- Replace `test_swipe_forwardWalksPagesAndClampsAtColour` with `test_swipe_forwardWalksBrightColourKelvinAndClampsAtKelvin` (BRIGHT → COLOUR → KELVIN → KELVIN), asserting `pageIndex()` 0,1,2,2 alongside `page()`.
- Add `test_pageAt_lampAndOfficeSequences`: lamp `pageAt(0..2)` = BRIGHT, COLOUR, KELVIN; office `pageAt(0..1)` = BRIGHT, KELVIN; `pageAt(99)` clamps to the last page.
- Add `test_swipe_officeSkipsColour`: office one swipe → KELVIN with `pageIndex() == 1`.
- Add `test_hitCentreDisc_withinAndOutside`: `TEST_ASSERT_TRUE(LightLayout::hitCentreDisc(120, 120))`, `(120, 120 - 34)` true, `(120, 120 - 35)` false, `(120 + 99, 120)` false; and `TEST_ASSERT_EQUAL_INT(34, LightLayout::kCentreDiscHitR)`; and a sweep asserting `hitCentreDisc` and `hitHueRing` never both claim a point (step 4 px over the face).
- Add `test_resetPage_fromColourReturnsToBrightnessAndEndsScrub`: `onColourPageCard()`, `scrub(90, 1000)`, `resetPage()` → page BRIGHT, `scrubbing()` false, pending settle left alone (`settling()` still true — `resetPage` never drops an edit).
- Update `RUN_TEST` list.

- [ ] **Step 2: Run to confirm failures**: `~/Library/Python/3.9/bin/pio test -e native -f test_light_card_state 2>&1 | tail -8`.

- [ ] **Step 3: `LightCardState`**

Header: keep `enum class Page : uint8_t { BRIGHT, KELVIN, COLOUR };` (values are stable for `FrameKey::lightPage`). Replace `Page m_page = Page::BRIGHT;` with `uint8_t m_pageIdx = 0; // index into the card's page sequence (pageAt)`. Add:
```cpp
    // The card's page sequence (spec 4.18 amendment): Lamp is Brightness ->
    // Colour -> Kelvin, Office (no colour) is Brightness -> Kelvin. Indices
    // past the end clamp to the last page.
    Page pageAt(uint8_t i) const;
    uint8_t pageIndex() const { return m_pageIdx; }
    Page page() const { return pageAt(m_pageIdx); }
```
Implementation:
```cpp
LightCardState::Page LightCardState::pageAt(uint8_t i) const
{
    static constexpr Page kLamp[3]   = {Page::BRIGHT, Page::COLOUR, Page::KELVIN};
    static constexpr Page kOffice[2] = {Page::BRIGHT, Page::KELVIN};
    const uint8_t last = pageCount() - 1;
    if (i > last) i = last;
    return m_hasColour ? kLamp[i] : kOffice[i];
}
```
`resetPage()`: `m_pageIdx = 0;` (also `m_scrubActive = false;` — already there from Plan 15). `swipe()`: `m_pageIdx = clampInt(m_pageIdx + dir, 0, pageCount() - 1)`. Every other `m_page` read (`detents()` switch, `selectHue()`, `scrub()`, `tick()` idle check) → `page()`. Update the header comments that describe the order ("Lamp: Brightness → Kelvin → Colour" wherever it appears in `LightCardState.h`).

- [ ] **Step 4: `LightLayout.h`**: after `hitPowerGlyph`, add
```cpp
// Colour page: the centre disc is a tap target that leaves the page. Hit
// radius a little larger than the disc (kCentreDiscR 30) and well inside the
// ring's touch annulus (kHueHitInnerR 70), so the two can never both claim
// a point.
constexpr int kCentreDiscHitR = 34;
inline bool hitCentreDisc(int x, int y)
{
    const int dx = x - kCentreX;
    const int dy = y - kCentreY;
    return dx * dx + dy * dy <= kCentreDiscHitR * kCentreDiscHitR;
}
```

- [ ] **Step 5: `DialLightsView.cpp:107`**: `if (i == static_cast<int>(card.pageIndex()))`. **`DialUi.cpp` `handleLightTap()`**: in the Colour-page section (where the comment says the ring is handled by the scrub path), add:
```cpp
    // Colour page: a tap on the centre disc leaves the page (spec 4.18
    // amendment). The ring itself is handled by the scrub path in
    // handleInput(); the disc sits inside the ring's touch annulus, so a tap
    // here is never claimed and always arrives as ev.tap.
    if (card.page() == LightCardState::Page::COLOUR && LightLayout::hitCentreDisc(x, y))
    {
        card.resetPage();
        playAcceptBeep(true);
        return;
    }
```
Check `resetPage()` is public (it is used by `resetLightPages()`).

- [ ] **Step 6: Tests and build**: `~/Library/Python/3.9/bin/pio test -e native 2>&1 | tail -2` (all pass); `~/Library/Python/3.9/bin/pio run -e dial-ota 2>&1 | grep -E "error|SUCCESS|FAILED" | tail -2`.

- [ ] **Step 7: Docs**: README — the Lamp page list reads Brightness → Colour → Kelvin; the Colour bullet gains "Tap the centre disc to go back to Brightness." AUDIT Phase K — add `- [ ] Swipe once from Brightness: Colour page (dots show the middle dot lit); swipe again: Kelvin. Office: one swipe reaches Kelvin.` and `- [ ] On the Colour page tap the centre disc: beep, back to Brightness, no lamp command sent.` Spec §4.12 page-dots bullet gets the superseded note quoted in Files above.

- [ ] **Step 8: Commit** (`git add` the touched src/test/doc files only):
`Lights: Colour page second; tap the centre disc to leave it (spec 4.18 amendment)` + Co-Authored-By line.
