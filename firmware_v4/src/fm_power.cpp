/**
 * FloodMesh V4 - external power detection. See fm_power.h.
 */
#include "fm_power.h"

namespace {

bool     g_fitted = false;
bool     g_external = false;
bool     g_started = false;
uint32_t g_vbusMv = 0;

// Trend method: one average per minute, newest first, FM_PWR_TREND_MIN + 1 of them.
const uint8_t kMinutes = FM_PWR_TREND_MIN + 1;
uint32_t g_minuteAvg[kMinutes];
uint8_t  g_minutes = 0;           // how many entries of g_minuteAvg are valid
uint32_t g_sum = 0;
uint16_t g_n = 0;
uint32_t g_minuteStart = 0;
uint32_t g_prevMv = 0;

void resetTrend() {
  g_minutes = 0;
  g_sum = 0;
  g_n = 0;
}

void decide(bool external, const char *why, uint32_t mv) {
  if (external == g_external) return;
  g_external = external;
  resetTrend();   // judge the next change from here, not from before this one
  Serial.printf("[PWR] trend: %s (%s, %lu mV)\n", external ? "external power" : "battery", why,
                (unsigned long)mv);
}

/**
 * Drive the pin, release it, read it back 50 us later. An unconnected pin
 * keeps the level it was driven to (a few pF, leakage in nA); with the
 * divider fitted it is pulled to 0 V (battery) or ~2.5 V (VBUS) within a
 * microsecond. So: fitted if it fails to hold HIGH, or fails to hold LOW.
 */
bool probeDivider(int pin) {
  pinMode(pin, OUTPUT);
  digitalWrite(pin, HIGH);
  delayMicroseconds(20);
  pinMode(pin, INPUT);
  delayMicroseconds(50);
  const bool heldHigh = digitalRead(pin) == HIGH;
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  delayMicroseconds(20);
  pinMode(pin, INPUT);
  delayMicroseconds(50);
  const bool heldLow = digitalRead(pin) == LOW;
  return !heldHigh || !heldLow;
}

}  // namespace

void fmPowerBegin() {
#if FM_PWR_SENSE_PIN >= 0
  g_fitted = probeDivider(FM_PWR_SENSE_PIN);
#else
  g_fitted = false;
#endif
  Serial.printf("[PWR] power-sense divider on GPIO %d: %s\n", (int)FM_PWR_SENSE_PIN,
                g_fitted ? "fitted - external power is measured"
                         : "not fitted - guessing from the battery voltage trend");
}

bool fmPowerUpdate(uint32_t battMv, uint32_t nowMs) {
#if FM_PWR_SENSE_PIN >= 0
  if (g_fitted) {
    g_vbusMv = (uint32_t)analogReadMilliVolts(FM_PWR_SENSE_PIN) * 2UL;   // 100 k / 100 k
    g_external = g_vbusMv >= FM_PWR_VBUS_MV;
    g_started = true;
    return g_external;
  }
#endif

  if (!g_started) {
    g_started = true;
    g_external = battMv >= FM_PWR_ON_MV || battMv < FM_PWR_NO_BATT_MV;
    g_prevMv = battMv;
    g_minuteStart = nowMs;
    resetTrend();
    return g_external;
  }
  if (battMv < FM_PWR_NO_BATT_MV) {
    decide(true, "no cell", battMv);
    g_prevMv = battMv;
    return g_external;
  }

  // A jump between two readings 10 s apart: a charger connecting or leaving.
  const int32_t step = (int32_t)battMv - (int32_t)g_prevMv;
  g_prevMv = battMv;
  if (step >= FM_PWR_STEP_MV) decide(true, "voltage jumped up", battMv);
  else if (step <= -FM_PWR_STEP_MV) decide(false, "voltage dropped", battMv);

  // Minute averages, then the change across the window.
  g_sum += battMv;
  g_n++;
  if ((nowMs - g_minuteStart) >= 60000UL && g_n) {
    g_minuteStart = nowMs;
    for (uint8_t i = kMinutes - 1; i > 0; i--) g_minuteAvg[i] = g_minuteAvg[i - 1];
    g_minuteAvg[0] = g_sum / g_n;
    if (g_minutes < kMinutes) g_minutes++;
    g_sum = 0;
    g_n = 0;
    if (g_minutes == kMinutes) {
      const int32_t trend = (int32_t)g_minuteAvg[0] - (int32_t)g_minuteAvg[kMinutes - 1];
      if (trend >= FM_PWR_RISE_MV) decide(true, "voltage rising", g_minuteAvg[0]);
      else if (trend <= -FM_PWR_FALL_MV) decide(false, "voltage falling", g_minuteAvg[0]);
    }
  }
  return g_external;
}

bool        fmPowerExternal() { return g_external; }
bool        fmPowerSenseFitted() { return g_fitted; }
uint32_t    fmPowerVbusMv() { return g_vbusMv; }
const char *fmPowerMethod() { return g_fitted ? "VBUS" : "trend"; }
