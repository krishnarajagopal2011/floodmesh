/**
 * FloodMesh V4 - alarm types and SOS channels.
 *
 * The on-air alarm byte keeps V3's values 0..4, so a V3 SOS (4) arrives here
 * as an SOS on the General channel. V4 adds four more SOS values, one per
 * channel (docs/architecture.md §13.8):
 *
 *   4 SOS General   (trapped / water rising and anything else)
 *   5 SOS Medical
 *   6 SOS Evacuation
 *   7 SOS Hazard
 *   8 SOS Food supply
 *
 * V4 has no civilian presets. The old preset values 0..3 are still decoded,
 * because V3 units in the same test can send them: MEDICAL, WATER and
 * EVACUATE become SOS on Medical, General and Evacuation; SAFE becomes a
 * status text that every unit shows.
 *
 * The channel number is also the digit key that picks it, on the sender's
 * channel screen and on the responder's tab bar.
 */
#pragma once
#include <Arduino.h>

enum FmAlarm : uint8_t {
  FM_ALARM_SAFE = 0,        // legacy preset (V3): shown as a status text
  FM_ALARM_MEDICAL,         // legacy preset (V3): SOS Medical
  FM_ALARM_WATER,           // legacy preset (V3): SOS General
  FM_ALARM_EVACUATE,        // legacy preset (V3): SOS Evacuation
  FM_ALARM_SOS,             // SOS, General channel (same value as V3's SOS)
  FM_ALARM_SOS_MEDICAL,
  FM_ALARM_SOS_EVACUATION,
  FM_ALARM_SOS_HAZARD,
  FM_ALARM_SOS_FOOD,
  FM_ALARM_COUNT,
  FM_ALARM_NONE = 0xFF
};

enum FmSosChannel : uint8_t {
  FM_SOS_GENERAL = 0,
  FM_SOS_MEDICAL,
  FM_SOS_EVACUATION,
  FM_SOS_HAZARD,
  FM_SOS_FOOD,
  FM_SOS_CH_COUNT
};

static_assert(FM_ALARM_SOS + FM_SOS_CH_COUNT == FM_ALARM_COUNT,
              "one SOS alarm value per channel, in channel order");

/** Alarm byte for an SOS on this channel. */
inline uint8_t fmSosAlarmType(uint8_t ch) {
  return (uint8_t)(FM_ALARM_SOS + (ch < FM_SOS_CH_COUNT ? ch : (uint8_t)FM_SOS_GENERAL));
}

/**
 * True if this alarm byte is an SOS, legacy presets included; *ch gets its
 * channel. SAFE and unknown values return false.
 */
inline bool fmAlarmSosChannel(uint8_t a, uint8_t *ch) {
  uint8_t c;
  if (a >= FM_ALARM_SOS && a < FM_ALARM_COUNT) c = (uint8_t)(a - FM_ALARM_SOS);
  else if (a == FM_ALARM_MEDICAL)              c = FM_SOS_MEDICAL;
  else if (a == FM_ALARM_WATER)                c = FM_SOS_GENERAL;
  else if (a == FM_ALARM_EVACUATE)             c = FM_SOS_EVACUATION;
  else return false;
  if (ch) *ch = c;
  return true;
}

/** "GENERAL", "MEDICAL", ... for detail screens and the serial log. */
inline const char *fmSosChName(uint8_t ch) {
  switch (ch) {
    case FM_SOS_GENERAL:    return "GENERAL";
    case FM_SOS_MEDICAL:    return "MEDICAL";
    case FM_SOS_EVACUATION: return "EVACUATION";
    case FM_SOS_HAZARD:     return "HAZARD";
    case FM_SOS_FOOD:       return "FOOD SUPPLY";
    default:                return "?";
  }
}

/** Tab labels: "GEN", "MED", "EVAC", "HAZ", "FOOD". */
inline const char *fmSosChShort(uint8_t ch) {
  switch (ch) {
    case FM_SOS_GENERAL:    return "GEN";
    case FM_SOS_MEDICAL:    return "MED";
    case FM_SOS_EVACUATION: return "EVAC";
    case FM_SOS_HAZARD:     return "HAZ";
    case FM_SOS_FOOD:       return "FOOD";
    default:                return "?";
  }
}

/** The same labels in lower case, for channels a responder has not selected. */
inline const char *fmSosChShortLower(uint8_t ch) {
  switch (ch) {
    case FM_SOS_GENERAL:    return "gen";
    case FM_SOS_MEDICAL:    return "med";
    case FM_SOS_EVACUATION: return "evac";
    case FM_SOS_HAZARD:     return "haz";
    case FM_SOS_FOOD:       return "food";
    default:                return "?";
  }
}

inline const char *fmAlarmName(uint8_t a) {
  switch (a) {
    case FM_ALARM_SAFE:           return "SAFE";
    case FM_ALARM_MEDICAL:        return "NEED MEDICAL";
    case FM_ALARM_WATER:          return "WATER GND FLOOR";
    case FM_ALARM_EVACUATE:       return "NEED EVACUATION";
    case FM_ALARM_SOS:            return "SOS GENERAL";
    case FM_ALARM_SOS_MEDICAL:    return "SOS MEDICAL";
    case FM_ALARM_SOS_EVACUATION: return "SOS EVACUATION";
    case FM_ALARM_SOS_HAZARD:     return "SOS HAZARD";
    case FM_ALARM_SOS_FOOD:       return "SOS FOOD SUPPLY";
    default:                      return "?";
  }
}
