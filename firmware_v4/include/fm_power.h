/**
 * FloodMesh V4 - is this unit on external power? (docs/architecture.md §13.2)
 *
 * Two methods, chosen at boot:
 *
 * 1. Power-sense divider (exact). The Heltec 5V pin is USB VBUS: 5 V on USB,
 *    solar or a power bank, 0 V on battery. With
 *        5V pin -> 100 k -> GPIO 3 -> 100 k -> GND
 *    (the same wiring as prototype v2) GPIO 3 reads ~2.5 V on external power
 *    and ~0 V on battery. fmPowerBegin() probes for the divider, so one build
 *    runs on units with and without it.
 *
 * 2. Battery-voltage trend (fallback, approximate). Plugging in makes the cell
 *    voltage jump up and keep rising; unplugging makes it drop and keep
 *    falling. A flat voltage says nothing - a sleeping unit on battery also
 *    looks flat for hours - so the verdict only changes on a clear rise or
 *    fall, and otherwise keeps the last one. The first reading after boot
 *    starts it: 4.15 V or more (a charger holds ~4.2 V) or no cell at all
 *    counts as external. Known blind spot: a unit plugged in with an already
 *    full cell shows no rise, so it stays "battery" until overridden.
 */
#pragma once
#include <Arduino.h>

#ifndef FM_PWR_SENSE_PIN
#define FM_PWR_SENSE_PIN 3        // -1 = never look for the divider
#endif
#ifndef FM_PWR_VBUS_MV
#define FM_PWR_VBUS_MV 4000       // VBUS above this = external power (divider x2)
#endif
#ifndef FM_PWR_ON_MV
#define FM_PWR_ON_MV 4150         // trend method: first reading at or above = external
#endif
#ifndef FM_PWR_NO_BATT_MV
#define FM_PWR_NO_BATT_MV 2500    // below this there is no cell: USB must be powering us
#endif
#ifndef FM_PWR_STEP_MV
#define FM_PWR_STEP_MV 40         // one reading to the next: a plug-in jump / unplug drop
#endif
#ifndef FM_PWR_RISE_MV
#define FM_PWR_RISE_MV 30         // over the trend window: charging
#endif
#ifndef FM_PWR_FALL_MV
#define FM_PWR_FALL_MV 20         // over the trend window: on battery
#endif
#ifndef FM_PWR_TREND_MIN
#define FM_PWR_TREND_MIN 10       // trend window, in minutes
#endif

/** Probe for the divider. Call once from setup(), before the first update. */
void fmPowerBegin();

/**
 * Feed one battery reading (every ~10 s). Returns the automatic verdict:
 * true = external power. Reads VBUS itself when the divider is fitted.
 */
bool fmPowerUpdate(uint32_t battMv, uint32_t nowMs);

bool        fmPowerExternal();      // last verdict
bool        fmPowerSenseFitted();   // divider found at boot
uint32_t    fmPowerVbusMv();        // last VBUS reading; 0 without the divider
const char *fmPowerMethod();        // "VBUS" or "trend"
