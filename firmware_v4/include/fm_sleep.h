/**
 * FloodMesh V4 - light sleep with radio and keypad wake (docs/architecture.md §13.1).
 *
 * There is no shared listening window. A unit on battery puts the ESP32 into
 * light sleep whenever it has nothing to do, while the SX1262 keeps
 * receiving. A complete packet raises DIO1 (GPIO 14), a key press pulls a
 * keypad column low, and either one wakes the ESP32 within about a
 * millisecond. RAM, the screen contents and every timer survive: light sleep
 * is not a reboot, which is why V4 uses it rather than deep sleep.
 *
 * Each sleep is capped (FM_SLEEP_MAX_MS) as a safety net: if a wake source
 * were ever missed, the loop still runs, scans the keypad and reads the radio
 * at least that often. A wake costs a millisecond or two, so a 1 s cap adds
 * little to the average current.
 *
 * The receiver itself still listens continuously (~5-6 mA) unless the network
 * is built with FM_RX_DUTY_CYCLE and a long preamble (fm_radio.h). Those
 * numbers, and the board's own sleep current, still have to be measured.
 */
#pragma once
#include <Arduino.h>

#ifndef FM_SLEEP_MAX_MS
#define FM_SLEEP_MAX_MS 1000       // longest single sleep; see the safety-net note
#endif

enum FmWake : uint8_t {
  FM_WAKE_NONE = 0,    // did not sleep: a key is down or a packet is waiting
  FM_WAKE_RADIO,
  FM_WAKE_KEY,
  FM_WAKE_TIMER,
  FM_WAKE_OTHER,
};

/** Sleep for at most maxMs (capped at FM_SLEEP_MAX_MS). Returns what woke it. */
FmWake fmSleepLight(uint32_t maxMs);

/** Time spent asleep since boot, and how many sleeps, for the status screen. */
uint32_t fmSleepTotalMs();
uint32_t fmSleepCount();
