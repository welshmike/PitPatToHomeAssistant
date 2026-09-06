# Plan 9: Flights radar empty state

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Checkbox steps.

**Goal:** spec §4.13. Replace the Flights card's `no aircraft nearby` text with an animated radar sweep and the word `Searching`.

**Verification:** `pio test -e native`, `pio run -e devkit-usb`, `pio run -e dial-usb`; no new `src/` warnings. `pio` is at `~/Library/Python/3.9/bin/pio`. Never stage `src/config.h`.

**Constraints:** colours only via `DialTheme`/`Col` (palette is full: reuse `DIM_DIM`, `DIM`, `NET_ON`, `SPEED_DIM`); no `delay`, no per-frame heap, no `String`; wrap-safe time; the FrameKey must make the card redraw at 10 Hz only while the radar is visible; `FlightsService`, `NetTask`, `TreadmillHandler` untouched.

---

## Task 1: Radar empty state

**Files:** `src/DialFlightsView.h/.cpp` (empty-state branch → `drawRadarSweep(gfx, theme, phase)`), `src/DialUi.h/.cpp` (`FrameKey.radarPhase`, computed in `buildFrameKey()` as `(nowMs / 100) % 30` when `currentScreen == FLIGHTS && count == 0 && !offline`, else 0; pass the phase into the Flights draw call), `README.md` (one sentence in the Flights section), `doc/AUDIT_2026-09-03.md` (Phase G/H addendum item: radar sweeps at ~3 s per revolution on an empty card; stops redrawing when an aircraft appears or the card is left).

- [ ] Geometry per spec §4.13: rings r 36/72/108 `drawCircle` `DIM_DIM`; crosshair `drawLine` `DIM_DIM`; centre `fillCircle` r 4 `NET_ON`; sweep angle `phase * 12°` measured clockwise from 12 o'clock; trailing wedges via `fillArc(120,120, 108, 0, a-12, a, SPEED_DIM)` and `(a-24, a-12, DIM_DIM)` drawn BEFORE the rings/crosshair so the lines stay visible; the sweep line via `thickLine`/`drawLine` from the centre to the rim in `NET_ON` on top; `Searching` `Font2` `DIM` at (120,196). Keep the stale-dot behaviour.
- [ ] `FrameKey`: add `uint8_t radarPhase` and include it in `operator==`. Confirm by reading `render()` that a FrameKey change is what triggers a redraw (it is) and that nothing else in the key churns.
- [ ] Verify by inspection that `offline` still wins (draws `waiting for HA`), and that a populated list draws exactly as before.
- [ ] Commit: `Flights card: radar sweep 'Searching' empty state`.
