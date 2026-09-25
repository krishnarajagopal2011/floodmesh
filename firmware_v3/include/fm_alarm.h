/**
 * FloodMesh V3 - alarm types (same on-air values as the earlier firmware, so
 * V3 alarms stay compatible with it).
 */
#pragma once
#include <Arduino.h>

enum FmAlarm : uint8_t {
  FM_ALARM_SAFE = 0,
  FM_ALARM_MEDICAL,
  FM_ALARM_WATER,
  FM_ALARM_EVACUATE,
  FM_ALARM_SOS,
  FM_ALARM_COUNT,
  FM_ALARM_NONE = 0xFF
};

inline const char *fmAlarmName(uint8_t a) {
  switch (a) {
    case FM_ALARM_SAFE:     return "SAFE";
    case FM_ALARM_MEDICAL:  return "NEED MEDICAL";
    case FM_ALARM_WATER:    return "WATER GND FLOOR";
    case FM_ALARM_EVACUATE: return "NEED EVACUATION";
    case FM_ALARM_SOS:      return "SOS";
    default:                return "?";
  }
}
