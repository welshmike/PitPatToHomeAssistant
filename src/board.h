#pragma once
// Per-target pins and features. Selected by -DBOARD_DEVKIT or -DBOARD_M5DIAL.
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
