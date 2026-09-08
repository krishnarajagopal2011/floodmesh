/**
 * FloodMesh - off-grid store-and-forward emergency communicator.
 * Heltec WiFi LoRa 32 V3 (ESP32-S3FN8 + SX1262 + SSD1306).
 *
 * This file owns the user-facing device: boot, the OLED, and the wiring
 * between button events and the mesh. It holds no protocol logic - fragmenting,
 * relaying, authenticating and reassembling all live in fm_mesh.
 *
 * The two traffic classes, and what they cost
 * ------------------------------------------
 *   Layer 1  status alarms, 24 bytes, PSK-authenticated, ~70 ms on air.
 *            Never throttled, never queued behind voice, and they abort a
 *            voice burst already in flight.
 *   Layer 2  voice notes, 10 s of Codec2 at 1200 bps = 1500 bytes = 8
 *            fragments, ~2.6 s on air. Best-effort. Refused when the
 *            regulatory duty-cycle budget cannot cover the whole burst.
 *
 * Input model (see fm_input.h for the state machine)
 * -------------------------------------------------
 * A 2x2 keypad, plus a side PTT switch that acts as a shift key:
 *
 *        L1 [top-left]        R1 [top-right]
 *        L2 [bottom-left]     R2 [bottom-right]
 *
 *   PTT up    R1 = channel up   R2 = channel down
 *             L2 = inbox prev   L1 = play / select
 *   PTT down  record; R1 = SAFE     R2 = WATER GROUND FLOOR
 *                     L2 = MEDICAL  L1 = EVACUATION
 *   released  3 s decision window: hold L1 to send now, tap L2 to bin it
 *
 * BENCH BUILD (FM_TEST_PTT_ON_R2 = 1): the side switch is not fitted, so R2
 * stands in as the PTT. R2 therefore has no channel-down and no WATER alarm;
 * hold R2 to record, R2 + another key for that key's alarm. Channel still
 * cycles with R1, so all four channels stay reachable. Set the flag to 0 once
 * the real switch is on GPIO 47.
 *
 * With PTT_HARDWARE_INSTALLED = 0 and no stand-in, the shift key does not
 * exist at all: recording and every alarm are unreachable and the device is a
 * receive-only pager.
 *
 * A note on blocking. fmMeshLoop() can block for the length of a CAD backoff
 * while it transmits, which stalls button polling and the display. Capture is
 * protected by fmMeshInhibitTx(), but the stall is real and visible. Moving the
 * radio onto its own FreeRTOS task is the documented next step; the protocol
 * was made correct single-threaded first so that a later concurrency bug can be
 * told apart from a protocol bug.
 */

#include <Arduino.h>
#include <Wire.h>
#include <esp_system.h>
#include <U8g2lib.h>

#include "floodmesh_pins.h"
#include "fm_airtime.h"
#include "fm_audio.h"
#include "fm_auth.h"
#include "fm_codec.h"
#include "fm_dedup.h"
#include "fm_ids.h"
#include "fm_input.h"
#include "fm_mesh.h"
#include "fm_packet.h"
#include "fm_radio.h"
#include "fm_store.h"

// ---------------------------------------------------------------- build knobs
#ifndef FLOODMESH_VERSION
#define FLOODMESH_VERSION "0.3.0"
#endif
#ifndef ADC_CTRL_ENABLE_LEVEL
#define ADC_CTRL_ENABLE_LEVEL LOW
#endif
#ifndef VBAT_DIVIDER
#define VBAT_DIVIDER 4.90f
#endif
#ifndef VBAT_CAL
#define VBAT_CAL 1.000f
#endif
#ifndef FM_PSK_HEX
#define FM_PSK_HEX "000102030405060708090A0B0C0D0E0F"
#endif

static const uint32_t kUiPeriodMs    = 60;
static const uint32_t kBattPeriodMs  = 2000;
static const uint32_t kHeartbeatMs   = 1000;
static const uint32_t kRxAlarmHoldMs = 6000;  // how long an incoming alarm owns the screen

// ---------------------------------------------------------------- peripherals
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, PIN_OLED_RST, PIN_OLED_SCL, PIN_OLED_SDA);
static bool g_oledOk = false;

// ---------------------------------------------------------------- channels
enum ChannelId { CH_MEDICAL = 0, CH_BOATS, CH_VOLUNTEERS, CH_SUPPLIES };
static const char *kChannelName[FM_CH_COUNT] = {"MEDICAL", "RESCUE BOATS", "VOLUNTEERS",
                                                "SUPPLIES"};
static const char *kChannelTag[FM_CH_COUNT] = {"MDC", "BOT", "VOL", "SUP"};
static uint8_t g_channel = CH_MEDICAL;

static const char *chName(uint8_t c) { return c < FM_CH_COUNT ? kChannelName[c] : "?"; }
static const char *chTag(uint8_t c) { return c < FM_CH_COUNT ? kChannelTag[c] : "???"; }

// ---------------------------------------------------------------- buffers
// The two big allocations in the firmware, both static so a fragmented heap can
// never fail them mid-emergency. 3 KB of the 320 KB DRAM budget.
static uint8_t g_clip[FM_CLIP_BYTES];      // capture / playback scratch
static int16_t g_pcm[FM_CODEC_SAMPLES_PER_FRAME];

static int32_t  g_micPeak = 0;             // |sample| high-water mark this capture
static uint64_t g_micEnergy = 0;           // sum of squares, for RMS
static uint32_t g_micSamples = 0;
static uint16_t g_capFrames = 0;           // Codec2 frames captured so far
static size_t   g_pcmFill = 0;             // samples buffered toward the next frame
static bool     g_clipReady = false;       // g_clip holds a complete captured note

// playback
static bool     g_playing = false;
static uint16_t g_playFrame = 0;
static uint16_t g_playFrames = 0;

// inbox
static uint8_t  g_inboxSel = 0;
// When the operator browses, their choice is PINNED: an arriving message must
// not yank the screen out from under someone mid-read. The pin lapses after a
// period of no interaction, so a device left alone drifts back to showing the
// newest traffic - which is what matters if nobody is holding it.
#ifndef FM_INBOX_PIN_MS
#define FM_INBOX_PIN_MS 30000
#endif
static uint32_t g_inboxPinnedAt = 0;   // 0 = not pinned, follow the newest

static bool inboxPinned(uint32_t now) {
  return g_inboxPinnedAt != 0 && (now - g_inboxPinnedAt) < FM_INBOX_PIN_MS;
}

// incoming alarm banner
static bool     g_rxAlarmShow = false;
static uint32_t g_rxAlarmAt = 0;
static char     g_rxAlarmFrom[FM_CALLSIGN_LEN + 1] = {0};
static uint8_t  g_rxAlarmType = 0;
static uint8_t  g_rxAlarmCh = 0;

/**
 * Point the inbox at the newest clip. fmStoreNewest() returns a not-found
 * sentinel when the store is empty, and that sentinel must never reach the UI:
 * it rendered on the OLED as "INBOX [256/3]".
 */
static void selectNewestClip() {
  const uint8_t newest = fmStoreNewest();
  g_inboxSel = (newest < FM_STORE_SLOTS) ? newest : 0;
}

// ---------------------------------------------------------------- buzzer
static void buzz(uint16_t onMs, uint8_t pulses = 1, uint16_t gapMs = 60) {
  for (uint8_t i = 0; i < pulses; i++) {
    digitalWrite(PIN_BUZZER, HIGH);
    delay(onMs);
    digitalWrite(PIN_BUZZER, LOW);
    if (i + 1 < pulses) delay(gapMs);
  }
}

// ---------------------------------------------------------------- battery
static int cmpInt(const void *a, const void *b) {
  return (*(const int *)a) - (*(const int *)b);
}

static float readBatteryVolts() {
  digitalWrite(PIN_ADC_CTRL, ADC_CTRL_ENABLE_LEVEL);
  delayMicroseconds(500);
  const uint8_t kSamples = 15;
  int mv[kSamples];
  for (uint8_t i = 0; i < kSamples; i++) mv[i] = analogReadMilliVolts(PIN_VBAT_ADC);
  digitalWrite(PIN_ADC_CTRL, !ADC_CTRL_ENABLE_LEVEL);
  qsort(mv, kSamples, sizeof(int), cmpInt);  // median rejects LoRa/switching spikes
  return (mv[kSamples / 2] * VBAT_DIVIDER * VBAT_CAL) / 1000.0f;
}

static uint8_t batteryPercent(float v) {
  static const float kCurve[][2] = {
      {4.20f, 100}, {4.10f, 92}, {4.00f, 83}, {3.90f, 72}, {3.80f, 59},
      {3.75f, 50},  {3.70f, 40}, {3.65f, 30}, {3.60f, 20}, {3.50f, 10},
      {3.40f, 5},   {3.20f, 0},
  };
  const size_t n = sizeof(kCurve) / sizeof(kCurve[0]);
  if (v >= kCurve[0][0]) return 100;
  if (v <= kCurve[n - 1][0]) return 0;
  for (size_t i = 1; i < n; i++) {
    if (v >= kCurve[i][0]) {
      const float span = kCurve[i - 1][0] - kCurve[i][0];
      const float frac = (v - kCurve[i][0]) / span;
      return (uint8_t)(kCurve[i][1] + frac * (kCurve[i - 1][1] - kCurve[i][1]) + 0.5f);
    }
  }
  return 0;
}

// ---------------------------------------------------------------- PSK
/** Parse FM_PSK_HEX into 16 bytes. Returns false if it is malformed. */
static bool parsePsk(uint8_t out[16]) {
  const char *h = FM_PSK_HEX;
  if (strlen(h) != 32) return false;
  for (uint8_t i = 0; i < 16; i++) {
    uint8_t b = 0;
    for (uint8_t k = 0; k < 2; k++) {
      const char c = h[i * 2 + k];
      uint8_t v;
      if (c >= '0' && c <= '9')      v = (uint8_t)(c - '0');
      else if (c >= 'A' && c <= 'F') v = (uint8_t)(c - 'A' + 10);
      else if (c >= 'a' && c <= 'f') v = (uint8_t)(c - 'a' + 10);
      else return false;
      b = (uint8_t)((b << 4) | v);
    }
    out[i] = b;
  }
  return true;
}

static bool pskIsPlaceholder(const uint8_t k[16]) {
  for (uint8_t i = 0; i < 16; i++) {
    if (k[i] != i) return false;
  }
  return true;
}

// ---------------------------------------------------------------- UI helpers
static void drawRight(int y, const char *s) {
  oled.drawStr(128 - oled.getStrWidth(s), y, s);
}

static void formatAge(char *out, size_t n, uint32_t rxMillis, uint32_t now) {
  const uint32_t sec = (now - rxMillis) / 1000;
  if (sec < 60)        snprintf(out, n, "%02lus AGO", (unsigned long)sec);
  else if (sec < 3600) snprintf(out, n, "%02lum AGO", (unsigned long)(sec / 60));
  else                 snprintf(out, n, "%02luh AGO", (unsigned long)(sec / 3600));
}

// ------------------------------------------------ State 1: standby / browsing
static void drawStandby(uint8_t pct, uint32_t now) {
  char l[40];

  oled.setFont(u8g2_font_5x8_tf);
  snprintf(l, sizeof(l), "CH: %s", chName(g_channel));
  oled.drawStr(0, 7, l);
  snprintf(l, sizeof(l), "%u%%", pct);
  drawRight(7, l);
  oled.drawHLine(0, 9, 128);

  FmClipMeta m;
  const bool have = fmStoreMeta(g_inboxSel, &m) && m.used;
  // If the view is pinned and something newer has arrived, say so. Not
  // stealing the selection must not mean staying silent about new traffic.
  const uint8_t newest = fmStoreNewest();
  const bool unread = inboxPinned(now) && newest < FM_STORE_SLOTS && newest != g_inboxSel;
  snprintf(l, sizeof(l), "INBOX [%u/%u]%s", g_inboxSel + 1, (unsigned)FM_STORE_SLOTS,
           unread ? " *NEW*" : "");
  oled.drawStr(0, 19, l);
  if (have) {
    formatAge(l, sizeof(l), m.rxMillis, now);
    drawRight(19, l);
    snprintf(l, sizeof(l), "FROM: %s", m.callSign);
    oled.drawStr(0, 28, l);
    snprintf(l, sizeof(l), "LEN : %02us   CH:%s", (unsigned)(m.lengthMs10 / 10),
             chTag(m.channel));
    oled.drawStr(0, 37, l);
  } else {
    drawRight(19, "-- ---");
    oled.drawStr(0, 28, "FROM: -----");
    oled.drawStr(0, 37, "LEN : --s");
  }
  oled.drawHLine(0, 41, 128);

  if (g_playing) {
    snprintf(l, sizeof(l), "PLAYING %u/%u", g_playFrame, g_playFrames);
    oled.drawStr(0, 51, l);
  } else {
#if FM_TEST_PTT_ON_R2
    oled.drawStr(0, 51, "R1 CH  L2 PREV  L1 PLAY");
#else
    oled.drawStr(0, 51, "R1/R2 CH  L2 PREV");
#endif
  }
  // Airtime is a regulatory budget, not a curiosity - keep it on screen.
  snprintf(l, sizeof(l), "%s  AIR %u.%u%%", fmCallSign(), fmAirtimePermille() / 10,
           fmAirtimePermille() % 10);
  oled.setFont(u8g2_font_4x6_tf);
  oled.drawStr(0, 62, l);
}

// --------------------------------------- State 2: PTT held, recording + chords
static void drawRecording(uint8_t pct, bool capped) {
  char l[40];
  const uint32_t ms = fmRecordElapsedMs();

  oled.drawBox(0, 0, 128, 11);
  oled.setDrawColor(0);
  oled.setFont(u8g2_font_5x8_tf);
  oled.drawStr(2, 8, capped ? ">> CAP 10.0s" : ">> RECORDING");
  snprintf(l, sizeof(l), "%u%%", pct);
  oled.drawStr(126 - oled.getStrWidth(l), 8, l);
  oled.setDrawColor(1);

  oled.setFont(u8g2_font_5x8_tf);
  snprintf(l, sizeof(l), "CH: %s", chName(g_channel));
  oled.drawStr(0, 21, l);
  snprintf(l, sizeof(l), "%4.1f/%.0fs", ms / 1000.0f, FM_VOICE_MAX_MS / 1000.0f);
  drawRight(21, l);

  oled.drawFrame(0, 25, 128, 12);
  const int w = (int)((uint32_t)ms * 124UL / FM_VOICE_MAX_MS);
  if (w > 0) oled.drawBox(2, 27, w, 8);

  oled.drawStr(0, 48, capped ? "RELEASE PTT" : "RELEASE = REVIEW");

  // The chord legend stays visible the whole time PTT is down - it is the only
  // thing telling the user the keys have changed meaning. Laid out to mirror
  // the physical 2x2 keypad, so the operator reads position rather than a list.
  oled.drawHLine(0, 52, 128);
  oled.setFont(u8g2_font_4x6_tf);
  oled.drawStr(0, 58, "EVAC");
  drawRight(58, "SAFE");
  oled.drawStr(0, 64, "MED");
#if !FM_TEST_PTT_ON_R2
  drawRight(64, "WATR");
#else
  drawRight(64, "[PTT]");   // R2 is the modifier on this build, not an alarm
#endif
}

// ------------------------------- State 3: send / cancel decision window
static void drawPendingSend(uint8_t pct) {
  char l[40];

  oled.drawBox(0, 0, 128, 11);
  oled.setDrawColor(0);
  oled.setFont(u8g2_font_5x8_tf);
  oled.drawStr(2, 8, "READY TO SEND");
  snprintf(l, sizeof(l), "%u%%", pct);
  oled.drawStr(126 - oled.getStrWidth(l), 8, l);
  oled.setDrawColor(1);

  oled.setFont(u8g2_font_5x8_tf);
  snprintf(l, sizeof(l), "CLIP %.1fs  CH:%s", fmHeldClipMs() / 1000.0f, chTag(g_channel));
  oled.drawStr(0, 21, l);

  uint8_t holdPct = 0;
  if (fmSendHoldProgress(&holdPct)) {
    oled.drawStr(0, 33, "SENDING...");
    oled.drawFrame(0, 36, 128, 10);
    const int w = (int)(holdPct * 124 / 100);
    if (w > 0) oled.drawBox(2, 38, w, 6);
  } else {
    const uint32_t left = fmSendRemainingMs();
    snprintf(l, sizeof(l), FM_SEND_ON_TIMEOUT ? "SENDING IN %lu..." : "DISCARD IN %lu...",
             (unsigned long)((left + 999) / 1000));
    oled.drawStr(0, 33, l);
    oled.drawFrame(0, 36, 128, 10);
    // Fill toward the send, left to right. A bar that drains reads as time
    // running out on something already happening; this is progress toward an
    // event that has not happened yet.
    const uint32_t done = FM_SEND_CONFIRM_MS - left;
    const int w = (int)(done * 124UL / FM_SEND_CONFIRM_MS);
    if (w > 0) oled.drawBox(2, 38, w, 6);
  }

  oled.drawHLine(0, 50, 128);
  oled.drawStr(0, 60, "HOLD L1=SEND  L2=CANCEL");
}

// ------------------------------------------- State 4: Layer 1 alarm, outgoing
static void drawAlarmSent(uint8_t pct) {
  char l[40];
  oled.drawBox(0, 0, 128, 11);
  oled.setDrawColor(0);
  oled.setFont(u8g2_font_5x8_tf);
  oled.drawStr(2, 8, "!! ALARM SENT !!");
  snprintf(l, sizeof(l), "%u%%", pct);
  oled.drawStr(126 - oled.getStrWidth(l), 8, l);
  oled.setDrawColor(1);

  oled.setFont(u8g2_font_7x13B_tf);
  const char *name = fmAlarmName(fmLastAlarm());
  oled.drawStr((128 - oled.getStrWidth(name)) / 2, 28, name);

  oled.setFont(u8g2_font_5x8_tf);
  snprintf(l, sizeof(l), "CH: %s", chName(g_channel));
  oled.drawStr(0, 42, l);
  oled.drawStr(0, 51, "LAYER 1 / PSK AUTH");
  oled.drawStr(0, 60, "RELEASE PTT");
}

// ------------------------------------------- State 5: Layer 1 alarm, incoming
static void drawAlarmRx(uint8_t pct, uint32_t now) {
  char l[40];
  // Inverted whole-header so it reads across a dark room at a glance.
  oled.drawBox(0, 0, 128, 13);
  oled.setDrawColor(0);
  oled.setFont(u8g2_font_7x13B_tf);
  oled.drawStr(2, 11, "ALARM IN");
  oled.setDrawColor(1);

  oled.setFont(u8g2_font_7x13B_tf);
  const char *name = fmAlarmName((FmAlarm)g_rxAlarmType);
  oled.drawStr((128 - oled.getStrWidth(name)) / 2, 30, name);

  oled.setFont(u8g2_font_5x8_tf);
  snprintf(l, sizeof(l), "FROM: %s", g_rxAlarmFrom);
  oled.drawStr(0, 43, l);
  snprintf(l, sizeof(l), "CH: %s", chName(g_rxAlarmCh));
  oled.drawStr(0, 52, l);
  snprintf(l, sizeof(l), "%lus ago", (unsigned long)((now - g_rxAlarmAt) / 1000));
  drawRight(52, l);
  oled.setFont(u8g2_font_4x6_tf);
  oled.drawStr(0, 62, "any button to dismiss");
}

static void drawScreen(uint8_t pct, uint32_t now) {
  if (!g_oledOk) return;
  oled.clearBuffer();
  if (g_rxAlarmShow) {
    drawAlarmRx(pct, now);
  } else {
    switch (fmInputState()) {
      case FM_IN_RECORDING:     drawRecording(pct, false); break;
      case FM_IN_CAPPED:        drawRecording(pct, true);  break;
      case FM_IN_PENDING_SEND:  drawPendingSend(pct);      break;
      case FM_IN_ALARM_LATCHED: drawAlarmSent(pct);        break;
      default:                  drawStandby(pct, now);     break;
    }
  }
  oled.sendBuffer();
}

// ---------------------------------------------------------------- mesh events
static void onAlarmRx(const FmAlarmRx *rx) {
  Serial.printf("[RX] ALARM %s from %s on CH %s (snr %.1f dB, rssi %.0f dBm)\n",
                fmAlarmName((FmAlarm)rx->alarmType), rx->header.callSign,
                chName(rx->header.channel), rx->snr, rx->rssi);
  snprintf(g_rxAlarmFrom, sizeof(g_rxAlarmFrom), "%s", rx->header.callSign);
  g_rxAlarmType = rx->alarmType;
  g_rxAlarmCh = rx->header.channel;
  g_rxAlarmAt = millis();
  g_rxAlarmShow = true;
  // Long, hard, and unlike every other sound the device makes.
  buzz(300, 3, 120);
}

static void onVoiceRx(const FmVoiceRx *rx) {
  Serial.printf("[RX] VOICE from %s on CH %s, %u/%u fragments\n", rx->header.callSign,
                chName(rx->header.channel), rx->fragsReceived, (unsigned)FM_FRAG_COUNT);
  FmClipMeta m;
  snprintf(m.callSign, sizeof(m.callSign), "%s", rx->header.callSign);
  m.channel = rx->header.channel;
  m.lengthMs10 = (uint16_t)(FM_VOICE_MAX_MS / 100);
  m.rxMillis = millis();
  m.used = true;
  if (fmStoreWrite(&m, rx->clip, rx->len)) {
    if (!inboxPinned(millis())) {
      selectNewestClip();
    } else {
      Serial.println("[UI] inbox selection pinned - new clip stored, view unchanged");
    }
    buzz(60, 2, 80);
  }
}

// ---------------------------------------------------------------- capture
static void captureStart() {
  g_capFrames = 0;
  g_pcmFill = 0;
  g_clipReady = false;
  g_micPeak = 0;
  g_micEnergy = 0;
  g_micSamples = 0;
  memset(g_clip, 0, sizeof(g_clip));
  fmMeshInhibitTx(true);   // a blocking transmit would punch a hole in the audio
  if (!fmMicStart()) Serial.println("[VOICE] microphone failed to start");
}

/**
 * Drain the mic and encode every whole Codec2 frame that is ready.
 *
 * This must consume ALL buffered audio each pass, not one frame per loop. The
 * I2S DMA holds only ~64 ms, so at one frame per iteration the capture rate is
 * hostage to whatever else the loop is doing - which is exactly how a 3 s press
 * produced 22 frames instead of 76.
 */
static void capturePump() {
  for (;;) {
    const size_t want = (size_t)FM_CODEC_SAMPLES_PER_FRAME - g_pcmFill;
    const size_t got = fmMicRead(g_pcm + g_pcmFill, want);
    if (got == 0) return;                  // DMA drained
    // Meter what the microphone is actually delivering. 250 encoded frames
    // prove the pipeline ran, not that anything was heard - Codec2 encodes
    // silence just as happily as speech.
    for (size_t i = 0; i < got; i++) {
      const int32_t v = g_pcm[g_pcmFill + i];
      const int32_t a = v < 0 ? -v : v;
      if (a > g_micPeak) g_micPeak = a;
      g_micEnergy += (uint64_t)((int64_t)v * v);
      g_micSamples++;
    }

    g_pcmFill += got;
    if (g_pcmFill < (size_t)FM_CODEC_SAMPLES_PER_FRAME) return;  // partial frame

    g_pcmFill = 0;
    if (g_capFrames >= FM_C2_FRAMES_TOTAL) return;               // clip is full
    if (fmCodecEncodeFrame(g_pcm, g_clip + (size_t)g_capFrames * FM_C2_FRAME_BYTES)) {
      g_capFrames++;
    }
  }
}

static void captureStop() {
  fmMicStop();
  fmMeshInhibitTx(false);
  // Frames never captured stay zero-filled, so a short note still fragments as
  // a full 1500-byte clip. Codec2 renders the tail as silence.
  g_clipReady = (g_capFrames > 0);

  const uint32_t rms =
      g_micSamples ? (uint32_t)sqrt((double)(g_micEnergy / g_micSamples)) : 0;
  Serial.printf("[VOICE] captured %u/%u frames, mic peak %ld rms %lu (full scale 32767)\n",
                g_capFrames, (unsigned)FM_C2_FRAMES_TOTAL, (long)g_micPeak,
                (unsigned long)rms);
  if (g_micPeak < 200) {
    Serial.println("[VOICE] !! microphone delivered near-silence. The clip will play");
    Serial.println("[VOICE] !! back as nothing. Check INMP441 wiring: WS/SCK/SD, and");
    Serial.println("[VOICE] !! that L/R is tied to GND for the LEFT slot.");
  }
}

// ---------------------------------------------------------------- playback
static void playbackStart(uint8_t slot) {
  FmClipMeta m;
  size_t len = 0;
  if (!fmStoreRead(slot, g_clip, sizeof(g_clip), &len, &m)) {
    Serial.printf("[PLAY] slot %u is empty or unreadable\n", slot);
    buzz(200);
    return;
  }
  g_playFrames = (uint16_t)(len / FM_C2_FRAME_BYTES);
  g_playFrame = 0;
  g_playing = true;
  fmAmpEnable(true);
  fmSpeakerStart();
  Serial.printf("[PLAY] slot %u from %s, %u frames\n", slot, m.callSign, g_playFrames);
}

static void playbackStop() {
  fmSpeakerStop();
  fmAmpEnable(false);   // back to hard shutdown; the amp idles at ~2 mA otherwise
  g_playing = false;
}

/** Decode and push one frame per call; the I2S DMA paces us. */
static void playbackPump() {
  if (g_playFrame >= g_playFrames) { playbackStop(); return; }
  if (fmCodecDecodeFrame(g_clip + (size_t)g_playFrame * FM_C2_FRAME_BYTES, g_pcm)) {
    fmSpeakerWrite(g_pcm, (size_t)FM_CODEC_SAMPLES_PER_FRAME);
  }
  g_playFrame++;
}

// ---------------------------------------------------------------- boot report
static void printBanner() {
  Serial.println();
  Serial.println("=========================================================");
  Serial.printf("  FloodMesh %s\n", FLOODMESH_VERSION);
  Serial.println("=========================================================");
  Serial.printf("  chip   : %s rev %u, %u core(s) @ %u MHz\n", ESP.getChipModel(),
                ESP.getChipRevision(), ESP.getChipCores(), getCpuFrequencyMhz());
  Serial.printf("  flash  : %u MB   free heap: %u B\n",
                ESP.getFlashChipSize() / (1024 * 1024), ESP.getFreeHeap());
  Serial.printf("  reset  : reason %d\n", (int)esp_reset_reason());
  Serial.println("---------------------------------------------------------");
  Serial.println("  INPUT MODEL");
  Serial.println("    keypad   : L1 top-left  L2 bottom-left");
  Serial.println("               R1 top-right R2 bottom-right");
#if FM_TEST_PTT_ON_R2
  Serial.println("    PTT      : R2 (bottom-right) - hold to record");
  Serial.println("    PTT up   : R1=CH+  L2=INBOX PREV  L1=PLAY");
  Serial.println("    PTT down : R2+R1=SAFE  R2+L2=MEDICAL  R2+L1=EVACUATE");
  Serial.println("               (WATER unavailable: its key is the PTT)");
#else
  Serial.println("    PTT up   : R1=CH+  R2=CH-  L2=INBOX PREV  L1=PLAY");
  Serial.println("    PTT down : record; R1=SAFE R2=WATER L2=MEDICAL L1=EVACUATE");
#endif
  Serial.printf("    released : %lu ms window, hold [>] to send, [<] to cancel\n",
                (unsigned long)FM_SEND_CONFIRM_MS);
  Serial.printf("    timeout  : %s\n",
                FM_SEND_ON_TIMEOUT ? "SENDS the clip" : "DISCARDS the clip");
  Serial.println("---------------------------------------------------------");
#if FM_WARN_PTT_BORROWED
  Serial.println("  [TEST] FM_TEST_PTT_ON_R2=1: the bottom-right key (R2, GPIO 4)");
  Serial.println("         is standing in for the side PTT switch. While this is");
  Serial.println("         set, R2 has NO channel-down and NO WATER alarm - hold");
  Serial.println("         R2 to record, R2 + another key for that key's alarm.");
  Serial.println("         Set -D FM_TEST_PTT_ON_R2=0 once GPIO 47 is soldered.");
#endif
#if FM_WARN_NO_PTT
  Serial.println("  [WARN] No PTT switch fitted (PTT_HARDWARE_INSTALLED=0).");
  Serial.println("         Recording is disabled, and because every Layer 1");
  Serial.println("         alarm is a PTT chord, THIS DEVICE CANNOT SEND ANY");
  Serial.println("         ALARM. It receives, relays and plays back only.");
#endif
#if FM_WARN_AMP_ON_USB_PINS
  Serial.println("  [WARN] Amp I2S uses GPIO19/20 (native USB D-/D+). Workable on");
  Serial.println("         this board but native USB-CDC and JTAG are forfeited.");
#endif
  Serial.println("=========================================================");
}

// ---------------------------------------------------------------- test transmitter
#if FM_ROLE_TESTTX
/**
 * Bench transmitter role, for a SECOND BOARD WITH NOTHING SOLDERED TO IT.
 *
 * It needs no microphone, speaker or keypad: it synthesises a Codec2 clip in
 * RAM at boot and broadcasts it on a timer, plus a rotating Layer 1 alarm. That
 * exercises the receiving node's entire inbound path - CAD, dedup, header
 * parsing, PSK verification, 8-fragment reassembly, LittleFS storage and
 * playback - which a single-board loopback cannot reach, because a node ignores
 * frames bearing its own call sign.
 *
 * Both boards must be built with the same FM_PSK_HEX or every alarm is
 * correctly rejected as unauthenticated. Call signs differ automatically: they
 * are derived from each chip's MAC.
 */
#ifndef FM_TESTTX_VOICE_MS
// 2.6 s of airtime per note. Every 120 s is ~2.2% duty - just inside the 2.5%
// regulatory budget, so the transmitter does not throttle itself mid-test.
#define FM_TESTTX_VOICE_MS 120000
#endif
#ifndef FM_TESTTX_ALARM_MS
#define FM_TESTTX_ALARM_MS 15000     // alarms are ~70 ms; frequent is harmless
#endif

static void buildTestClip() {
  // A 120 Hz buzz with three harmonics, gated one second on, one second off.
  // Codec2 models a vocal tract, so a harmonic-rich buzz survives encoding and
  // arrives as a recognisable pulsing tone. A pure sine would not: the codec
  // would try to fit a voice to it and the result would be unintelligible mush,
  // which tells you nothing about whether the link is working.
  static int16_t pcm[FM_CODEC_SAMPLES_PER_FRAME];
  float phase = 0.0f;
  const float dp = 2.0f * (float)M_PI * 120.0f / 8000.0f;

  uint16_t frames = 0;
  while (frames < FM_C2_FRAMES_TOTAL) {
    const bool on = ((frames / 25) % 2) == 0;   // 25 frames = 1 second
    for (int i = 0; i < FM_CODEC_SAMPLES_PER_FRAME; i++) {
      float s = 0.0f;
      if (on) {
        s = 0.50f * sinf(phase) + 0.30f * sinf(2.0f * phase) +
            0.20f * sinf(3.0f * phase);
      }
      phase += dp;
      if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
      pcm[i] = (int16_t)(s * 9000.0f);
    }
    if (!fmCodecEncodeFrame(pcm, g_clip + (size_t)frames * FM_C2_FRAME_BYTES)) break;
    frames++;
  }
  Serial.printf("[TESTTX] synthetic clip built: %u/%u frames\n", frames,
                (unsigned)FM_C2_FRAMES_TOTAL);
}

static void testTxLoop(uint32_t now) {
  static uint32_t tVoice = 0, tAlarm = 0;
  static uint8_t  nextAlarm = 0;
  static bool     firstVoiceSent = false;

  if (now - tAlarm >= FM_TESTTX_ALARM_MS) {
    tAlarm = now;
    const uint8_t a = (uint8_t)(nextAlarm++ % FM_ALARM_COUNT);
    Serial.printf("[TESTTX] alarm %s on CH %s\n", fmAlarmName((FmAlarm)a),
                  chName(g_channel));
    fmMeshSendAlarm(a, g_channel);
  }

  const bool voiceDue = firstVoiceSent ? (now - tVoice >= FM_TESTTX_VOICE_MS)
                                       : (now >= 12000);
  if (voiceDue) {
    tVoice = now;
    firstVoiceSent = true;
    Serial.printf("[TESTTX] voice note on CH %s\n", chName(g_channel));
    if (!fmMeshSendVoice(g_clip, FM_CLIP_BYTES, g_channel)) {
      Serial.println("[TESTTX] voice refused - duty-cycle budget spent");
    }
  }
}
#endif

// ---------------------------------------------------------------- keypad probe
#if FM_KEYPAD_DISCOVER
/**
 * Map an unknown 2x2 keypad. Drives each of the four pads LOW in turn with the
 * other three pulled up, and reports any pair the pressed key bridges.
 *
 * A matrix keypad has no ground pin: a key shorts one line to another rather
 * than to GND, which is why holding all four as inputs detects nothing.
 */
static void keypadDiscover() {
  const uint8_t pins[4]   = {PIN_KEY_L1, PIN_KEY_L2, PIN_KEY_R1, PIN_KEY_R2};
  const char *names[4]    = {"L1", "L2", "R1", "R2"};
  Serial.println();
  Serial.println("[KEYPAD] DISCOVERY MODE - normal firmware is not running.");
  Serial.println("[KEYPAD] Press ONE key at a time and hold it briefly.");
  Serial.println("[KEYPAD] Order please: top-left, top-right, bottom-left, bottom-right.");
  for (;;) {
    for (uint8_t d = 0; d < 4; d++) {
      for (uint8_t i = 0; i < 4; i++) {
        if (i == d) {
          pinMode(pins[i], OUTPUT);
          digitalWrite(pins[i], LOW);
        } else {
          pinMode(pins[i], INPUT_PULLUP);
        }
      }
      delayMicroseconds(200);          // let the pull-ups settle
      for (uint8_t i = 0; i < 4; i++) {
        if (i == d) continue;
        if (digitalRead(pins[i]) == LOW) {
          Serial.printf("[KEYPAD] closed: %s (GPIO%u) <-> %s (GPIO%u)\n",
                        names[d], pins[d], names[i], pins[i]);
          delay(300);                  // one report per press, not a torrent
        }
      }
    }
    delay(15);
  }
}
#endif

// ---------------------------------------------------------------- setup
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  // Vext feeds the OLED and every external module.
  pinMode(PIN_VEXT_CTRL, OUTPUT);
  digitalWrite(PIN_VEXT_CTRL, VEXT_ON);
  delay(120);

  pinMode(PIN_ADC_CTRL, OUTPUT);
  digitalWrite(PIN_ADC_CTRL, !ADC_CTRL_ENABLE_LEVEL);
  analogSetPinAttenuation(PIN_VBAT_ADC, ADC_11db);

  printBanner();

  pinMode(PIN_OLED_RST, OUTPUT);
  digitalWrite(PIN_OLED_RST, LOW);
  delay(20);
  digitalWrite(PIN_OLED_RST, HIGH);
  delay(20);
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL, 400000);
  Wire.setTimeOut(50);   // never let a stuck bus block boot indefinitely

  // Probe the bus before handing it to U8g2. U8g2's begin() gives no
  // diagnostics and, on a bus that never ACKs, is where boot goes to die.
  Serial.println("[I2C] scanning...");
  uint8_t found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[I2C]   device @ 0x%02X%s\n", addr,
                    addr == OLED_I2C_ADDR ? "  <- SSD1306" : "");
      found++;
    }
  }
  Serial.printf("[I2C] scan done, %u device(s)\n", found);

  // Hand the bus back before U8g2 takes it. U8g2's begin() calls Wire.begin()
  // itself; with the bus already up, the core returns early ("Bus already
  // started") and U8g2 proceeds against a peripheral it did not configure,
  // which is where boot was hanging. Releasing it first makes U8g2's own
  // initialisation authoritative.
  Wire.end();

  Serial.println("[OLED] begin...");
  oled.setI2CAddress(OLED_I2C_ADDR << 1);
  // 400 kHz: a full 1 KB framebuffer is ~23 ms here versus ~90 ms at 100 kHz,
  // and at 90 ms the loop could not drain the microphone fast enough to keep
  // Codec2 fed. The earlier hang was the double Wire.begin(), not the speed.
  oled.setBusClock(400000);
  g_oledOk = oled.begin();
  Serial.printf("[OLED] %s\n", g_oledOk ? "ok" : "FAILED");
  if (g_oledOk) {
    oled.clearBuffer();
    oled.setFont(u8g2_font_7x13B_tf);
    oled.drawStr(6, 28, "FloodMesh");
    oled.setFont(u8g2_font_5x8_tf);
    oled.drawStr(6, 44, FLOODMESH_VERSION);
    oled.sendBuffer();
  }

  fmInputBegin();
  fmIdsBegin();
  Serial.printf("[ID] call sign %s (%s)\n", fmCallSign(),
                fmCallSignIsDerived() ? "derived from MAC" : "operator-assigned");

  uint8_t psk[16];
  if (!parsePsk(psk)) {
    Serial.println("[AUTH] !! FM_PSK_HEX is malformed - must be 32 hex characters.");
    memset(psk, 0, sizeof(psk));
  }
  fmAuthBegin(psk);
  if (pskIsPlaceholder(psk)) {
    Serial.println("[AUTH] !! USING THE PLACEHOLDER KEY FROM THE SOURCE TREE.");
    Serial.println("[AUTH] !! Alarms are NOT authenticated against anyone who has");
    Serial.println("[AUTH] !! this firmware. Set -D FM_PSK_HEX before deployment.");
  }

  fmAirtimeBegin();
  fmDedupBegin();
  Serial.printf("[STORE] %s, %u clips held\n", fmStoreBegin() ? "mounted" : "UNAVAILABLE",
                fmStoreCount());
  selectNewestClip();

  // Float test on the microphone data line, BEFORE I2S claims the pin.
  //
  // An unconnected pin follows whichever internal resistor is enabled; a pin
  // with something attached to it does not. This separates a broken solder
  // joint from a mic that is wired but silent - a distinction the I2S stream
  // itself cannot make, because both present as an unbroken run of zeros.
  {
    pinMode(PIN_MIC_SD, INPUT_PULLUP);
    delayMicroseconds(500);
    const int up = digitalRead(PIN_MIC_SD);
    pinMode(PIN_MIC_SD, INPUT_PULLDOWN);
    delayMicroseconds(500);
    const int dn = digitalRead(PIN_MIC_SD);
    pinMode(PIN_MIC_SD, INPUT);
    const char *verdict =
        (up == 1 && dn == 0) ? "FLOATING - nothing is connected to this pin"
      : (up == 0 && dn == 0) ? "held LOW - shorted to GND, or the mic is driving it low"
      : (up == 1 && dn == 1) ? "held HIGH - tied to 3V3"
                             : "indeterminate";
    Serial.printf("[MIC] SD (GPIO%d) float test: pullup=%d pulldown=%d -> %s\n",
                  PIN_MIC_SD, up, dn, verdict);
  }

  Serial.printf("[AUDIO] %s\n", fmAudioBegin() ? "ok" : "FAILED");
  Serial.printf("[CODEC] %s (%d samples, %d bytes per frame)\n",
                fmCodecBegin() ? "ok" : "FAILED", fmCodecSamplesPerFrame(),
                fmCodecBytesPerFrame());
  fmAudioSetVolume(FM_AUDIO_VOL_DEFAULT);

  if (!fmRadioBegin()) {
    Serial.println("[RADIO] !! SX1262 init FAILED - the device cannot reach the mesh.");
    buzz(400, 3, 150);
  } else {
    fmMeshSetAlarmHandler(onAlarmRx);
    fmMeshSetVoiceHandler(onVoiceRx);
    fmMeshBegin();
  }

  const float v = readBatteryVolts();
  Serial.printf("[BATT] %.3f V (%u%%)\n", v, batteryPercent(v));
  if (v < 2.5f || v > 4.5f) {
    Serial.println("[BATT] !! out of Li-ion range. Flip ADC_CTRL_ENABLE_LEVEL or "
                   "check the pack.");
  }

#if FM_ROLE_TESTTX
  Serial.println("=========================================================");
  Serial.println("  ROLE: BENCH TRANSMITTER. This board only sends.");
  Serial.printf("  voice every %lu s, alarm every %lu s\n",
                (unsigned long)(FM_TESTTX_VOICE_MS / 1000),
                (unsigned long)(FM_TESTTX_ALARM_MS / 1000));
  Serial.println("=========================================================");
  buildTestClip();
#endif

  buzz(40, 2);
#if FM_KEYPAD_DISCOVER
  keypadDiscover();   // never returns
#endif
  Serial.println("[READY]");
}

// ---------------------------------------------------------------- loop
void loop() {
#if FM_ROLE_TESTTX
  {
    const uint32_t now = millis();
    static uint32_t tBeatTx = 0, tUiTx = 0;
    static uint8_t  pctTx = 0;
    fmMeshLoop(now);
    testTxLoop(now);
    if (now - tBeatTx >= kHeartbeatMs) {
      tBeatTx = now;
      digitalWrite(PIN_LED, !digitalRead(PIN_LED));
      pctTx = batteryPercent(readBatteryVolts());
    }
    if (now - tUiTx >= 500) {
      tUiTx = now;
      drawScreen(pctTx, now);
    }
    return;
  }
#endif
  const uint32_t now = millis();
  static uint32_t tUi = 0, tBatt = 0, tBeat = 0;
  static uint8_t pct = 0;

  const FmInputEvent ev = fmInputPoll(now);

  // Any button dismisses an incoming-alarm banner. It must not swallow the
  // press: the alarm has been seen, the press still does its normal job.
  if (g_rxAlarmShow &&
      (ev.nav != FM_NAV_NONE || ev.recStart || ev.alarmFired ||
       (now - g_rxAlarmAt) > kRxAlarmHoldMs)) {
    g_rxAlarmShow = false;
  }

  // --- Mode A: navigation ------------------------------------------------
  switch (ev.nav) {
    case FM_NAV_CH_UP:
      g_channel = (uint8_t)((g_channel + 1) % FM_CH_COUNT);
      Serial.printf("[UI] channel -> %s\n", chName(g_channel));
      buzz(12);
      break;
    case FM_NAV_CH_DOWN:
      g_channel = (uint8_t)((g_channel + FM_CH_COUNT - 1) % FM_CH_COUNT);
      Serial.printf("[UI] channel -> %s\n", chName(g_channel));
      buzz(12);
      break;
    case FM_NAV_INBOX_PREV:
      g_inboxSel = (uint8_t)((g_inboxSel + FM_STORE_SLOTS - 1) % FM_STORE_SLOTS);
      Serial.printf("[UI] inbox -> slot %u\n", g_inboxSel);
      buzz(12);
      break;
    case FM_NAV_PLAY:
      if (g_playing) playbackStop();
      else playbackStart(g_inboxSel);
      break;
    default:
      break;
  }

  // --- Mode B: recording lifecycle ---------------------------------------
  if (ev.recStart) {
    if (g_playing) playbackStop();   // the mic and the amp cannot both be live
    Serial.println("[VOICE] capture start");
    captureStart();
    buzz(20);
  }
  if (ev.recCapped) {
    Serial.printf("[VOICE] %lu ms cap reached\n", (unsigned long)FM_VOICE_MAX_MS);
    buzz(25, 2, 40);
  }
  if (ev.recStop) captureStop();
  if (ev.sendPending) {
    Serial.printf("[VOICE] %.2f s held - hold [>] to send, [<] to cancel, auto-%s\n",
                  ev.recMs / 1000.0f, FM_SEND_ON_TIMEOUT ? "send" : "discard");
  }
  if (ev.recDrop) {
    const char *why = ev.dropReason == FM_DROP_USER_CANCEL   ? "cancelled by operator"
                      : ev.dropReason == FM_DROP_ALARM_CHORD ? "preempted by Layer 1"
                      : ev.dropReason == FM_DROP_RERECORD    ? "operator re-recording"
                                                             : "unknown";
    Serial.printf("[VOICE] clip DROPPED (%.2f s) - %s. Nothing transmitted.\n",
                  ev.recMs / 1000.0f, why);
    g_clipReady = false;
    if (ev.dropReason != FM_DROP_ALARM_CHORD) buzz(200);
  }
  if (ev.sendCommit) {
#if FM_SELF_MONITOR
    // Bench aid: keep a copy of what we just recorded so it can be played back
    // locally. A node ignores its own transmissions, so without this there is
    // no way to hear your own voice through Codec2 - which is the only way to
    // judge whether 1200 bps is actually intelligible.
    if (g_clipReady) {
      FmClipMeta self;
      snprintf(self.callSign, sizeof(self.callSign), "%s", fmCallSign());
      self.channel = g_channel;
      self.lengthMs10 = (uint16_t)(ev.recMs / 100);
      self.rxMillis = now;
      self.used = true;
      if (fmStoreWrite(&self, g_clip, FM_CLIP_BYTES)) {
        selectNewestClip();
        Serial.println("[VOICE] own clip saved to inbox (FM_SELF_MONITOR)");
      }
    }
#endif
    if (g_clipReady && fmMeshSendVoice(g_clip, sizeof(g_clip), g_channel)) {
      Serial.printf("[VOICE] SEND %.2f s on CH %s\n", ev.recMs / 1000.0f, chName(g_channel));
      buzz(15);
    } else {
      Serial.println("[VOICE] send refused - no clip, or duty-cycle budget exhausted");
      buzz(200, 2, 80);
    }
    g_clipReady = false;
  }

  // --- Mode B: Layer 1 chord ---------------------------------------------
  if (ev.alarmFired) {
    Serial.printf("[ALARM] LAYER 1 >> %s << on CH %s\n", fmAlarmName(ev.alarm),
                  chName(g_channel));
    const bool ok = fmMeshSendAlarm((uint8_t)ev.alarm, g_channel);
    if (!ok) Serial.println("[ALARM] !! transmit FAILED");
    buzz(60, 3, 50);
  }

  // --- pumps --------------------------------------------------------------
  if (fmInputState() == FM_IN_RECORDING) capturePump();
  if (g_playing) playbackPump();
  fmMeshLoop(now);

  // --- periodic -----------------------------------------------------------
  if (tBatt == 0 || now - tBatt >= kBattPeriodMs) {
    tBatt = now ? now : 1;
    pct = batteryPercent(readBatteryVolts());
  }
  if (now - tBeat >= kHeartbeatMs) {
    tBeat = now;
    digitalWrite(PIN_LED, !digitalRead(PIN_LED));
  }
#if FM_DEBUG_TICK
  // Bench diagnostic: proves loop() is still running and shows the debounced
  // key states, so "nothing happened" can be told apart from "loop is wedged".
  static uint32_t tTick = 0;
  if (now - tTick >= 2000) {
    tTick = now;
    Serial.printf("[TICK] %lus  TR=%d BR=%d BL=%d TL=%d  state=%u  heap=%u\n",
                  (unsigned long)(now / 1000), (int)fmButtonHeld(FM_BTN_UP),
                  (int)fmButtonHeld(FM_BTN_DOWN), (int)fmButtonHeld(FM_BTN_PREV),
                  (int)fmButtonHeld(FM_BTN_SELECT), (unsigned)fmInputState(),
                  (unsigned)ESP.getFreeHeap());
  }
#endif
  // The OLED and the encoder share one core. While capturing, refresh less
  // often: a progress bar at 4 Hz looks fine and gives the codec the headroom
  // it needs to stay ahead of the microphone.
  const uint32_t uiPeriod =
      (fmInputState() == FM_IN_RECORDING) ? 250 : kUiPeriodMs;
  if (now - tUi >= uiPeriod) {
    tUi = now;
    drawScreen(pct, now);
  }
}
