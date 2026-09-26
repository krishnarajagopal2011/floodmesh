/**
 * FloodMesh firmware V3 - Heltec WiFi LoRa 32 V3 + 4x4 keypad + active buzzer.
 *
 * A field-test build for range, relaying, texting and SOS. No voice, no
 * MCP23017, no side button. The original firmware in the repository root is
 * untouched; this tree is built on its own (pio run -d firmware_v3 -e v3).
 *
 * Keys (4x4 membrane:  1 2 3 A / 4 5 6 B / 7 8 9 C / * 0 # D)
 * ---------------------------------------------------------------------------
 *   anywhere   hold * and # together 3 s  -> SOS alarm
 *   HOME       0-9 start a text (the key is typed)
 *              A alerts menu, B status / range test, C inbox
 *   COMPOSE    2-9 letters, 0 space, 1 punctuation
 *              * next T9 word, # mode T9 / ABC / 123
 *              A / B previous / next T9 word
 *              C accept word, hold C = SEND
 *              D delete (leaves when empty), hold D = discard
 *   INBOX      A / B scroll, C open, D back
 *   ALERTS     A / B choose, hold C = SEND, D back
 *   STATUS     A / B scroll heard list, C range-test ping on/off, D back
 *
 * Boot combinations:
 *   hold #                  -> BLE provisioning (FloodMesh Admin app)
 *   hold * and 0 for 10 s   -> factory reset (role, keys, call sign)
 *
 * Serial console (115200): help | info | prov | alarm <0-4> | text <msg> |
 *                          ping on|off | heard
 * Every received frame is logged as one "RXLOG," CSV line for range tests.
 */
#include <Arduino.h>
#include <Preferences.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <esp_system.h>

#include "floodmesh_pins.h"
#include "fm_airtime.h"
#include "fm_alarm.h"
#include "fm_auth.h"
#include "fm_dedup.h"
#include "fm_edit.h"
#include "fm_ids.h"
#include "fm_keypad.h"
#include "fm_mesh.h"
#include "fm_ota.h"
#include "fm_packet.h"
#include "fm_prov.h"
#include "fm_radio.h"
#include "fm_role.h"
#include "fm_text.h"

// ---------------------------------------------------------------- build knobs
#ifndef FLOODMESH_VERSION
#define FLOODMESH_VERSION "3.0.0"
#endif
#ifndef ADC_CTRL_ENABLE_LEVEL
#define ADC_CTRL_ENABLE_LEVEL HIGH
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
#ifndef FM_SOS_HOLD_MS
#define FM_SOS_HOLD_MS 3000        // * and # held together this long
#endif
#ifndef FM_SCREEN_OFF_MS
#define FM_SCREEN_OFF_MS 60000     // blank the OLED after this long idle
#endif
#ifndef FM_POPUP_MS
#define FM_POPUP_MS 8000           // how long a received text owns the screen
#endif
#ifndef FM_PING_PERIOD_MS
#define FM_PING_PERIOD_MS 30000    // range-test ping interval
#endif
#ifndef FM_KEY_CLICK_MS
#define FM_KEY_CLICK_MS 15         // 0 = silent keys; active buzzers need ~10 ms to sound
#endif

static const uint32_t kUiPeriodMs = 80;
static const uint32_t kBattPeriodMs = 10000;

// ---------------------------------------------------------------- display
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, PIN_OLED_RST, PIN_OLED_SCL, PIN_OLED_SDA);
static bool g_oledOk = false;
static bool g_screenOff = false;
static uint32_t g_lastActivity = 0;
static uint32_t g_lastDraw = 0;

// Boot request that survives a software reset: set by the serial "prov" command.
#define FM_BOOTREQ_PROV 0x50524F56UL   // "PROV"
static RTC_NOINIT_ATTR uint32_t g_bootReq;

// ---------------------------------------------------------------- state
enum Screen : uint8_t { SCR_HOME, SCR_COMPOSE, SCR_INBOX, SCR_VIEW, SCR_ALERTS, SCR_STATUS };
static Screen g_screen = SCR_HOME;

static float g_battV = 0.0f;
static uint8_t g_battPct = 0;
static uint32_t g_lastBatt = 0;

// Messages: a ring of the last 16, received and sent.
#define MSG_SLOTS 16
struct Msg {
  bool     mine;
  bool     passedOn;     // mine: a neighbour was heard relaying it
  uint8_t  type;         // FM_TYPE_ALARM / FM_TYPE_TEXT
  uint8_t  alarm;
  uint8_t  msgId;
  uint8_t  hops;
  int16_t  rssi;
  float    snr;
  uint32_t at;
  char     from[FM_CALLSIGN_LEN + 1];
  char     text[FM_TEXT_MAX_CHARS + 1];
};
static Msg g_msgs[MSG_SLOTS];
static uint8_t g_msgHead = 0;    // next slot to write
static uint8_t g_msgCount = 0;
static uint8_t g_inboxSel = 0;   // 0 = newest
static uint8_t g_unread = 0;

// Heard table: the last frame from each originator, for range tests.
#define HEARD_SLOTS 16
struct Heard {
  bool     used;
  char     cs[FM_CALLSIGN_LEN + 1];
  uint16_t count;
  uint8_t  hops;
  int16_t  rssi;
  float    snr;
  uint32_t at;
};
static Heard g_heard[HEARD_SLOTS];
static uint8_t g_heardSel = 0;

// Alerts menu (SOS is not in it: SOS is the * + # hold).
static const uint8_t kAlertList[] = {FM_ALARM_SAFE, FM_ALARM_WATER, FM_ALARM_EVACUATE,
                                     FM_ALARM_MEDICAL};
static uint8_t g_alertSel = 0;

// Popup overlay (received message, send result, notices).
static bool g_popup = false;
static bool g_popupSticky = false;   // stays until a key is pressed
static uint32_t g_popupUntil = 0;
static char g_popTitle[24];
static char g_popMeta[40];
static char g_popBody[FM_TEXT_MAX_CHARS + 1];

// SOS chord.
static bool g_chord = false;        // * and # were both held during this hold
static bool g_sosFired = false;

// Range-test ping.
static bool g_ping = false;
static uint32_t g_pingNext = 0;
static uint16_t g_pingSeq = 0;

// ---------------------------------------------------------------- buzzer
// Non-blocking: the mesh keeps running while it beeps.
static uint16_t g_bzPulses = 0, g_bzOn = 0, g_bzOff = 0;
static bool g_bzState = false;
static uint32_t g_bzNext = 0;

static void buzzStart(uint16_t pulses, uint16_t onMs, uint16_t offMs) {
  g_bzPulses = pulses;
  g_bzOn = onMs;
  g_bzOff = offMs;
  g_bzState = false;
  g_bzNext = millis();
}

static void buzzStop() {
  g_bzPulses = 0;
  g_bzState = false;
  digitalWrite(PIN_BUZZER, LOW);
}

static void buzzTick(uint32_t now) {
  if (!g_bzPulses && !g_bzState) return;
  if ((int32_t)(now - g_bzNext) < 0) return;
  if (g_bzState) {
    digitalWrite(PIN_BUZZER, LOW);
    g_bzState = false;
    g_bzNext = now + g_bzOff;
  } else if (g_bzPulses) {
    digitalWrite(PIN_BUZZER, HIGH);
    g_bzState = true;
    g_bzPulses--;
    g_bzNext = now + g_bzOn;
  }
}

static void keyClick() {
  if (FM_KEY_CLICK_MS > 0 && !g_bzPulses && !g_bzState) buzzStart(1, FM_KEY_CLICK_MS, 0);
}

/** Blocking beep, for boot and provisioning only. */
static void beepBlocking(uint16_t onMs, uint8_t pulses = 1, uint16_t gapMs = 60) {
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
  qsort(mv, kSamples, sizeof(int), cmpInt);   // median rejects LoRa/switching spikes
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

static void updateBattery(uint32_t now, bool force) {
  if (!force && (now - g_lastBatt) < kBattPeriodMs) return;
  g_lastBatt = now;
  g_battV = readBatteryVolts();
  g_battPct = batteryPercent(g_battV);
}

// ---------------------------------------------------------------- PSK
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

// ---------------------------------------------------------------- helpers
static void fmtAge(uint32_t ms, char *out, size_t n) {
  const uint32_t s = ms / 1000;
  if (s < 60)        snprintf(out, n, "%lus", (unsigned long)s);
  else if (s < 3600) snprintf(out, n, "%lum", (unsigned long)(s / 60));
  else               snprintf(out, n, "%luh", (unsigned long)(s / 3600));
}

static Msg *msgAt(uint8_t i) {   // 0 = newest
  if (i >= g_msgCount) return nullptr;
  return &g_msgs[(uint8_t)(g_msgHead + MSG_SLOTS - 1 - i) % MSG_SLOTS];
}

static Msg *msgPush() {
  Msg *m = &g_msgs[g_msgHead];
  memset(m, 0, sizeof(*m));
  g_msgHead = (uint8_t)((g_msgHead + 1) % MSG_SLOTS);
  if (g_msgCount < MSG_SLOTS) g_msgCount++;
  if (g_inboxSel && g_inboxSel + 1 < g_msgCount) g_inboxSel++;   // keep the same one selected
  return m;
}

static void heardUpdate(const char *cs, uint8_t hops, float rssi, float snr, uint32_t now) {
  int slot = -1, oldest = 0;
  for (uint8_t i = 0; i < HEARD_SLOTS; i++) {
    if (g_heard[i].used && strncmp(g_heard[i].cs, cs, FM_CALLSIGN_LEN) == 0) { slot = i; break; }
    if (slot < 0 && !g_heard[i].used) { slot = i; break; }
    if (g_heard[i].at < g_heard[oldest].at) oldest = i;
  }
  Heard *h = &g_heard[slot >= 0 ? slot : oldest];
  if (!h->used || strncmp(h->cs, cs, FM_CALLSIGN_LEN) != 0) {
    memset(h, 0, sizeof(*h));
    h->used = true;
    snprintf(h->cs, sizeof(h->cs), "%s", cs);
  }
  h->count++;
  h->hops = hops;
  h->rssi = (int16_t)rssi;
  h->snr = snr;
  h->at = now;
}

/** Heard entries sorted newest first; returns how many. */
static uint8_t heardSorted(uint8_t idx[HEARD_SLOTS]) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < HEARD_SLOTS; i++) {
    if (g_heard[i].used) idx[n++] = i;
  }
  for (uint8_t i = 1; i < n; i++) {
    for (uint8_t j = i; j > 0 && g_heard[idx[j]].at > g_heard[idx[j - 1]].at; j--) {
      const uint8_t t = idx[j];
      idx[j] = idx[j - 1];
      idx[j - 1] = t;
    }
  }
  return n;
}

static void wake(uint32_t now) {
  g_lastActivity = now;
  if (g_screenOff && g_oledOk) {
    oled.setPowerSave(0);
    g_screenOff = false;
  }
}

static void popup(const char *title, const char *meta, const char *body, bool sticky,
                  uint32_t holdMs = FM_POPUP_MS) {
  snprintf(g_popTitle, sizeof(g_popTitle), "%s", title);
  snprintf(g_popMeta, sizeof(g_popMeta), "%s", meta ? meta : "");
  snprintf(g_popBody, sizeof(g_popBody), "%s", body ? body : "");
  g_popup = true;
  g_popupSticky = sticky;
  g_popupUntil = millis() + holdMs;
  wake(millis());
}

/** Greedy word wrap into up to maxLines lines of `cols` characters. */
static uint8_t wordWrap(const char *s, uint8_t cols, char lines[][22], uint8_t maxLines) {
  uint8_t n = 0;
  const size_t len = strlen(s);
  size_t i = 0;
  while (i < len && n < maxLines) {
    while (i < len && s[i] == ' ') i++;
    if (i >= len) break;
    size_t end = i + cols;
    if (end >= len) {
      end = len;
    } else {
      size_t b = end;
      while (b > i && s[b] != ' ') b--;
      if (b > i) end = b;   // break at the last space; else hard-split a long word
    }
    const size_t w = end - i;
    memcpy(lines[n], s + i, w);
    lines[n][w] = '\0';
    n++;
    i = end;
  }
  return n;
}

/** One-line preview: "Me: HELLO", "KXQ2R: !SOS"; with an age, "KXQ2R 5m: ...". */
static void msgPreview(const Msg *m, const char *age, char *out, size_t n) {
  const char *who = m->mine ? "Me" : m->from;
  const char *bang = m->type == FM_TYPE_ALARM ? "!" : "";
  if (age) snprintf(out, n, "%s %s: %s%s", who, age, bang, m->text);
  else     snprintf(out, n, "%s: %s%s", who, bang, m->text);
  if (n > 25) out[25] = '\0';   // 25 columns of the 5x8 font
}

// ---------------------------------------------------------------- mesh callbacks
static void logRx(const char *kind, const FmHeader &h, uint8_t hops, float rssi, float snr,
                  const char *text) {
  // One CSV line per received frame: ms,kind,from,msgId,hops,rssi,snr,text
  Serial.printf("RXLOG,%lu,%s,%s,%u,%u,%.0f,%.1f,%s\n", (unsigned long)millis(), kind,
                h.callSign, (unsigned)h.msgId, (unsigned)hops, rssi, snr, text ? text : "");
}

static void onAlarmRx(const FmAlarmRx *rx) {
  const uint32_t now = millis();
  logRx("ALARM", rx->header, rx->hops, rx->rssi, rx->snr, fmAlarmName(rx->alarmType));
  heardUpdate(rx->header.callSign, rx->hops, rx->rssi, rx->snr, now);

  Msg *m = msgPush();
  m->type = FM_TYPE_ALARM;
  m->alarm = rx->alarmType;
  m->msgId = rx->header.msgId;
  m->hops = rx->hops;
  m->rssi = (int16_t)rx->rssi;
  m->snr = rx->snr;
  m->at = now;
  snprintf(m->from, sizeof(m->from), "%s", rx->header.callSign);
  snprintf(m->text, sizeof(m->text), "%s", fmAlarmName(rx->alarmType));
  g_unread++;

  char meta[40];
  snprintf(meta, sizeof(meta), "%s  %u hop  %d dBm", rx->header.callSign, (unsigned)rx->hops,
           (int)rx->rssi);
  const bool sos = rx->alarmType == FM_ALARM_SOS;
  popup(sos ? "!! SOS !!" : "ALERT", meta, fmAlarmName(rx->alarmType), sos);
  if (sos) buzzStart(60, 700, 300);   // about a minute, or until a key is pressed
  else     buzzStart(3, 150, 100);
}

static void onTextRx(const FmTextRx *rx) {
  const uint32_t now = millis();
  logRx("TEXT", rx->header, rx->hops, rx->rssi, rx->snr, rx->text);
  heardUpdate(rx->header.callSign, rx->hops, rx->rssi, rx->snr, now);

  // Range-test pings update the heard table and the log, not the inbox.
  if (strncmp(rx->text, "PING ", 5) == 0) {
    if (!g_bzPulses) buzzStart(1, 20, 0);
    return;
  }

  Msg *m = msgPush();
  m->type = FM_TYPE_TEXT;
  m->msgId = rx->header.msgId;
  m->hops = rx->hops;
  m->rssi = (int16_t)rx->rssi;
  m->snr = rx->snr;
  m->at = now;
  snprintf(m->from, sizeof(m->from), "%s", rx->header.callSign);
  snprintf(m->text, sizeof(m->text), "%s", rx->text);
  g_unread++;

  char meta[40];
  snprintf(meta, sizeof(meta), "%s  %u hop  %d dBm", rx->header.callSign, (unsigned)rx->hops,
           (int)rx->rssi);
  // Don't yank the screen away from someone typing: beep and count it instead.
  if (g_screen != SCR_COMPOSE) popup("MESSAGE", meta, rx->text, false);
  buzzStart(2, 60, 80);
}

static void onEcho(const FmEchoRx *e) {
  Serial.printf("[ECHO] our %s msg %u relayed by a neighbour (%.0f dBm, SNR %.1f)\n",
                e->type == FM_TYPE_ALARM ? "alarm" : "text", (unsigned)e->msgId, e->rssi,
                e->snr);
  for (uint8_t i = 0; i < g_msgCount; i++) {
    Msg *m = msgAt(i);
    if (m->mine && m->msgId == e->msgId && m->type == e->type && !m->passedOn) {
      m->passedOn = true;
      m->rssi = (int16_t)e->rssi;
      m->snr = e->snr;
      break;
    }
  }
}

// ---------------------------------------------------------------- sending
static bool sendAlarm(uint8_t a) {
  const int id = fmMeshSendAlarm(a);
  Serial.printf("[ALARM] send %s -> %s\n", fmAlarmName(a), id >= 0 ? "sent" : "FAILED");
  if (id < 0) {
    popup("SEND FAILED", "radio busy or down", fmAlarmName(a), false, 4000);
    buzzStart(1, 600, 0);
    return false;
  }
  Msg *m = msgPush();
  m->mine = true;
  m->type = FM_TYPE_ALARM;
  m->alarm = a;
  m->msgId = (uint8_t)id;
  m->at = millis();
  snprintf(m->from, sizeof(m->from), "%s", fmCallSign());
  snprintf(m->text, sizeof(m->text), "%s", fmAlarmName(a));
  popup(a == FM_ALARM_SOS ? "SOS SENT" : "ALERT SENT", "watch inbox for 'passed on'",
        fmAlarmName(a), false, 4000);
  buzzStart(2, 80, 60);
  return true;
}

static bool sendText(const char *text, bool quiet = false) {
  const int id = fmMeshSendText(text);
  Serial.printf("[TEXT] send \"%s\" -> %s\n", text, id >= 0 ? "sent" : "FAILED");
  if (id < 0) {
    if (!quiet) {
      popup("NOT SENT", "airtime limit or radio busy", text, false, 4000);
      buzzStart(1, 600, 0);
    }
    return false;
  }
  if (quiet) return true;
  Msg *m = msgPush();
  m->mine = true;
  m->type = FM_TYPE_TEXT;
  m->msgId = (uint8_t)id;
  m->at = millis();
  snprintf(m->from, sizeof(m->from), "%s", fmCallSign());
  snprintf(m->text, sizeof(m->text), "%s", text);
  popup("SENT", "watch inbox for 'passed on'", text, false, 3000);
  buzzStart(1, 80, 0);
  return true;
}

static void pingTick(uint32_t now) {
  if (!g_ping || (int32_t)(now - g_pingNext) < 0) return;
  g_pingNext = now + FM_PING_PERIOD_MS;
  char t[32];
  snprintf(t, sizeof(t), "PING %u B%u", (unsigned)++g_pingSeq, (unsigned)g_battPct);
  sendText(t, true);
}

static void setPing(bool on) {
  g_ping = on;
  g_pingNext = millis();
  Serial.printf("[PING] range-test ping %s (every %lu s)\n", on ? "ON" : "OFF",
                (unsigned long)(FM_PING_PERIOD_MS / 1000));
}

// ---------------------------------------------------------------- drawing
static void drawHeader(const char *title) {
  oled.setDrawColor(1);
  oled.drawBox(0, 0, 128, 11);
  oled.setDrawColor(0);
  oled.setFont(u8g2_font_5x8_tf);
  oled.drawStr(2, 8, title);
  char r[16];
  snprintf(r, sizeof(r), "%u%%", (unsigned)g_battPct);
  oled.drawStr(126 - oled.getStrWidth(r), 8, r);
  oled.setDrawColor(1);
}

static void drawFooter(const char *s) {
  oled.setFont(u8g2_font_4x6_tf);
  oled.drawStr(0, 63, s);
}

static void drawHome(uint32_t now) {
  char t[32];
  snprintf(t, sizeof(t), "%s %s", fmCallSign(), fmRoleTag(fmRole()));
  drawHeader(t);
  oled.setFont(u8g2_font_5x8_tf);
  snprintf(t, sizeof(t), "%.2fV  air %u.%u%%%s", g_battV, fmAirtimePermille() / 10,
           fmAirtimePermille() % 10, g_ping ? "  PING" : "");
  oled.drawStr(0, 20, t);
  if (fmRole() == FM_ROLE_RESPONDER) {
    snprintf(t, sizeof(t), "responder %lud left",
             (unsigned long)((fmRoleRemainingS() + 86399UL) / 86400UL));
    oled.drawStr(0, 29, t);
  }
  oled.drawHLine(0, 31, 128);
  if (g_msgCount == 0) {
    oled.drawStr(0, 42, "no messages yet");
    oled.drawStr(0, 51, "type 0-9 to write");
  } else {
    for (uint8_t i = 0; i < 2 && i < g_msgCount; i++) {
      const Msg *m = msgAt(i);
      msgPreview(m, nullptr, t, sizeof(t));
      oled.drawStr(0, 41 + i * 10, t);
    }
    if (g_unread) {
      snprintf(t, sizeof(t), "%u new", (unsigned)g_unread);
      oled.drawStr(128 - oled.getStrWidth(t), 20, t);
    }
  }
  drawFooter("A:ALERT B:STAT C:INBOX *+#:SOS");
}

static void drawCompose(uint32_t now) {
  char t[32];
  uint8_t ci = 0, cn = 0;
  fmEditCandidateInfo(&ci, &cn);
  if (fmEditMode() == FM_EDIT_T9 && cn > 1) {
    snprintf(t, sizeof(t), "%s %u/%u  %u/%u", fmEditModeName(), (unsigned)ci + 1, (unsigned)cn,
             (unsigned)fmEditLength(), (unsigned)FM_TEXT_MAX_CHARS);
  } else {
    snprintf(t, sizeof(t), "%s  %u/%u", fmEditModeName(), (unsigned)fmEditLength(),
             (unsigned)FM_TEXT_MAX_CHARS);
  }
  drawHeader(t);

  char buf[FM_TEXT_MAX_CHARS + 16];
  size_t pend = 0;
  fmEditRender(buf, sizeof(buf), &pend);
  const size_t len = strlen(buf);
  const uint8_t cols = 21;
  oled.setFont(u8g2_font_6x10_tf);
  // Show the last 4 lines so the cursor is always visible.
  const size_t totalLines = len / cols + 1;
  const size_t first = totalLines > 4 ? totalLines - 4 : 0;
  for (size_t l = first; l < totalLines; l++) {
    const uint8_t y = (uint8_t)(22 + (l - first) * 10);
    for (size_t c = 0; c < cols; c++) {
      const size_t i = l * cols + c;
      if (i >= len) break;
      const char s[2] = {buf[i], '\0'};
      oled.drawStr((uint8_t)(c * 6), y, s);
      if (i >= pend) oled.drawHLine((uint8_t)(c * 6), y + 1, 6);   // word in progress
    }
  }
  if ((now / 450) % 2 == 0) {   // blinking cursor
    const size_t l = len / cols, c = len % cols;
    if (l >= first) oled.drawVLine((uint8_t)(c * 6), (uint8_t)(22 + (l - first) * 10 - 8), 9);
  }
  drawFooter("C:OK holdC:SEND D:DEL *:WORD #:MODE");
}

static void drawInbox(uint32_t now) {
  char t[40];
  snprintf(t, sizeof(t), "INBOX %u", (unsigned)g_msgCount);
  drawHeader(t);
  oled.setFont(u8g2_font_5x8_tf);
  if (g_msgCount == 0) {
    oled.drawStr(0, 30, "empty");
  } else {
    const uint8_t rows = 5;
    const uint8_t top = g_inboxSel >= rows ? (uint8_t)(g_inboxSel - rows + 1) : 0;
    for (uint8_t r = 0; r < rows && top + r < g_msgCount; r++) {
      const Msg *m = msgAt(top + r);
      char age[8];
      fmtAge(now - m->at, age, sizeof(age));
      msgPreview(m, age, t, sizeof(t));
      const uint8_t y = (uint8_t)(20 + r * 9);
      if (top + r == g_inboxSel) {
        oled.drawBox(0, y - 8, 128, 9);
        oled.setDrawColor(0);
        oled.drawStr(1, y, t);
        oled.setDrawColor(1);
      } else {
        oled.drawStr(1, y, t);
      }
    }
  }
}

static void drawView(uint32_t now) {
  const Msg *m = msgAt(g_inboxSel);
  if (!m) { g_screen = SCR_INBOX; return; }
  char t[40], age[8];
  fmtAge(now - m->at, age, sizeof(age));
  snprintf(t, sizeof(t), "%s %s %s", m->mine ? "TO ALL" : m->from,
           m->type == FM_TYPE_ALARM ? "ALERT" : "", age);
  drawHeader(t);
  char lines[4][22];
  const uint8_t n = wordWrap(m->text, 21, lines, 4);
  oled.setFont(u8g2_font_6x10_tf);
  for (uint8_t i = 0; i < n; i++) oled.drawStr(0, (uint8_t)(22 + i * 10), lines[i]);
  if (m->mine) {
    if (m->passedOn) {
      snprintf(t, sizeof(t), "passed on: %d dBm SNR %.1f", m->rssi, m->snr);
    } else {
      snprintf(t, sizeof(t), "not heard relayed yet");
    }
  } else {
    snprintf(t, sizeof(t), "%u hop  %d dBm  SNR %.1f", (unsigned)m->hops, m->rssi, m->snr);
  }
  drawFooter(t);
}

static void drawAlerts() {
  drawHeader("SEND ALERT");
  oled.setFont(u8g2_font_6x10_tf);
  const uint8_t n = sizeof(kAlertList);
  for (uint8_t i = 0; i < n; i++) {
    const uint8_t y = (uint8_t)(22 + i * 10);
    if (i == g_alertSel) {
      oled.drawBox(0, y - 9, 128, 10);
      oled.setDrawColor(0);
      oled.drawStr(2, y, fmAlarmName(kAlertList[i]));
      oled.setDrawColor(1);
    } else {
      oled.drawStr(2, y, fmAlarmName(kAlertList[i]));
    }
  }
  drawFooter("A/B:CHOOSE  hold C:SEND  D:BACK");
}

static void drawStatus(uint32_t now) {
  char t[40];
  snprintf(t, sizeof(t), "STATUS %s", g_ping ? "PING ON" : "");
  drawHeader(t);
  oled.setFont(u8g2_font_5x8_tf);
  snprintf(t, sizeof(t), "air %u.%u%% relay %lu sup %lu q%u", fmAirtimePermille() / 10,
           fmAirtimePermille() % 10, (unsigned long)fmMeshFramesRelayed(),
           (unsigned long)fmMeshFramesSuppressed(), (unsigned)fmMeshRelayQueueDepth());
  oled.drawStr(0, 19, t);
  oled.setFont(u8g2_font_4x6_tf);
  oled.drawStr(0, 27, "HEARD  hop  dBm   SNR  age   n");
  oled.setFont(u8g2_font_5x8_tf);
  uint8_t idx[HEARD_SLOTS];
  const uint8_t n = heardSorted(idx);
  if (n == 0) oled.drawStr(0, 37, "nobody yet");
  if (g_heardSel >= n && n) g_heardSel = n - 1;
  const uint8_t top = g_heardSel >= 3 ? (uint8_t)(g_heardSel - 2) : 0;
  for (uint8_t r = 0; r < 3 && top + r < n; r++) {
    const Heard &h = g_heard[idx[top + r]];
    char age[8];
    fmtAge(now - h.at, age, sizeof(age));
    snprintf(t, sizeof(t), "%-5s %u %4d %5.1f %4s %u", h.cs, (unsigned)h.hops, h.rssi, h.snr,
             age, (unsigned)h.count);
    oled.drawStr(0, (uint8_t)(36 + r * 9), t);
  }
  drawFooter(g_ping ? "C:PING OFF  A/B:SCROLL  D:BACK" : "C:PING ON  A/B:SCROLL  D:BACK");
}

static void drawPopup() {
  drawHeader(g_popTitle);
  oled.setFont(u8g2_font_5x8_tf);
  oled.drawStr(0, 20, g_popMeta);
  char lines[4][22];
  const uint8_t n = wordWrap(g_popBody, 21, lines, 4);
  oled.setFont(u8g2_font_6x10_tf);
  for (uint8_t i = 0; i < n; i++) oled.drawStr(0, (uint8_t)(31 + i * 10), lines[i]);
  if (g_popupSticky) drawFooter("press any key");
}

static void drawSosHold(uint32_t heldMs) {
  drawHeader("SOS");
  oled.setFont(u8g2_font_7x13B_tf);
  if (g_sosFired) {
    oled.drawStr(0, 32, "SOS SENT");
    oled.setFont(u8g2_font_5x8_tf);
    oled.drawStr(0, 48, "release the keys");
  } else {
    oled.drawStr(0, 30, "KEEP HOLDING");
    const uint32_t w = heldMs >= FM_SOS_HOLD_MS ? 124 : heldMs * 124 / FM_SOS_HOLD_MS;
    oled.drawFrame(0, 38, 128, 12);
    if (w) oled.drawBox(2, 40, (uint8_t)w, 8);
    oled.setFont(u8g2_font_5x8_tf);
    oled.drawStr(0, 60, "release to cancel");
  }
}

static void draw(uint32_t now, bool force) {
  if (!g_oledOk || g_screenOff) return;
  if (!force && (now - g_lastDraw) < kUiPeriodMs) return;
  g_lastDraw = now;
  oled.clearBuffer();
  const bool chordHeld = fmKeypadHeld(FM_KP_STAR) && fmKeypadHeld(FM_KP_HASH);
  const uint32_t held = chordHeld ? min(fmKeypadHeldMs(FM_KP_STAR, now),
                                        fmKeypadHeldMs(FM_KP_HASH, now))
                                  : 0;
  if (chordHeld && (held >= 250 || g_sosFired)) {
    drawSosHold(held);
  } else if (g_popup) {
    drawPopup();
  } else {
    switch (g_screen) {
      case SCR_HOME:    drawHome(now); break;
      case SCR_COMPOSE: drawCompose(now); break;
      case SCR_INBOX:   drawInbox(now); break;
      case SCR_VIEW:    drawView(now); break;
      case SCR_ALERTS:  drawAlerts(); break;
      case SCR_STATUS:  drawStatus(now); break;
    }
  }
  oled.sendBuffer();
}

// ---------------------------------------------------------------- keys
static bool isDigitKey(uint8_t k) {
  return k != FM_KP_A && k != FM_KP_B && k != FM_KP_C && k != FM_KP_D && k != FM_KP_STAR &&
         k != FM_KP_HASH;
}

static void handleCompose(const FmKeyEvent &ev, uint32_t now) {
  const uint8_t k = ev.key;
  if (ev.type == FM_KEV_PRESS) {
    if (isDigitKey(k)) fmEditKey(k, now);
    else if (k == FM_KP_A) fmEditCycleCandidate(-1);
    else if (k == FM_KP_B) fmEditCycleCandidate(+1);
  } else if (ev.type == FM_KEV_LONG) {
    if (k == FM_KP_C) {
      char out[FM_TEXT_MAX_CHARS + 1];
      fmEditFinish(out, sizeof(out));
      if (!out[0]) return;
      if (sendText(out)) {
        fmEditClear();
        g_screen = SCR_HOME;
      }
    } else if (k == FM_KP_D) {
      fmEditClear();
      g_screen = SCR_HOME;
      buzzStart(1, 150, 0);
    }
  } else if (ev.type == FM_KEV_RELEASE && !ev.wasLong) {
    // * and # act on release so an SOS chord never also edits the text.
    if ((k == FM_KP_STAR || k == FM_KP_HASH) && !g_chord) fmEditKey(k, now);
    else if (k == FM_KP_C) fmEditAccept();
    else if (k == FM_KP_D) {
      if (fmEditIsEmpty()) g_screen = SCR_HOME;
      else fmEditBackspace();
    }
  }
}

static void handleKey(const FmKeyEvent &ev, uint32_t now) {
  if (ev.type == FM_KEV_NONE) return;
  const bool press = ev.type == FM_KEV_PRESS;

  if (press) {
    keyClick();
    // A press that wakes the screen or dismisses a popup does nothing else.
    if (g_screenOff) { wake(now); return; }
    g_lastActivity = now;
    if (g_popup) {
      g_popup = false;
      buzzStop();
      return;
    }
  }

  switch (g_screen) {
    case SCR_HOME:
      if (!press) break;
      if (isDigitKey(ev.key)) {
        fmEditClear();
        fmEditKey(ev.key, now);
        g_screen = SCR_COMPOSE;
      } else if (ev.key == FM_KP_A) {
        g_alertSel = 0;
        g_screen = SCR_ALERTS;
      } else if (ev.key == FM_KP_B) {
        g_heardSel = 0;
        g_screen = SCR_STATUS;
      } else if (ev.key == FM_KP_C) {
        g_inboxSel = 0;
        g_unread = 0;
        g_screen = SCR_INBOX;
      }
      break;

    case SCR_COMPOSE:
      handleCompose(ev, now);
      break;

    case SCR_INBOX:
      if (!press) break;
      if (ev.key == FM_KP_A && g_inboxSel > 0) g_inboxSel--;
      else if (ev.key == FM_KP_B && g_inboxSel + 1 < g_msgCount) g_inboxSel++;
      else if (ev.key == FM_KP_C && g_msgCount) g_screen = SCR_VIEW;
      else if (ev.key == FM_KP_D) g_screen = SCR_HOME;
      break;

    case SCR_VIEW:
      if (!press) break;
      if (ev.key == FM_KP_A && g_inboxSel > 0) g_inboxSel--;
      else if (ev.key == FM_KP_B && g_inboxSel + 1 < g_msgCount) g_inboxSel++;
      else if (ev.key == FM_KP_D || ev.key == FM_KP_C) g_screen = SCR_INBOX;
      break;

    case SCR_ALERTS:
      if (press && ev.key == FM_KP_A && g_alertSel > 0) g_alertSel--;
      else if (press && ev.key == FM_KP_B && g_alertSel + 1u < sizeof(kAlertList)) g_alertSel++;
      else if (press && ev.key == FM_KP_D) g_screen = SCR_HOME;
      else if (ev.type == FM_KEV_LONG && ev.key == FM_KP_C) {
        if (sendAlarm(kAlertList[g_alertSel])) g_screen = SCR_HOME;
      }
      break;

    case SCR_STATUS:
      if (!press) break;
      if (ev.key == FM_KP_A && g_heardSel > 0) g_heardSel--;
      else if (ev.key == FM_KP_B) g_heardSel++;   // clamped when drawn
      else if (ev.key == FM_KP_C) setPing(!g_ping);
      else if (ev.key == FM_KP_D) g_screen = SCR_HOME;
      break;
  }
}

/** * and # held together: SOS after FM_SOS_HOLD_MS, from any screen. */
static void sosTick(uint32_t now) {
  const bool star = fmKeypadHeld(FM_KP_STAR), hash = fmKeypadHeld(FM_KP_HASH);
  if (star && hash) {
    if (!g_chord) {
      g_chord = true;
      g_sosFired = false;
      wake(now);
    }
    const uint32_t held = min(fmKeypadHeldMs(FM_KP_STAR, now), fmKeypadHeldMs(FM_KP_HASH, now));
    if (!g_sosFired && held >= FM_SOS_HOLD_MS) {
      g_sosFired = true;
      Serial.println("[ALARM] SOS >> * + # held <<");
      sendAlarm(FM_ALARM_SOS);
      g_popup = false;   // the hold screen shows "SOS SENT" until release
    }
  } else if (!star && !hash) {
    g_chord = false;
  }
}

// ---------------------------------------------------------------- provisioning / boot
static void provDraw(const char *l1, const char *l2, const char *l3, const char *l4) {
  if (!g_oledOk) return;
  oled.clearBuffer();
  oled.drawBox(0, 0, 128, 13);
  oled.setDrawColor(0);
  oled.setFont(u8g2_font_7x13B_tf);
  oled.drawStr(2, 11, l1);
  oled.setDrawColor(1);
  oled.drawStr(0, 28, l2);
  oled.setFont(u8g2_font_5x8_tf);
  oled.drawStr(0, 42, l3);
  oled.drawStr(0, 54, l4);
  oled.setFont(u8g2_font_4x6_tf);
  oled.drawStr(0, 63, "open FloodMesh Admin app");
  oled.sendBuffer();
}

static void factoryReset() {
  fmRoleFactoryReset();
  Preferences p;
  if (p.begin(FM_NVS_NAMESPACE, false)) {
    p.remove(FM_NVS_KEY_CALLSIGN);   // back to the MAC-derived call sign
    p.end();
  }
  fmOtaForget();                     // and no stored WiFi network
}

static bool g_otaAtBoot = false;   // B held at power-on: open the WiFi update window

static void bootCombos() {
  bool wantProv = (g_bootReq == FM_BOOTREQ_PROV);
  g_bootReq = 0;
  const uint16_t keys = fmKeypadScanRaw();
  const uint16_t kReset = FM_KP_BIT(FM_KP_STAR) | FM_KP_BIT(FM_KP_0);

  if ((keys & kReset) == kReset) {
    Serial.println("[BOOT] * + 0 held: factory reset in 10 s - release to cancel");
    const uint32_t t0 = millis();
    bool held = true;
    while (millis() - t0 < 10000UL) {
      if ((fmKeypadScanRaw() & kReset) != kReset) { held = false; break; }
      char l[24];
      snprintf(l, sizeof(l), "in %lu s", (unsigned long)(10 - (millis() - t0) / 1000));
      provDraw("FACTORY RESET", l, "release to cancel", "");
      delay(200);
    }
    if (held) {
      factoryReset();
      provDraw("FACTORY RESET", "done", "rebooting...", "");
      beepBlocking(500);
      delay(800);
      ESP.restart();
    }
    Serial.println("[BOOT] factory reset cancelled");
  }
  if (keys & FM_KP_BIT(FM_KP_HASH)) wantProv = true;
  if (keys & FM_KP_BIT(FM_KP_B)) {
    Serial.println("[BOOT] B held: WiFi update window");
    g_otaAtBoot = true;
  }

  if (wantProv) {
    beepBlocking(40, 3, 60);
    fmProvRun(provDraw);   // never returns
  }
}

// ---------------------------------------------------------------- serial console
static void printHeard() {
  uint8_t idx[HEARD_SLOTS];
  const uint8_t n = heardSorted(idx);
  Serial.printf("[HEARD] %u station(s)\n", (unsigned)n);
  for (uint8_t i = 0; i < n; i++) {
    const Heard &h = g_heard[idx[i]];
    Serial.printf("  %-5s hops %u  %4d dBm  SNR %5.1f  %lus ago  x%u\n", h.cs, (unsigned)h.hops,
                  h.rssi, h.snr, (unsigned long)((millis() - h.at) / 1000), (unsigned)h.count);
  }
}

static void pollSerial() {
  static char line[96];
  static size_t n = 0;
  static bool overflow = false;
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (n + 1 < sizeof(line)) line[n++] = c;
      else overflow = true;
      continue;
    }
    line[n] = '\0';
    n = 0;
    if (overflow) {
      // Never run a truncated command: a clipped WiFi password would be stored
      // and fail later with no clue why. And never echo it back.
      overflow = false;
      memset(line, 0, sizeof(line));
      Serial.printf("[CMD] line longer than %u characters - ignored\n",
                    (unsigned)(sizeof(line) - 1));
      continue;
    }
    if (strcmp(line, "help") == 0) {
      Serial.println("[CMD] help | info | prov | alarm <0-4> | text <msg> | ping on|off | heard"
                     " | beep | callsign <1-5 of A-Z 0-9> | wifi | wifi ssid <name>"
                     " | wifi pass <pw> | wifi forget | ota | ota off");
    } else if (strncmp(line, "wifi", 4) == 0) {
      // Every "wifi..." line ends here, so a mistyped one can never reach the
      // "unknown" branch below, which would echo a password to the log.
      if (strncmp(line, "wifi ssid ", 10) == 0) {
        if (fmOtaSetSsid(line + 10)) Serial.printf("[WIFI] network set to '%s'\n", fmOtaSsid());
        else Serial.println("[WIFI] network refused - 1 to 32 printable characters");
      } else if (strncmp(line, "wifi pass ", 10) == 0 || strcmp(line, "wifi pass") == 0) {
        if (fmOtaSetPass(line[9] ? line + 10 : "")) Serial.println("[WIFI] password saved");
        else Serial.println("[WIFI] password refused - 8 to 63 printable characters");
      } else if (strcmp(line, "wifi forget") == 0) {
        fmOtaForget();
        Serial.println("[WIFI] network forgotten");
      } else if (strcmp(line, "wifi") == 0) {
        Serial.printf("[WIFI] network %s, password %s, update window %s\n",
                      fmOtaHasSsid() ? fmOtaSsid() : "not set",
                      fmOtaHasPass() ? "saved" : "not set", fmOtaActive() ? "OPEN" : "closed");
      } else {
        Serial.println("[WIFI] usage: wifi | wifi ssid <name> | wifi pass <pw> | wifi forget");
      }
      memset(line, 0, sizeof(line));
    } else if (strcmp(line, "ota") == 0) {
      if (fmOtaStart(FM_OTA_WINDOW_MS)) popup("UPDATE MODE", "joining WiFi", fmOtaSsid(), false, 30000);
    } else if (strcmp(line, "ota off") == 0) {
      fmOtaStop();
    } else if (strcmp(line, "info") == 0) {
      char fp[17];
      fmAdminFingerprint(fp);
      Serial.printf("[CMD] %s v%s role=%s left=%lus batt=%.2fV air=%u permille "
                    "relayed=%lu suppressed=%lu admin=%s\n",
                    fmCallSign(), FLOODMESH_VERSION, fmRoleName(fmRole()),
                    (unsigned long)fmRoleRemainingS(), g_battV, fmAirtimePermille(),
                    (unsigned long)fmMeshFramesRelayed(),
                    (unsigned long)fmMeshFramesSuppressed(), fp[0] ? fp : "none");
    } else if (strncmp(line, "callsign ", 9) == 0) {
      // Operator override, stored in NVS; * + 0 factory reset returns to the MAC one.
      if (fmSetCallSign(line + 9)) Serial.printf("[CMD] call sign now %s\n", fmCallSign());
      else Serial.printf("[CMD] callsign refused - 1-5 characters, A-Z 0-9 only, still %s\n",
                         fmCallSign());
    } else if (strcmp(line, "prov") == 0) {
      Serial.println("[CMD] rebooting into provisioning mode");
      g_bootReq = FM_BOOTREQ_PROV;
      delay(100);
      ESP.restart();
    } else if (strncmp(line, "alarm ", 6) == 0) {
      const int a = atoi(line + 6);
      if (a >= 0 && a < (int)FM_ALARM_COUNT) sendAlarm((uint8_t)a);
      else Serial.println("[CMD] alarm: 0 SAFE, 1 MEDICAL, 2 WATER, 3 EVACUATE, 4 SOS");
    } else if (strncmp(line, "text ", 5) == 0) {
      sendText(line + 5);
    } else if (strcmp(line, "ping on") == 0) {
      setPing(true);
    } else if (strcmp(line, "ping off") == 0) {
      setPing(false);
    } else if (strcmp(line, "beep") == 0) {
      // Buzzer bench check: GPIO held HIGH for 3 s, long enough to measure.
      Serial.printf("[BUZZ] GPIO %d HIGH for 3 s - measure base and collector now\n",
                    PIN_BUZZER);
      buzzStop();
      digitalWrite(PIN_BUZZER, HIGH);
      delay(3000);
      digitalWrite(PIN_BUZZER, LOW);
      Serial.println("[BUZZ] off");
    } else if (strcmp(line, "heard") == 0) {
      printHeard();
    } else if (line[0]) {
      Serial.printf("[CMD] unknown '%s' - try help\n", line);
    }
  }
}

// ---------------------------------------------------------------- setup / loop
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  pinMode(PIN_VEXT_CTRL, OUTPUT);   // Vext powers the OLED
  digitalWrite(PIN_VEXT_CTRL, VEXT_ON);
  delay(120);

  pinMode(PIN_ADC_CTRL, OUTPUT);
  digitalWrite(PIN_ADC_CTRL, !ADC_CTRL_ENABLE_LEVEL);
  analogSetPinAttenuation(PIN_VBAT_ADC, ADC_11db);

  Serial.println();
  Serial.println("=========================================================");
  Serial.printf("  FloodMesh firmware V3 %s - keypad + buzzer, no voice\n", FLOODMESH_VERSION);
  Serial.println("=========================================================");

  pinMode(PIN_OLED_RST, OUTPUT);
  digitalWrite(PIN_OLED_RST, LOW);
  delay(20);
  digitalWrite(PIN_OLED_RST, HIGH);
  delay(20);
  // Probe first: U8g2's begin() hangs silently on a bus that never ACKs.
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL, 400000);
  Wire.setTimeOut(50);
  Wire.beginTransmission(OLED_I2C_ADDR);
  const bool oledAck = Wire.endTransmission() == 0;
  Wire.end();   // U8g2 calls Wire.begin() itself; a second begin hangs boot
  if (oledAck) {
    oled.setI2CAddress(OLED_I2C_ADDR << 1);
    oled.setBusClock(400000);
    g_oledOk = oled.begin();
  }
  Serial.printf("[OLED] %s\n", g_oledOk ? "ok" : "NOT FOUND");
  if (g_oledOk) {
    oled.clearBuffer();
    oled.setFont(u8g2_font_7x13B_tf);
    oled.drawStr(6, 28, "FloodMesh V3");
    oled.setFont(u8g2_font_5x8_tf);
    oled.drawStr(6, 44, FLOODMESH_VERSION);
    oled.sendBuffer();
  }

  fmKeypadBegin();
  fmIdsBegin();
  Serial.printf("[ID] call sign %s (%s)\n", fmCallSign(),
                fmCallSignIsDerived() ? "derived from MAC" : "operator-assigned");
  fmRoleBegin();
  bootCombos();   // may enter provisioning mode, which never returns
  fmOtaBegin(provDraw);
  const bool otaResume = fmOtaResumePending();   // read once: it clears the flag

  uint8_t psk[16];
  if (!parsePsk(psk)) {
    Serial.println("[AUTH] !! FM_PSK_HEX is malformed - must be 32 hex characters.");
    memset(psk, 0, sizeof(psk));
  }
  fmAuthBegin(psk);
  if (pskIsPlaceholder(psk)) {
    Serial.println("[AUTH] !! USING THE PLACEHOLDER KEY FROM THE SOURCE TREE.");
    Serial.println("[AUTH] !! Fine for bench tests. Set -D FM_PSK_HEX before deployment.");
  }

  fmAirtimeBegin();
  fmDedupBegin();
  fmEditClear();

  if (!fmRadioBegin()) {
    Serial.println("[RADIO] !! SX1262 init FAILED - the device cannot reach the mesh.");
    beepBlocking(400, 3, 150);
  } else {
    fmMeshSetAlarmHandler(onAlarmRx);
    fmMeshSetTextHandler(onTextRx);
    fmMeshSetEchoHandler(onEcho);
    fmMeshBegin();
  }

  updateBattery(millis(), true);
  Serial.printf("[BATT] %.3f V (%u%%)\n", g_battV, g_battPct);
  Serial.println("[CMD] type 'help' for serial commands");
  beepBlocking(30, 2, 60);
  g_lastActivity = millis();

  if (otaResume || g_otaAtBoot) {
    if (otaResume) Serial.println("[OTA] rebooted after an update - reopening the window");
    if (fmOtaStart(otaResume ? FM_OTA_RESUME_MS : FM_OTA_WINDOW_MS)) {
      popup("UPDATE MODE", "joining WiFi", fmOtaSsid(), false, 30000);
    } else {
      popup("UPDATE MODE", "cannot open", fmOtaHasSsid() ? "see serial" : "no WiFi network stored",
            false);
    }
  }
}

void loop() {
  const uint32_t now = millis();

  pollSerial();
  fmMeshLoop(now);

  switch (fmOtaLoop(now)) {
    case FM_OTA_EV_JOINED: {
      char meta[40], body[40];
      snprintf(meta, sizeof(meta), "%s %s", fmOtaIp(), fmOtaHost());
      snprintf(body, sizeof(body), "Open %lu min for WiFi updates.",
               (unsigned long)fmOtaMinutesLeft(now));
      popup("UPDATE MODE", meta, body, false, 30000);
      break;
    }
    case FM_OTA_EV_FAILED:
      popup("UPDATE MODE", "WiFi join failed", "Hotspot on? 2.4 GHz? Password right?", false);
      break;
    case FM_OTA_EV_CLOSED:
      popup("UPDATE MODE", "closed", "WiFi is off again.", false, 4000);
      break;
    default:
      break;
  }

  if (fmRoleLoop(now)) {
    popup("ROLE EXPIRED", "responder grant ended", "Now a civilian unit. Renew in the admin app.",
          true);
    buzzStart(3, 300, 200);
  }

  const FmKeyEvent ev = fmKeypadPoll(now);
  handleKey(ev, now);
  sosTick(now);
  if (g_screen == SCR_COMPOSE) fmEditTick(now);

  if (g_popup && !g_popupSticky && (int32_t)(now - g_popupUntil) >= 0) g_popup = false;
  if (!g_popup && !g_bzPulses && !g_screenOff && g_oledOk &&
      (now - g_lastActivity) >= FM_SCREEN_OFF_MS && g_screen != SCR_COMPOSE) {
    oled.setPowerSave(1);
    g_screenOff = true;
  }

  buzzTick(now);
  pingTick(now);
  updateBattery(now, false);
  digitalWrite(PIN_LED, g_ping && ((now / 100) % 20 == 0) ? HIGH : LOW);
  draw(now, false);
}
