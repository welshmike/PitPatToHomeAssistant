#pragma once
// Per-target pins and features. Selected by -DBOARD_DEVKIT or -DBOARD_M5DIAL.
//
// config.h first (Plan 17, spec 4.19): HAS_CALENDAR below is decided from
// CALENDAR_URL, which only config.h defines. Every device build already
// pulls config.h in (FlightsService.h, TimeService.h, main.cpp all include
// it directly, same as MQTT_SERVER etc.), so this is just moving that
// dependency one level up to where HAS_CALENDAR needs it. Native (host)
// builds never reach this include today (no file in the native
// build_src_filter — platformio.ini env:native — includes board.h), but the
// guard makes that structural rather than incidental: config.h is gitignored
// and not guaranteed to exist on a fresh checkout, so a host build must never
// be able to require it just because some future native test pulls board.h in.
#if !defined(NATIVE_TEST)
  #include "config.h"
#endif

#if defined(BOARD_M5DIAL)
  #define BOARD_NAME        "m5dial"
  #define HAS_STATUS_LED    0
  #define HAS_DIAL_UI       1
  #define OTA_HOSTNAME      "pacekeeper-dial"
#elif defined(BOARD_DEVKIT) || !defined(NATIVE_TEST)
  #define BOARD_NAME        "devkit"
  #define HAS_STATUS_LED    1
  #define LED_BLE_PIN       2      // built-in blue LED, active-high
  #define HAS_DIAL_UI       0
#else
  #define BOARD_NAME        "native"
  #define HAS_STATUS_LED    0
  #define HAS_DIAL_UI       0
#endif

// Calendar card (Plan 17, spec 4.19): compiled in whenever config.h defines
// CALENDAR_URL (the Apps Script feed endpoint), independent of board type.
#if defined(CALENDAR_URL)
  #define HAS_CALENDAR 1
#else
  #define HAS_CALENDAR 0
#endif
