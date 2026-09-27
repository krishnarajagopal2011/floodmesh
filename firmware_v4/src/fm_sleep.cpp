/**
 * FloodMesh V4 - light sleep. See fm_sleep.h.
 */
#include "fm_sleep.h"

#include <driver/gpio.h>
#include <esp_sleep.h>

#include "floodmesh_pins.h"
#include "fm_keypad.h"
#include "fm_radio.h"

namespace {
uint32_t g_totalMs = 0;
uint32_t g_count = 0;
}  // namespace

FmWake fmSleepLight(uint32_t maxMs) {
  if (maxMs == 0) return FM_WAKE_NONE;
  if (maxMs > FM_SLEEP_MAX_MS) maxMs = FM_SLEEP_MAX_MS;

  // Arm both sides first; either one can veto. The after-wake calls are made
  // on every path, because a prepare leaves pins and interrupts changed even
  // when it returns false.
  const bool keysOk = fmKeypadPrepareSleep();
  const bool radioOk = keysOk && fmRadioPrepareSleep();
  if (!keysOk || !radioOk) {
    fmRadioAfterWake();
    fmKeypadAfterWake();
    return FM_WAKE_NONE;
  }

  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup((uint64_t)maxMs * 1000ULL);
  Serial.flush();   // the UART stops in light sleep; finish the log line first

  const uint32_t t0 = millis();
  esp_light_sleep_start();
  const uint32_t slept = millis() - t0;   // millis() keeps counting through light sleep

  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

  // Which GPIO woke us is not reported for light sleep; the levels tell.
  const bool dio1High = digitalRead(PIN_LORA_DIO1) == HIGH;
  fmRadioAfterWake();
  fmKeypadAfterWake();

  g_totalMs += slept;
  g_count++;

  if (cause == ESP_SLEEP_WAKEUP_TIMER) return FM_WAKE_TIMER;
  if (cause == ESP_SLEEP_WAKEUP_GPIO) return dio1High ? FM_WAKE_RADIO : FM_WAKE_KEY;
  return FM_WAKE_OTHER;
}

uint32_t fmSleepTotalMs() { return g_totalMs; }
uint32_t fmSleepCount() { return g_count; }
