/**
 * FloodMesh firmware V4 - Heltec WiFi LoRa 32 V3 + 4x4 keypad + active buzzer.
 *
 * The field-test build carrying the architecture decisions of 27 September
 * 2026 (docs/architecture.md §13): SOS channels chosen by the sender, SOS
 * shown only on responder units, delivery ACKs and "Help coming", SOS retries,
 * powered units and heartbeats, the text-forwarding rule, and light sleep with
 * radio and keypad wake. No voice, no MCP23017, no side button. V3 is kept in
 * firmware_v3/ for comparison; this tree is built on its own
 * (pio run -d firmware_v4 -e v4).
 *
 * Keys (4x4 membrane:  1 2 3 A / 4 5 6 B / 7 8 9 C / * 0 # D)
 * ---------------------------------------------------------------------------
 *   anywhere   hold * and # together 3 s  -> SOS channel screen:
 *              1 MED, 2 EVAC, 3 HAZ, 4 FOOD, 0 GENERAL; GENERAL after 5 s;
 *              D cancels
 *   HOME       0-9 start a text (the key is typed)
 *              A SOS list (responder units), B status, C inbox
 *              hold D = stop your own SOS retries
 *   COMPOSE    2-9 letters, 0 space, 1 punctuation
 *              * next T9 word, # mode T9 / ABC / 123
 *              A / B previous / next T9 word
 *              C accept word, hold C = SEND
 *              D delete (leaves when empty), hold D = discard
 *   INBOX      A / B scroll, C open, D back
 *   SOS LIST   0-4 jump to a channel tab, hold 0-4 for 1 s = select / unselect
 *              A / B move, C open, D back (tab -> main list -> home)
 *   SOS VIEW   hold 4 and 6 together 3 s = HELP IS COMING (bar fills;
 *              release early to cancel), D back
 *   STATUS     A / B scroll heard list, C ping on/off, # power mode,
 *              * powered units (coverage), D back
 *
 * Boot combinations:
 *   hold #                  -> BLE provisioning (FloodMesh Admin app)
 *   hold B                  -> WiFi update window
 *   hold * and 0 for 10 s   -> factory reset (role, keys, call sign, WiFi, SOS channels)
 *
 * Serial console (115200): type "help". Every received frame is logged as
 * one "RXLOG," CSV line for range tests.
 *
 * Field logger (fm_log.h, docs/field-logger-protocol.md): every frame received,
 * sent and relayed, SOS state changes and a status record every 5 min go to
 * flash and are uploaded over WiFi every 5 min. The Status screen shows how
 * many records are waiting; "log" on serial says more.
 */
#include <Arduino.h>
#include <Preferences.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <esp_ota_ops.h>
#include <esp_random.h>
#include <esp_system.h>

#include "floodmesh_pins.h"
#include "fm_airtime.h"
#include "fm_alarm.h"
#include "fm_auth.h"
#include "fm_dedup.h"
#include "fm_edit.h"
#include "fm_ids.h"
#include "fm_keypad.h"
#include "fm_log.h"
#include "fm_mesh.h"
#include "fm_ota.h"
#include "fm_packet.h"
#include "fm_power.h"
#include "fm_prov.h"
#include "fm_radio.h"
#include "fm_role.h"
#include "fm_sleep.h"
#include "fm_sos.h"
#include "fm_text.h"

// ---------------------------------------------------------------- build knobs
#ifndef FLOODMESH_VERSION
#define FLOODMESH_VERSION "4.2.0"
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
#ifndef FM_SOS_PICK_MS
#define FM_SOS_PICK_MS 5000        // channel screen: GENERAL goes out after this long
#endif
#ifndef FM_SOS_TOGGLE_MS
#define FM_SOS_TOGGLE_MS 1000      // hold a digit on the SOS list to (un)select a channel
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
#ifndef FM_HEARTBEAT_MS
#define FM_HEARTBEAT_MS (30UL * 60UL * 1000UL)   // §13.5: every 30 min
#endif
#ifndef FM_COVER_LATE_MS
#define FM_COVER_LATE_MS (35UL * 60UL * 1000UL)  // one heartbeat overdue
#endif
#ifndef FM_COVER_DOWN_MS
#define FM_COVER_DOWN_MS (65UL * 60UL * 1000UL)  // two missed: presumed down
#endif
#ifndef FM_HELP_HOLD_MS
#define FM_HELP_HOLD_MS 3000       // responder: 4 and 6 held together this long = HELP IS COMING
#endif
#ifndef FM_LIGHT_SLEEP
#define FM_LIGHT_SLEEP 1           // 0 = never sleep (V3 behaviour)
#endif
#ifndef FM_OTA_VERIFY_MS
#define FM_OTA_VERIFY_MS 60000     // a new image must run this long before it is kept
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
enum Screen : uint8_t {
  SCR_HOME,
  SCR_COMPOSE,
  SCR_INBOX,
  SCR_VIEW,
  SCR_SOSLIST,
  SCR_SOSVIEW,
  SCR_STATUS,
  SCR_COVER,
};
static Screen g_screen = SCR_HOME;

static float g_battV = 0.0f;
static uint8_t g_battPct = 0;
static uint32_t g_lastBatt = 0;

// Messages: a ring of the last 16 texts, received and sent, plus our own SOS.
#define MSG_SLOTS 16
struct Msg {
  bool     mine;
  bool     passedOn;     // mine: a neighbour was heard relaying it
  uint8_t  type;         // FM_TYPE_ALARM (our own SOS) / FM_TYPE_TEXT
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

// Powered units heard through their heartbeats (§13.5): the coverage page.
#define COVER_SLOTS 16
struct Cover {
  bool     used;
  char     cs[FM_CALLSIGN_LEN + 1];
  uint8_t  batt;
  uint8_t  hops;
  int16_t  rssi;
  uint16_t count;
  uint32_t at;
};
static Cover g_cover[COVER_SLOTS];
static uint8_t g_coverSel = 0;

// Popup overlay (received message, send result, notices).
static bool g_popup = false;
static bool g_popupSticky = false;   // stays until a key is pressed
static uint32_t g_popupUntil = 0;
static char g_popTitle[24];
static char g_popMeta[40];
static char g_popBody[4 * 21 + 1];   // what the popup shows: 4 lines of 21 characters

// SOS chord and channel screen.
static bool g_chord = false;        // * and # were both held during this hold
static bool g_sosFired = false;     // the hold completed; the channel screen opened
static bool g_sosPick = false;      // channel screen showing
static uint32_t g_sosPickUntil = 0;
static FmSosState g_sosLastState = FM_SOS_IDLE;
static int g_sosLastId = -1;        // msgId of our own SOS's latest frame, for the field log

// Responder "HELP IS COMING" chord (4 + 6) on the SOS detail screen.
static bool g_helpChord = false;    // both keys held during this hold
static bool g_helpFired = false;    // the hold completed and the reply went out

// Responder SOS list.
static int8_t g_sosTab = -1;        // -1 = main list, else the channel tab being viewed
static uint8_t g_sosSel = 0;
static int g_sosViewIdx = -1;       // entry shown on SCR_SOSVIEW
static int8_t g_toggleKey = -1;     // digit being held on the SOS list
static bool g_toggleDone = false;   // its 1 s toggle already fired
static uint32_t g_lastEscCheck = 0;

// The screen each key went down on. A long press or release only counts on
// that same screen: C held from the SOS list into the SOS detail must not
// send "Help coming", and D held from the inbox onto home must not stop an SOS.
static const uint8_t kNoScreen = 0xFF;
static uint8_t g_pressedOn[FM_KP_KEYS];

// Power and heartbeat.
enum PwrMode : uint8_t { PWR_AUTO, PWR_ON, PWR_OFF };
static PwrMode g_pwrMode = PWR_AUTO;
static bool g_powered = false;      // effective: stays awake, forwards everything
static bool g_pwrApplied = false;
static uint32_t g_hbNext = 0;

// Sleep.
static bool g_sleepEnabled = FM_LIGHT_SLEEP != 0;
static uint32_t g_lastSerialMs = 0;

// Range-test ping.
static bool g_ping = false;
static uint32_t g_pingNext = 0;
static uint16_t g_pingSeq = 0;

// A freshly installed image stays on trial until it has run FM_OTA_VERIFY_MS.
static bool g_otaPendingVerify = false;

// ---------------------------------------------------------------- buzzer
// Non-blocking: the mesh keeps running while it beeps.
static uint16_t g_bzPulses = 0, g_bzOn = 0, g_bzOff = 0;
static bool g_bzState = false;
static uint32_t g_bzNext = 0;

// Buzzer drive. An ACTIVE buzzer has its own oscillator and only needs the pin
// held HIGH; a PASSIVE one driven that way just clicks, which is how unit E's
// beeps sounded "feeble". `buzzer passive <Hz>` (kept in NVS) switches the unit
// to a square wave from the LEDC peripheral at that frequency; 0 = active.
#define FM_NVS_KEY_BUZZHZ "buzzhz"
static const uint8_t kBuzzLedc = 4;   // LEDC channel; nothing else in V4 uses LEDC
static uint16_t g_buzzHz = 0;

static void buzzAttach(uint16_t hz) {
  if (hz) {
    ledcSetup(kBuzzLedc, hz, 8);
    ledcAttachPin(PIN_BUZZER, kBuzzLedc);
    ledcWrite(kBuzzLedc, 0);
  } else {
    ledcDetachPin(PIN_BUZZER);
    pinMode(PIN_BUZZER, OUTPUT);
    digitalWrite(PIN_BUZZER, LOW);
  }
}

static void buzzPin(bool on) {
  if (g_buzzHz) ledcWrite(kBuzzLedc, on ? 128 : 0);   // 50% duty square wave
  else          digitalWrite(PIN_BUZZER, on ? HIGH : LOW);
}

static void loadBuzzer() {
  Preferences p;
  if (p.begin(FM_NVS_NAMESPACE, true)) {
    const uint16_t hz = p.getUShort(FM_NVS_KEY_BUZZHZ, 0);
    p.end();
    if (hz == 0 || (hz >= 500 && hz <= 8000)) g_buzzHz = hz;
  }
  buzzAttach(g_buzzHz);
}

static bool setBuzzerHz(uint16_t hz) {
  if (hz != 0 && (hz < 500 || hz > 8000)) return false;
  Preferences p;
  if (!p.begin(FM_NVS_NAMESPACE, false)) return false;
  p.putUShort(FM_NVS_KEY_BUZZHZ, hz);
  p.end();
  g_buzzHz = hz;
  buzzAttach(hz);
  return true;
}

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
  buzzPin(false);
}

static void buzzTick(uint32_t now) {
  if (!g_bzPulses && !g_bzState) return;
  if ((int32_t)(now - g_bzNext) < 0) return;
  if (g_bzState) {
    buzzPin(false);
    g_bzState = false;
    g_bzNext = now + g_bzOff;
  } else if (g_bzPulses) {
    buzzPin(true);
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
    buzzPin(true);
    delay(onMs);
    buzzPin(false);
    if (i + 1 < pulses) delay(gapMs);
  }
}

// ---------------------------------------------------------------- battery and power
static int cmpInt(const void *a, const void *b) {
  return (*(const int *)a) - (*(const int *)b);
}

// Per-unit battery calibration (`batt cal <volts>`), kept in NVS. Heltec V3
// sub-revisions differ in the divider: unit D (a board printed "V3") read
// 2.85 V for a 4.07 V cell, a steady 0.70x. The factor describes the board, so
// a factory reset keeps it.
#define FM_NVS_KEY_BATTCAL "battcal"
static float g_battCal = 1.0f;

/** Battery volts before the per-unit factor (build-time divider and VBAT_CAL only). */
static float readBatteryRaw() {
  digitalWrite(PIN_ADC_CTRL, ADC_CTRL_ENABLE_LEVEL);
  delayMicroseconds(500);
  const uint8_t kSamples = 15;
  int mv[kSamples];
  for (uint8_t i = 0; i < kSamples; i++) mv[i] = analogReadMilliVolts(PIN_VBAT_ADC);
  digitalWrite(PIN_ADC_CTRL, !ADC_CTRL_ENABLE_LEVEL);
  qsort(mv, kSamples, sizeof(int), cmpInt);   // median rejects LoRa/switching spikes
  return (mv[kSamples / 2] * VBAT_DIVIDER * VBAT_CAL) / 1000.0f;
}

static float readBatteryVolts() { return readBatteryRaw() * g_battCal; }

static void loadBatteryCal() {
  Preferences p;
  if (!p.begin(FM_NVS_NAMESPACE, true)) return;
  const float f = p.getFloat(FM_NVS_KEY_BATTCAL, 1.0f);
  p.end();
  if (f >= 0.5f && f <= 2.0f) g_battCal = f;
}

/** Store factor = measured / raw. False when the raw reading or factor is implausible. */
static bool setBatteryCal(float measuredV) {
  const float raw = readBatteryRaw();
  if (raw < 1.0f || measuredV < 2.5f || measuredV > 4.5f) return false;
  const float f = measuredV / raw;
  if (f < 0.5f || f > 2.0f) return false;
  Preferences p;
  if (!p.begin(FM_NVS_NAMESPACE, false)) return false;
  p.putFloat(FM_NVS_KEY_BATTCAL, f);
  p.end();
  g_battCal = f;
  return true;
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

static const char *pwrModeName(PwrMode m) {
  switch (m) {
    case PWR_ON:  return "ON";
    case PWR_OFF: return "OFF";
    default:      return "AUTO";
  }
}

/** Push the effective power state to the mesh and radio; start heartbeats. */
static void applyPower(uint32_t now) {
  const bool p = g_pwrMode == PWR_ON ? true : g_pwrMode == PWR_OFF ? false : fmPowerExternal();
  fmMeshSetBattery(g_battPct);
  if (g_pwrApplied && p == g_powered) return;
  g_pwrApplied = true;
  g_powered = p;
  fmMeshSetPowered(p);
  fmRadioSetDutyCycle(!p);
  if (p) g_hbNext = now + 5000UL + (esp_random() % 55000UL);   // first heartbeat within a minute
  Serial.printf("[PWR] %s (mode %s, %s, batt %.2f V, VBUS %lu mV): %s\n",
                p ? "EXTERNAL POWER" : "BATTERY", pwrModeName(g_pwrMode), fmPowerMethod(),
                g_battV, (unsigned long)fmPowerVbusMv(),
                p ? "stays awake, forwards everything, sends heartbeats"
                  : "sleeps when idle, forwards SOS always and texts only with no powered unit nearby");
}

static void updateBattery(uint32_t now, bool force) {
  if (!force && (now - g_lastBatt) < kBattPeriodMs) return;
  g_lastBatt = now;
  g_battV = readBatteryVolts();
  g_battPct = batteryPercent(g_battV);
  fmPowerUpdate((uint32_t)(g_battV * 1000.0f), now);   // divider if fitted, else the trend
  applyPower(now);
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
static bool isResponder() { return fmRole() == FM_ROLE_RESPONDER; }

static void fmtAge(uint32_t ms, char *out, size_t n) {
  const uint32_t s = ms / 1000;
  if (s < 60)        snprintf(out, n, "%lus", (unsigned long)s);
  else if (s < 3600) snprintf(out, n, "%lum", (unsigned long)(s / 60));
  else               snprintf(out, n, "%luh", (unsigned long)(s / 3600));
}

/** Digit value of a key, or -1 for A-D, * and #. */
static int keyDigit(uint8_t k) {
  const char c = fmKeypadChar(k);
  return (c >= '0' && c <= '9') ? c - '0' : -1;
}

static bool isDigitKey(uint8_t k) { return keyDigit(k) >= 0; }

/** Call sign with the on-air pad spaces trimmed, for display. */
static void trimCs(const char *cs, char *out, size_t n) {
  snprintf(out, n, "%s", cs);
  for (int i = (int)strlen(out) - 1; i >= 0 && out[i] == ' '; i--) out[i] = '\0';
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

static void coverUpdate(const FmHeartbeatRx *rx, uint32_t now) {
  int slot = -1, oldest = 0;
  for (uint8_t i = 0; i < COVER_SLOTS; i++) {
    if (g_cover[i].used && strncmp(g_cover[i].cs, rx->header.callSign, FM_CALLSIGN_LEN) == 0) {
      slot = i;
      break;
    }
    if (slot < 0 && !g_cover[i].used) { slot = i; break; }
    if (g_cover[i].at < g_cover[oldest].at) oldest = i;
  }
  Cover *c = &g_cover[slot >= 0 ? slot : oldest];
  if (!c->used || strncmp(c->cs, rx->header.callSign, FM_CALLSIGN_LEN) != 0) {
    memset(c, 0, sizeof(*c));
    c->used = true;
    snprintf(c->cs, sizeof(c->cs), "%s", rx->header.callSign);
  }
  c->batt = rx->battPct;
  c->hops = rx->hops;
  c->rssi = (int16_t)rx->rssi;
  c->count++;
  c->at = now;
}

/** Coverage entries sorted oldest heartbeat first, so the ones going quiet lead. */
static uint8_t coverSorted(uint8_t idx[COVER_SLOTS]) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < COVER_SLOTS; i++) {
    if (g_cover[i].used) idx[n++] = i;
  }
  for (uint8_t i = 1; i < n; i++) {
    for (uint8_t j = i; j > 0 && g_cover[idx[j]].at < g_cover[idx[j - 1]].at; j--) {
      const uint8_t t = idx[j];
      idx[j] = idx[j - 1];
      idx[j - 1] = t;
    }
  }
  return n;
}

static const char *coverState(uint32_t ageMs) {
  if (ageMs >= FM_COVER_DOWN_MS) return "DOWN?";
  if (ageMs >= FM_COVER_LATE_MS) return "late";
  return "ok";
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

/** One-line preview: "Me: HELLO", "KXQ2R: HELP"; with an age, "KXQ2R 5m: ...". */
static void msgPreview(const Msg *m, const char *age, char *out, size_t n) {
  const char *who = m->mine ? "Me" : m->from;
  const char *bang = m->type == FM_TYPE_ALARM ? "!" : "";
  if (age) snprintf(out, n, "%s %s: %s%s", who, age, bang, m->text);
  else     snprintf(out, n, "%s: %s%s", who, bang, m->text);
  if (n > 25) out[25] = '\0';   // 25 columns of the 5x8 font
}

/** Show the SOS list behind a popup, unless someone is typing. */
static void showSosList() {
  if (g_screen == SCR_COMPOSE) return;
  g_screen = SCR_SOSLIST;
  g_sosTab = -1;
  g_sosSel = 0;
}

// ---------------------------------------------------------------- mesh callbacks
/**
 * One CSV line per received frame (ms,kind,from,msgId,hops,rssi,snr,text), and
 * the same reading as an `rx` record for the field logger. t is the logger's
 * finer type (SOS vs ALARM, PING vs TEXT); ch is the SOS channel or -1.
 */
static void logRx(const char *kind, FmLogType t, int ch, const FmHeader &h, uint8_t hops,
                  float rssi, float snr, const char *text) {
  Serial.printf("RXLOG,%lu,%s,%s,%u,%u,%.0f,%.1f,%s\n", (unsigned long)millis(), kind,
                h.callSign, (unsigned)h.msgId, (unsigned)hops, rssi, snr, text ? text : "");
  fmLogRx(t, h.callSign, h.msgId, hops, rssi, snr, text, ch);
}

/** A text for the inbox, with a popup unless someone is typing. */
static void inboxText(const FmHeader &h, uint8_t hops, float rssi, float snr, const char *text,
                      uint32_t now) {
  Msg *m = msgPush();
  m->type = FM_TYPE_TEXT;
  m->msgId = h.msgId;
  m->hops = hops;
  m->rssi = (int16_t)rssi;
  m->snr = snr;
  m->at = now;
  snprintf(m->from, sizeof(m->from), "%s", h.callSign);
  snprintf(m->text, sizeof(m->text), "%s", text);
  g_unread++;

  char meta[40];
  snprintf(meta, sizeof(meta), "%s  %u hop  %d dBm", h.callSign, (unsigned)hops, (int)rssi);
  // Don't yank the screen away from someone typing: beep and count it instead.
  if (g_screen != SCR_COMPOSE) popup("MESSAGE", meta, text, false);
  buzzStart(2, 60, 80);
}

static void onAlarmRx(const FmAlarmRx *rx) {
  const uint32_t now = millis();
  uint8_t ch = FM_SOS_GENERAL;
  const bool sos = fmAlarmSosChannel(rx->alarmType, &ch);
  logRx("ALARM", sos ? FM_LOG_T_SOS : FM_LOG_T_ALARM, sos ? (int)ch : -1, rx->header, rx->hops,
        rx->rssi, rx->snr, fmAlarmName(rx->alarmType));
  heardUpdate(rx->header.callSign, rx->hops, rx->rssi, rx->snr, now);

  // A V3 unit's SAFE preset: a status text for everyone (there are no presets in V4).
  if (rx->alarmType == FM_ALARM_SAFE) {
    inboxText(rx->header, rx->hops, rx->rssi, rx->snr, "SAFE", now);
    return;
  }
  if (!sos) return;

  // §13.4: civilian units forward an SOS silently. The mesh has already queued
  // the relay; nothing is shown and the buzzer stays quiet.
  if (!isResponder()) {
    Serial.printf("[SOS] %s from %s - forwarded silently (civilian unit)\n", fmSosChName(ch),
                  rx->header.callSign);
    return;
  }

  bool isNew = false;
  const int idx = fmSosRxAdd(rx->header.callSign, ch, rx->header.msgId, rx->hops, rx->rssi,
                             rx->snr, now, &isNew);
  // §13.8 safeguard 5: acknowledge every SOS received, shown or not, so the
  // sender stops retrying; the race means only one responder per area answers.
  fmMeshQueueAck(FM_ACK_DELIVERED, rx->header.callSign, rx->header.msgId, rx->snr);
  if (idx < 0 || !isNew) return;

  if (fmSosSelected(ch)) {
    char title[24], meta[40];
    snprintf(title, sizeof(title), "!! SOS %s !!", fmSosChShort(ch));
    snprintf(meta, sizeof(meta), "%s  %u hop  %d dBm", rx->header.callSign, (unsigned)rx->hops,
             (int)rx->rssi);
    popup(title, meta, fmSosChName(ch), true);
    buzzStart(60, 700, 300);   // about a minute, or until a key is pressed
    fmSosRxAlarmed(idx, now);  // reminders count from here
    showSosList();
  } else {
    Serial.printf("[SOS] %s from %s - channel not selected: tab marked, no alarm\n",
                  fmSosChName(ch), rx->header.callSign);
  }
}

static void onTextRx(const FmTextRx *rx) {
  const uint32_t now = millis();
  logRx("TEXT", strncmp(rx->text, "PING ", 5) == 0 ? FM_LOG_T_PING : FM_LOG_T_TEXT, -1,
        rx->header, rx->hops, rx->rssi, rx->snr, rx->text);
  heardUpdate(rx->header.callSign, rx->hops, rx->rssi, rx->snr, now);

  // Range-test pings update the heard table and the log, not the inbox.
  if (strncmp(rx->text, "PING ", 5) == 0) {
    if (!g_bzPulses) buzzStart(1, 20, 0);
    return;
  }
  inboxText(rx->header, rx->hops, rx->rssi, rx->snr, rx->text, now);
}

static void onAckRx(const FmAckRx *rx) {
  const uint32_t now = millis();
  char t[24];
  snprintf(t, sizeof(t), "%s %s", rx->kind == FM_ACK_HELP ? "HELP" : "DELIVERED", rx->target);
  logRx("ACK", FM_LOG_T_ACK, -1, rx->header, rx->hops, rx->rssi, rx->snr, t);
  heardUpdate(rx->header.callSign, rx->hops, rx->rssi, rx->snr, now);

  if (memcmp(rx->target, fmCallSign(), FM_CALLSIGN_LEN) == 0) {
    if (fmSosOnAck(rx->kind, rx->targetMsgId, rx->header.callSign)) {
      fmLogSos(rx->kind == FM_ACK_HELP ? FM_LOG_SOS_HELP : FM_LOG_SOS_DELIVERED, rx->targetMsgId,
               fmSosChannel(), fmSosTries() > 0 ? (uint8_t)(fmSosTries() - 1) : 0);
      if (rx->kind == FM_ACK_HELP) {
        popup("HELP IS COMING", rx->header.callSign,
              "A responder is on the way. Stay where you are.", true);
        buzzStart(5, 200, 150);
      } else {
        popup("SOS DELIVERED", rx->header.callSign,
              "A responder unit has it. Re-sent every 15 min until help is coming.", false, 10000);
        buzzStart(3, 80, 80);
      }
    }
    return;
  }
  // Another responder's "Help coming" stops our escalation for that SOS.
  if (isResponder() && rx->kind == FM_ACK_HELP) {
    fmSosRxHelp(rx->target, rx->header.callSign, false);
  }
}

static void onHeartbeatRx(const FmHeartbeatRx *rx) {
  const uint32_t now = millis();
  char t[12];
  snprintf(t, sizeof(t), "B%u", (unsigned)rx->battPct);
  logRx("HEARTBEAT", FM_LOG_T_HEARTBEAT, -1, rx->header, rx->hops, rx->rssi, rx->snr, t);
  heardUpdate(rx->header.callSign, rx->hops, rx->rssi, rx->snr, now);
  coverUpdate(rx, now);
}

static void onEcho(const FmEchoRx *e) {
  fmLogEcho(e->type, e->msgId, e->rssi, e->snr);   // every type: heartbeats and ACKs too
  if (e->type != FM_TYPE_ALARM && e->type != FM_TYPE_TEXT) return;
  Serial.printf("[ECHO] our %s msg %u relayed by a neighbour (%.0f dBm, SNR %.1f)\n",
                e->type == FM_TYPE_ALARM ? "SOS" : "text", (unsigned)e->msgId, e->rssi, e->snr);
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
/** Field logger: our own SOS changed state. n = retries so far (the first send is not one). */
static void logSos(FmLogSosState st, int id) {
  const uint8_t tries = fmSosTries();
  fmLogSos(st, id, fmSosChannel(), tries > 0 ? (uint8_t)(tries - 1) : 0);
}

/** Stop our own SOS (hold D, or `sos stop`). Logged only if one was running. */
static void stopOwnSos() {
  const FmSosState before = fmSosState();
  fmSosStop();
  if (before != FM_SOS_IDLE && before != FM_SOS_STOPPED) logSos(FM_LOG_SOS_STOPPED, g_sosLastId);
}

/** Start an SOS episode; the first frame goes out on the next pass of sosSenderTick(). */
static void startSos(uint8_t ch) {
  g_sosPick = false;
  g_sosLastId = -1;
  fmSosStart(ch, millis());
  Serial.printf("[SOS] %s - sending, then retrying until a responder answers\n", fmSosChName(ch));
}

static void sosSenderTick(uint32_t now) {
  if (fmSosDue(now)) {
    const uint8_t ch = fmSosChannel();
    const uint8_t a = fmSosAlarmType(ch);
    const bool keepAlive = fmSosState() == FM_SOS_DELIVERED;
    const int id = fmMeshSendAlarm(a);
    const bool first = !keepAlive && fmSosTries() == 0;
    fmSosSent(id, now);
    if (id >= 0) {
      g_sosLastId = id;
      // A keep-alive after DELIVERED is not a state change: its tx record says it.
      if (!keepAlive) logSos(first ? FM_LOG_SOS_SENT : FM_LOG_SOS_RETRY, id);
    }
    Serial.printf("[SOS] %s %s %u -> %s, next in %lu s\n", fmSosChName(ch),
                  keepAlive ? "keep-alive after try" : "try", (unsigned)fmSosTries(),
                  id >= 0 ? "sent" : "FAILED", (unsigned long)(fmSosNextInMs(now) / 1000));
    if (id >= 0 && first) {
      Msg *m = msgPush();
      m->mine = true;
      m->type = FM_TYPE_ALARM;
      m->alarm = a;
      m->msgId = (uint8_t)id;
      m->at = now;
      snprintf(m->from, sizeof(m->from), "%s", fmCallSign());
      snprintf(m->text, sizeof(m->text), "%s", fmAlarmName(a));
      popup("SOS SENT", fmSosChName(ch), "Retrying until a responder unit answers.", false, 5000);
      buzzStart(2, 80, 60);
    } else if (id < 0 && first) {
      popup("SOS NOT SENT YET", "radio busy or down", "Trying again in 30 s.", false, 5000);
      buzzStart(1, 600, 0);
    }
  }

  // Announce the end of retries once.
  const FmSosState st = fmSosState();
  if (st != g_sosLastState) {
    if (st == FM_SOS_NOT_DELIVERED) {
      logSos(FM_LOG_SOS_NOT_DELIVERED, g_sosLastId);
      // §7.6: two beeps and a message after the 5th retry goes unanswered.
      popup("SOS NOT DELIVERED", fmSosChName(fmSosChannel()),
            "No responder answered 5 retries. Move higher or outside and send again.", true);
      buzzStart(2, 300, 200);
    }
    g_sosLastState = st;
  }
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

/** Responder: HELP IS COMING for the SOS shown on SCR_SOSVIEW. Returns true if sent. */
static bool sendHelpComing(int entryIdx) {
  FmSosEntry *e = fmSosEntryAt(entryIdx);
  if (!e) return false;
  const int id = fmMeshSendAck(FM_ACK_HELP, e->cs, e->msgId);
  Serial.printf("[SOS] HELP IS COMING -> %s -> %s\n", e->cs, id >= 0 ? "sent" : "FAILED");
  if (id < 0) {
    popup("NOT SENT", "radio busy", "Hold 4 and 6 to try again.", false, 4000);
    buzzStart(1, 600, 0);
    return false;
  }
  fmSosRxHelp(e->cs, fmCallSign(), true);
  popup("REPLY SENT", e->cs, "The sender's unit shows HELP IS COMING.", false, 4000);
  buzzStart(2, 80, 60);
  return true;
}

static void heartbeatTick(uint32_t now) {
  if (!g_powered || (int32_t)(now - g_hbNext) < 0) return;
  const int id = fmMeshSendHeartbeat(g_battPct);
  Serial.printf("[HB] heartbeat -> %s\n", id >= 0 ? "sent" : "skipped");
  // Jitter so powered units switched on together do not stay in step.
  g_hbNext = now + (id >= 0 ? FM_HEARTBEAT_MS + (esp_random() % 60000UL) : 60000UL);
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

// The mode is kept in NVS: a roof unit set to ON must still be ON after it is
// switched off and on again, and without the GPIO 3 divider AUTO can guess
// wrong (field test 28 Sep: a handheld flipped to "powered" 13 times).
#define FM_NVS_KEY_PWRMODE "pwrmode"

static void setPowerMode(PwrMode m) {
  g_pwrMode = m;
  Preferences p;
  if (p.begin(FM_NVS_NAMESPACE, false)) {
    p.putUChar(FM_NVS_KEY_PWRMODE, (uint8_t)m);
    p.end();
  }
  applyPower(millis());
}

static void loadPowerMode() {
  Preferences p;
  if (!p.begin(FM_NVS_NAMESPACE, true)) return;
  const uint8_t m = p.getUChar(FM_NVS_KEY_PWRMODE, PWR_AUTO);
  p.end();
  if (m <= PWR_OFF) g_pwrMode = (PwrMode)m;
}

// ---------------------------------------------------------------- drawing
static void drawHeader(const char *title) {
  oled.setDrawColor(1);
  oled.drawBox(0, 0, 128, 11);
  oled.setDrawColor(0);
  oled.setFont(u8g2_font_5x8_tf);
  oled.drawStr(2, 8, title);
  char r[16];
  snprintf(r, sizeof(r), "%s%u%%", g_powered ? "P " : "", (unsigned)g_battPct);
  oled.drawStr(126 - oled.getStrWidth(r), 8, r);
  oled.setDrawColor(1);
}

static void drawFooter(const char *s) {
  oled.setFont(u8g2_font_4x6_tf);
  oled.drawStr(0, 63, s);
}

/** One line about our own SOS, or "" if there is none to report. */
static void sosSenderLine(uint32_t now, char *t, size_t n) {
  const char *ch = fmSosChShort(fmSosChannel());
  switch (fmSosState()) {
    case FM_SOS_WAITING: {
      char age[8];
      fmtAge(fmSosNextInMs(now), age, sizeof(age));
      snprintf(t, n, "SOS %s: waiting, retry %s", ch, age);
      break;
    }
    case FM_SOS_DELIVERED: snprintf(t, n, "SOS %s: DELIVERED", ch); break;
    case FM_SOS_HELP:      snprintf(t, n, "SOS %s: HELP IS COMING", ch); break;
    case FM_SOS_NOT_DELIVERED: snprintf(t, n, "SOS %s: NOT DELIVERED", ch); break;
    default:               t[0] = '\0'; break;
  }
}

static void drawHome(uint32_t now) {
  char t[40];
  if (isResponder()) {
#if FM_BENCH_RESPONDER
    snprintf(t, sizeof(t), "%s RSP TEST", fmCallSign());   // never mistaken for a real one
#else
    snprintf(t, sizeof(t), "%s RSP %lud", fmCallSign(),
             (unsigned long)((fmRoleRemainingS() + 86399UL) / 86400UL));
#endif
  } else {
    snprintf(t, sizeof(t), "%s USER", fmCallSign());
  }
  drawHeader(t);
  oled.setFont(u8g2_font_5x8_tf);
  snprintf(t, sizeof(t), "%.2fV  air %u.%u%%%s", g_battV, fmAirtimePermille() / 10,
           fmAirtimePermille() % 10, g_ping ? "  PING" : "");
  oled.drawStr(0, 20, t);
  sosSenderLine(now, t, sizeof(t));
  if (!t[0] && isResponder()) {
    const uint8_t unseen = fmSosUnseenVisible();
    if (unseen) snprintf(t, sizeof(t), "SOS: %u  (%u NEW)", (unsigned)fmSosVisibleCount(),
                         (unsigned)unseen);
    else        snprintf(t, sizeof(t), "SOS: %u", (unsigned)fmSosVisibleCount());
  }
  if (t[0]) oled.drawStr(0, 29, t);
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
  if (fmSosState() == FM_SOS_WAITING || fmSosState() == FM_SOS_DELIVERED) {
    drawFooter("hold D:STOP SOS  B:STAT C:INBOX");
  } else if (isResponder()) {
    drawFooter("A:SOS B:STAT C:INBOX *+#:SOS");
  } else {
    drawFooter("B:STATUS  C:INBOX  *+#:SOS");
  }
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
           m->type == FM_TYPE_ALARM ? "SOS" : "", age);
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

/**
 * The tab bar of the responder SOS screen (§13.8): selected channels in
 * capitals, the others in lower case; an unselected channel holding an SOS
 * this unit has not shown yet is drawn inverted with a dot; the tab being
 * viewed is underlined.
 */
static void drawSosTabs() {
  oled.setFont(u8g2_font_5x8_tf);
  uint8_t x = 0;
  for (uint8_t ch = 0; ch < FM_SOS_CH_COUNT; ch++) {
    const bool sel = fmSosSelected(ch);
    const char *label = sel ? fmSosChShort(ch) : fmSosChShortLower(ch);
    const bool dot = fmSosTabDot(ch);
    const uint8_t tw = (uint8_t)oled.getStrWidth(label);
    const uint8_t w = (uint8_t)(tw + (dot ? 4 : 0));
    if (dot) {
      oled.drawBox(x, 0, (uint8_t)(w + 2), 10);
      oled.setDrawColor(0);
      oled.drawStr((uint8_t)(x + 1), 8, label);
      oled.drawDisc((uint8_t)(x + 1 + tw + 2), 4, 1);
      oled.setDrawColor(1);
    } else {
      oled.drawStr((uint8_t)(x + 1), 8, label);
    }
    if (g_sosTab == (int8_t)ch) oled.drawHLine(x, 10, (uint8_t)(w + 2));
    x = (uint8_t)(x + w + 2 + 3);
  }
  oled.drawHLine(0, 12, 128);
}

static void drawSosList(uint32_t now) {
  drawSosTabs();
  uint8_t idx[FM_SOS_TABLE];
  const uint8_t n = fmSosList(g_sosTab, idx, FM_SOS_TABLE);
  oled.setFont(u8g2_font_5x8_tf);
  if (n == 0) {
    oled.drawStr(0, 24, g_sosTab < 0 ? "no SOS in your channels" : "no SOS in this channel");
    oled.drawStr(0, 36, "hold 1-4: add/remove");
    oled.drawStr(0, 45, "a channel (GEN always on)");
  } else {
    if (g_sosSel >= n) g_sosSel = (uint8_t)(n - 1);
    const uint8_t rows = 4;
    const uint8_t top = g_sosSel >= rows ? (uint8_t)(g_sosSel - rows + 1) : 0;
    for (uint8_t r = 0; r < rows && top + r < n; r++) {
      FmSosEntry *e = fmSosEntryAt(idx[top + r]);
      if (!e) continue;
      e->seen = true;   // shown on this unit's screen: clears the tab dot
      char age[8], cs[FM_CALLSIGN_LEN + 1], t[32];
      fmtAge(now - e->lastAt, age, sizeof(age));
      trimCs(e->cs, cs, sizeof(cs));
      snprintf(t, sizeof(t), "%-5s %-4s %4s %s%s", cs, fmSosChShort(e->ch), age,
               e->helpBy[0] ? "H" : "", e->escalated ? "!" : "");
      const uint8_t y = (uint8_t)(22 + r * 9);
      if (top + r == g_sosSel) {
        oled.drawBox(0, y - 8, 128, 9);
        oled.setDrawColor(0);
        oled.drawStr(1, y, t);
        oled.setDrawColor(1);
      } else {
        oled.drawStr(1, y, t);
      }
    }
  }
  drawFooter("0-4:TAB A/B:MOVE C:OPEN D:BACK");
}

static void drawSosView(uint32_t now) {
  FmSosEntry *e = fmSosEntryAt(g_sosViewIdx);
  if (!e) { g_screen = SCR_SOSLIST; return; }
  e->seen = true;
  char t[40], age[8], age2[8], cs[FM_CALLSIGN_LEN + 1];
  trimCs(e->cs, cs, sizeof(cs));
  snprintf(t, sizeof(t), "SOS %s%s", cs, e->escalated ? " (ESCALATED)" : "");
  drawHeader(t);
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 22, fmSosChName(e->ch));
  oled.setFont(u8g2_font_5x8_tf);
  fmtAge(now - e->firstAt, age, sizeof(age));
  fmtAge(now - e->lastAt, age2, sizeof(age2));
  snprintf(t, sizeof(t), "first %s ago, last %s, x%u", age, age2, (unsigned)e->count);
  oled.drawStr(0, 32, t);
  snprintf(t, sizeof(t), "%u hop  %d dBm  SNR %.1f", (unsigned)e->hops, e->rssi, e->snr);
  oled.drawStr(0, 41, t);
  if (e->helpMine) {
    snprintf(t, sizeof(t), "HELP IS COMING: you");
  } else if (e->helpBy[0]) {
    snprintf(t, sizeof(t), "HELP IS COMING: %s", e->helpBy);
  } else {
    snprintf(t, sizeof(t), "nobody has answered yet");
  }
  oled.drawStr(0, 51, t);
  drawFooter("hold 4+6: HELP IS COMING  D:BACK");
}

static void drawSosPick(uint32_t now) {
  drawHeader("SOS: PICK HELP TYPE");
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 23, "1 MED  2 EVAC 3 HAZ");
  oled.drawStr(0, 35, "4 FOOD 0 GENERAL");
  const uint32_t left = (int32_t)(g_sosPickUntil - now) > 0 ? g_sosPickUntil - now : 0;
  char t[32];
  snprintf(t, sizeof(t), "SENDS GENERAL IN %lus", (unsigned long)((left + 999) / 1000));
  oled.setFont(u8g2_font_5x8_tf);
  oled.drawStr(0, 49, t);
  drawFooter("D: CANCEL");
}

static void drawStatus(uint32_t now) {
  char t[48];
  const uint32_t up = now ? now : 1;
  snprintf(t, sizeof(t), "STATUS %s slp %lu%%", g_ping ? "PING" : "",
           (unsigned long)((uint64_t)fmSleepTotalMs() * 100u / up));
  drawHeader(t);
  oled.setFont(u8g2_font_5x8_tf);
  snprintf(t, sizeof(t), "air %u.%u%% rly %lu sup %lu q%u", fmAirtimePermille() / 10,
           fmAirtimePermille() % 10, (unsigned long)fmMeshFramesRelayed(),
           (unsigned long)fmMeshFramesSuppressed(), (unsigned)fmMeshRelayQueueDepth());
  oled.drawStr(0, 19, t);

  oled.setFont(u8g2_font_4x6_tf);
  char near[10];
  const uint32_t nearAge = fmMeshPoweredNearbyAgeMs(now);
  if (nearAge == UINT32_MAX) snprintf(near, sizeof(near), "none");
  else fmtAge(nearAge, near, sizeof(near));
  snprintf(t, sizeof(t), "PWR %s:%s (%s) near %s", pwrModeName(g_pwrMode),
           g_powered ? "EXT" : "BATT", fmPowerMethod(), near);
  oled.drawStr(0, 26, t);
  // Field logger: records waiting and how long ago the last upload got through.
  fmLogStatusLine(t, sizeof(t), now);
  oled.drawStr(0, 33, t);
  oled.drawStr(0, 40, "HEARD  hop  dBm   SNR  age   n");

  oled.setFont(u8g2_font_5x8_tf);
  uint8_t idx[HEARD_SLOTS];
  const uint8_t n = heardSorted(idx);
  if (n == 0) oled.drawStr(0, 48, "nobody yet");
  if (g_heardSel >= n && n) g_heardSel = n - 1;
  const uint8_t top = g_heardSel >= 2 ? (uint8_t)(g_heardSel - 1) : 0;
  for (uint8_t r = 0; r < 2 && top + r < n; r++) {
    const Heard &h = g_heard[idx[top + r]];
    char age[8];
    fmtAge(now - h.at, age, sizeof(age));
    snprintf(t, sizeof(t), "%-5s %u %4d %5.1f %4s %u", h.cs, (unsigned)h.hops, h.rssi, h.snr,
             age, (unsigned)h.count);
    oled.drawStr(0, (uint8_t)(48 + r * 8), t);
  }
  drawFooter(g_ping ? "C:PING OFF #:POWER *:COVER D:BACK" : "C:PING ON #:POWER *:COVER D:BACK");
}

static void drawCover(uint32_t now) {
  char t[40];
  uint8_t idx[COVER_SLOTS];
  const uint8_t n = coverSorted(idx);
  snprintf(t, sizeof(t), "POWERED UNITS %u", (unsigned)n);
  drawHeader(t);
  oled.setFont(u8g2_font_4x6_tf);
  oled.drawStr(0, 18, "UNIT   batt hop  last   state");
  oled.setFont(u8g2_font_5x8_tf);
  if (n == 0) oled.drawStr(0, 30, "no heartbeat heard yet");
  if (g_coverSel >= n && n) g_coverSel = n - 1;
  const uint8_t rows = 4;
  const uint8_t top = g_coverSel >= rows ? (uint8_t)(g_coverSel - rows + 1) : 0;
  for (uint8_t r = 0; r < rows && top + r < n; r++) {
    const Cover &c = g_cover[idx[top + r]];
    char age[8];
    fmtAge(now - c.at, age, sizeof(age));
    snprintf(t, sizeof(t), "%-5s %3u%% %u %5s %s", c.cs, (unsigned)(c.batt > 100 ? 0 : c.batt),
             (unsigned)c.hops, age, coverState(now - c.at));
    oled.drawStr(0, (uint8_t)(28 + r * 9), t);
  }
  drawFooter("A/B:SCROLL  D:BACK");
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
  oled.drawStr(0, 30, "KEEP HOLDING");
  const uint32_t w = heldMs >= FM_SOS_HOLD_MS ? 124 : heldMs * 124 / FM_SOS_HOLD_MS;
  oled.drawFrame(0, 38, 128, 12);
  if (w) oled.drawBox(2, 40, (uint8_t)w, 8);
  oled.setFont(u8g2_font_5x8_tf);
  oled.drawStr(0, 60, "release to cancel");
}

/** Responder: 4 and 6 held on the SOS detail screen (same pattern as the SOS hold). */
static void drawHelpHold(uint32_t heldMs) {
  drawHeader("REPLY TO SOS");
  oled.setFont(u8g2_font_7x13B_tf);
  if (g_helpFired) {
    oled.drawStr(0, 30, "HELP IS COMING");
    oled.setFont(u8g2_font_5x8_tf);
    oled.drawStr(0, 44, "sent");
    oled.drawStr(0, 60, "release the keys");
    return;
  }
  oled.drawStr(0, 26, "HELP IS COMING");
  const uint32_t w = heldMs >= FM_HELP_HOLD_MS ? 124 : heldMs * 124 / FM_HELP_HOLD_MS;
  oled.drawFrame(0, 36, 128, 12);
  if (w) oled.drawBox(2, 38, (uint8_t)w, 8);
  oled.setFont(u8g2_font_5x8_tf);
  oled.drawStr(0, 60, "keep holding - release to cancel");
}

static bool helpChordHeld() {
  return g_screen == SCR_SOSVIEW && fmKeypadHeld(FM_KP_4) && fmKeypadHeld(FM_KP_6);
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
  const uint32_t helpHeld = helpChordHeld() ? min(fmKeypadHeldMs(FM_KP_4, now),
                                                  fmKeypadHeldMs(FM_KP_6, now))
                                              : 0;
  if (g_sosPick) {
    drawSosPick(now);
  } else if (chordHeld && held >= 250 && !g_sosFired) {
    drawSosHold(held);
  } else if (g_helpChord && !g_popup && (helpHeld >= 250 || g_helpFired)) {
    drawHelpHold(helpHeld);
  } else if (g_popup) {
    drawPopup();
  } else {
    switch (g_screen) {
      case SCR_HOME:    drawHome(now); break;
      case SCR_COMPOSE: drawCompose(now); break;
      case SCR_INBOX:   drawInbox(now); break;
      case SCR_VIEW:    drawView(now); break;
      case SCR_SOSLIST: drawSosList(now); break;
      case SCR_SOSVIEW: drawSosView(now); break;
      case SCR_STATUS:  drawStatus(now); break;
      case SCR_COVER:   drawCover(now); break;
    }
  }
  oled.sendBuffer();
}

// ---------------------------------------------------------------- keys
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

/** The channel screen owns the keypad until a channel is picked or D cancels. */
static void handleSosPick(const FmKeyEvent &ev) {
  if (ev.type != FM_KEV_PRESS) return;
  const int d = keyDigit(ev.key);
  if (d >= 0 && d < FM_SOS_CH_COUNT) {
    startSos((uint8_t)d);
  } else if (ev.key == FM_KP_D) {
    g_sosPick = false;
    Serial.println("[SOS] cancelled on the channel screen");
    popup("SOS CANCELLED", "", "Nothing was sent.", false, 3000);
  }
}

static void handleSosList(const FmKeyEvent &ev) {
  if (ev.type != FM_KEV_PRESS) return;
  const int d = keyDigit(ev.key);
  if (d >= 0) {
    if (d < FM_SOS_CH_COUNT) {
      g_sosTab = (int8_t)d;
      g_sosSel = 0;
      g_toggleKey = (int8_t)ev.key;   // a 1 s hold of the same key toggles the channel
      g_toggleDone = false;
    }
    return;
  }
  uint8_t idx[FM_SOS_TABLE];
  const uint8_t n = fmSosList(g_sosTab, idx, FM_SOS_TABLE);
  if (ev.key == FM_KP_A && g_sosSel > 0) g_sosSel--;
  else if (ev.key == FM_KP_B && g_sosSel + 1 < n) g_sosSel++;
  else if (ev.key == FM_KP_C && n) {
    g_sosViewIdx = idx[g_sosSel < n ? g_sosSel : 0];
    g_screen = SCR_SOSVIEW;
  } else if (ev.key == FM_KP_D) {
    if (g_sosTab >= 0) {
      g_sosTab = -1;
      g_sosSel = 0;
    } else {
      g_screen = SCR_HOME;
    }
  }
}

/** Holding a digit for FM_SOS_TOGGLE_MS on the SOS list selects or unselects that channel. */
static void sosToggleTick(uint32_t now) {
  if (g_screen != SCR_SOSLIST || g_toggleKey < 0) return;
  if (!fmKeypadHeld((uint8_t)g_toggleKey)) {
    g_toggleKey = -1;
    return;
  }
  if (g_toggleDone || fmKeypadHeldMs((uint8_t)g_toggleKey, now) < FM_SOS_TOGGLE_MS) return;
  g_toggleDone = true;
  const int d = keyDigit((uint8_t)g_toggleKey);
  if (d < 0 || d >= FM_SOS_CH_COUNT) return;
  char body[48];
  if (!fmSosToggle((uint8_t)d)) {
    popup("GENERAL", "always selected", "General SOS always shows on every responder unit.",
          false, 3000);
  } else {
    snprintf(body, sizeof(body), "%s SOS %s", fmSosChName((uint8_t)d),
             fmSosSelected((uint8_t)d) ? "will alert you" : "is hidden (tab dot only)");
    popup(fmSosSelected((uint8_t)d) ? "CHANNEL SELECTED" : "CHANNEL REMOVED", "", body, false,
          3000);
    Serial.printf("[SOS] channel %s %s (mask 0x%02X)\n", fmSosChName((uint8_t)d),
                  fmSosSelected((uint8_t)d) ? "selected" : "removed",
                  (unsigned)fmSosSelectionMask());
  }
  buzzStart(1, 60, 0);
}

static void handleKey(const FmKeyEvent &ev, uint32_t now) {
  if (ev.type == FM_KEV_NONE) return;
  const bool press = ev.type == FM_KEV_PRESS;

  if (ev.key >= FM_KP_KEYS) return;
  if (press) {
    keyClick();
    g_pressedOn[ev.key] = kNoScreen;
    // A press that wakes the screen or dismisses a popup does nothing else.
    if (g_screenOff) { wake(now); return; }
    g_lastActivity = now;
    if (g_popup && !g_sosPick) {
      g_popup = false;
      buzzStop();
      return;
    }
  }

  if (g_sosPick) {
    handleSosPick(ev);
    return;
  }

  if (press) g_pressedOn[ev.key] = (uint8_t)g_screen;
  else if (g_pressedOn[ev.key] != (uint8_t)g_screen) return;   // went down elsewhere

  switch (g_screen) {
    case SCR_HOME:
      if (press && isDigitKey(ev.key)) {
        fmEditClear();
        fmEditKey(ev.key, now);
        g_screen = SCR_COMPOSE;
      } else if (press && ev.key == FM_KP_A && isResponder()) {
        showSosList();
      } else if (press && ev.key == FM_KP_B) {
        g_heardSel = 0;
        g_screen = SCR_STATUS;
      } else if (press && ev.key == FM_KP_C) {
        g_inboxSel = 0;
        g_unread = 0;
        g_screen = SCR_INBOX;
      } else if (ev.type == FM_KEV_LONG && ev.key == FM_KP_D &&
                 (fmSosState() == FM_SOS_WAITING || fmSosState() == FM_SOS_DELIVERED)) {
        stopOwnSos();
        Serial.println("[SOS] retries stopped by the user");
        popup("SOS STOPPED", "", "Not sent again. Hold * and # to send a new SOS.", false, 4000);
        buzzStart(1, 300, 0);
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

    case SCR_SOSLIST:
      if (!isResponder()) { g_screen = SCR_HOME; break; }
      handleSosList(ev);
      break;

    case SCR_SOSVIEW:
      // The reply is the 4 + 6 hold (helpTick); keys here only go back.
      if (press && (ev.key == FM_KP_D || ev.key == FM_KP_C)) g_screen = SCR_SOSLIST;
      break;

    case SCR_STATUS:
      if (press && ev.key == FM_KP_A && g_heardSel > 0) g_heardSel--;
      else if (press && ev.key == FM_KP_B) g_heardSel++;   // clamped when drawn
      else if (press && ev.key == FM_KP_C) setPing(!g_ping);
      else if (press && ev.key == FM_KP_D) g_screen = SCR_HOME;
      else if (ev.type == FM_KEV_RELEASE && !ev.wasLong && !g_chord) {
        // * and # act on release, like the editor, so the SOS chord stays clean.
        if (ev.key == FM_KP_HASH) {
          setPowerMode(g_pwrMode == PWR_AUTO ? PWR_ON : g_pwrMode == PWR_ON ? PWR_OFF : PWR_AUTO);
        } else if (ev.key == FM_KP_STAR) {
          g_coverSel = 0;
          g_screen = SCR_COVER;
        }
      }
      break;

    case SCR_COVER:
      if (!press) break;
      if (ev.key == FM_KP_A && g_coverSel > 0) g_coverSel--;
      else if (ev.key == FM_KP_B) g_coverSel++;   // clamped when drawn
      else if (ev.key == FM_KP_D) g_screen = SCR_STATUS;
      break;
  }
}

/** * and # held together: the SOS channel screen after FM_SOS_HOLD_MS, from any screen. */
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
      g_sosPick = true;
      g_sosPickUntil = now + FM_SOS_PICK_MS;
      g_popup = false;
      Serial.println("[SOS] >> * + # held << channel screen");
      buzzStart(1, 120, 0);
    }
  } else if (!star && !hash) {
    g_chord = false;
  }
  // Nobody picked: send it as General (§13.8), so a panicking user still gets an SOS out.
  if (g_sosPick && (int32_t)(now - g_sosPickUntil) >= 0) {
    Serial.println("[SOS] no channel picked in time - GENERAL");
    startSos(FM_SOS_GENERAL);
  }
}

/**
 * Responder: 4 and 6 held together on the SOS detail screen send HELP IS
 * COMING after FM_HELP_HOLD_MS; releasing either key before then cancels.
 */
static void helpTick(uint32_t now) {
  if (helpChordHeld() && isResponder()) {
    if (!g_helpChord) {
      g_helpChord = true;
      g_helpFired = false;
      wake(now);
    }
    const uint32_t held = min(fmKeypadHeldMs(FM_KP_4, now), fmKeypadHeldMs(FM_KP_6, now));
    if (!g_helpFired && held >= FM_HELP_HOLD_MS) {
      g_helpFired = true;
      // On success the hold screen shows "sent" until release; on failure the
      // NOT SENT popup stays up instead.
      if (sendHelpComing(g_sosViewIdx)) g_popup = false;
    }
  } else if (!fmKeypadHeld(FM_KP_4) && !fmKeypadHeld(FM_KP_6)) {
    if (g_helpChord && !g_helpFired) Serial.println("[SOS] HELP IS COMING cancelled (released early)");
    g_helpChord = false;
  }
}

/**
 * Responder alarms after arrival (§13.8): an SOS in a hidden channel escalates
 * when nobody has answered (Medical after 5 min, the others after 15), and an
 * unanswered SOS in the main list reminds again every 15 min until someone
 * sends HELP IS COMING.
 */
static void escalateTick(uint32_t now) {
  if (!isResponder() || (now - g_lastEscCheck) < 1000) return;
  g_lastEscCheck = now;
  bool first = false;
  const int idx = fmSosRxEscalateTick(now, &first);
  FmSosEntry *e = fmSosEntryAt(idx);
  if (!e) return;
  char meta[40], age[8], cs[FM_CALLSIGN_LEN + 1];
  fmtAge(now - e->firstAt, age, sizeof(age));
  trimCs(e->cs, cs, sizeof(cs));
  snprintf(meta, sizeof(meta), "%s  %s  %s, no reply", cs, fmSosChShort(e->ch), age);
  Serial.printf("[SOS] %s SOS from %s %s: no HELP IS COMING after %s (alarm %u)\n",
                fmSosChName(e->ch), cs, first ? "ESCALATED" : "reminder", age,
                (unsigned)e->alarms);
  if (first) {
    popup("!! SOS ESCALATED !!", meta,
          "Nobody has sent Help is coming. Open it from the SOS list.", true);
  } else {
    popup("!! SOS WAITING !!", meta, "Still no Help is coming. Reminder every 15 min.", true);
  }
  buzzStart(30, 700, 300);
  showSosList();
}

// ---------------------------------------------------------------- sleep
static bool canSleep(uint32_t now) {
  if (!FM_LIGHT_SLEEP || !g_sleepEnabled || g_powered) return false;
  // The screen is on: someone is using the unit. Without an OLED, the same timeout.
  const bool idle = g_oledOk ? g_screenOff : (now - g_lastActivity) >= FM_SCREEN_OFF_MS;
  if (!idle) return false;
  if (g_popup || g_bzPulses || g_bzState || g_sosPick || g_ping) return false;
  // fmLogBusy(): a field-logger upload window has WiFi up; light sleep would drop it.
  if (fmKeypadMask() || fmMeshBusy() || fmOtaActive() || fmLogBusy()) return false;
  if ((now - g_lastSerialMs) < 30000UL) return false;   // someone on the serial console
  return true;
}

static void sleepTick(uint32_t now) {
  if (!canSleep(now)) return;
  uint32_t ms = FM_SLEEP_MAX_MS;
  const uint32_t sosIn = fmSosNextInMs(now);
  if (sosIn < ms) ms = sosIn;
  const uint32_t logIn = fmLogNextInMs(now);   // awake when the next upload window is due
  if (logIn < ms) ms = logIn;
  if (ms < 5) return;   // not worth it
  fmSleepLight(ms);
}

// ---------------------------------------------------------------- firmware trial (rollback)
/**
 * Arduino marks a freshly installed image as good at boot unless told to wait.
 * V4 waits: the image stays on trial until the radio has come up and the unit
 * has run FM_OTA_VERIFY_MS. A reset before that (a crash loop, a hang the
 * watchdog catches) makes the bootloader go back to the previous image.
 */
extern "C" bool verifyRollbackLater() { return true; }

static bool otaImagePending() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  return running && esp_ota_get_state_partition(running, &st) == ESP_OK &&
         st == ESP_OTA_IMG_PENDING_VERIFY;
}

static void otaMarkGood(const char *why) {
  if (!otaImagePending()) {
    g_otaPendingVerify = false;
    return;
  }
  esp_ota_mark_app_valid_cancel_rollback();
  g_otaPendingVerify = false;
  Serial.printf("[OTA] new firmware kept (%s)\n", why);
}

static void otaTrialBegin(bool radioOk) {
  if (!otaImagePending()) return;
  if (!radioOk) {
    Serial.println("[OTA] new firmware failed its self-test (radio) - going back to the old one");
    delay(200);
    esp_ota_mark_app_invalid_rollback_and_reboot();
    return;
  }
  g_otaPendingVerify = true;
  Serial.printf("[OTA] new firmware on trial: kept after %lu s of normal running\n",
                (unsigned long)(FM_OTA_VERIFY_MS / 1000));
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
    p.remove(FM_NVS_KEY_PWRMODE);    // and power mode back to AUTO
    p.end();
  }
  if (p.begin("fmsos", false)) {     // SOS channel selection back to "all"
    p.clear();
    p.end();
  }
  fmOtaForget();                     // and no stored WiFi network
  fmLogForgetSettings();             // nor network 2 and the log server (the records stay)
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
      // An operator is at the unit and chose to reset it: keep the firmware
      // it is running rather than letting the restart roll it back.
      otaMarkGood("factory reset");
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
    // Provisioning ends in a restart; an image still on trial would be rolled back.
    otaMarkGood("provisioning");
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

static void printCover() {
  uint8_t idx[COVER_SLOTS];
  const uint8_t n = coverSorted(idx);
  Serial.printf("[COVER] %u powered unit(s) heard\n", (unsigned)n);
  for (uint8_t i = 0; i < n; i++) {
    const Cover &c = g_cover[idx[i]];
    const uint32_t age = millis() - c.at;
    Serial.printf("  %-5s batt %3u%%  hops %u  %4d dBm  last %lus ago  x%u  %s\n", c.cs,
                  (unsigned)c.batt, (unsigned)c.hops, c.rssi, (unsigned long)(age / 1000),
                  (unsigned)c.count, coverState(age));
  }
}

static void printSos() {
  if (!isResponder()) {
    Serial.println("[SOS] this is not a responder unit: SOS are forwarded, not listed");
    return;
  }
  uint8_t idx[FM_SOS_TABLE];
  const uint8_t n = fmSosList(-1, idx, FM_SOS_TABLE);
  Serial.printf("[SOS] selection mask 0x%02X, %u in the main list\n",
                (unsigned)fmSosSelectionMask(), (unsigned)n);
  for (uint8_t i = 0; i < n; i++) {
    const FmSosEntry *e = fmSosEntryAt(idx[i]);
    Serial.printf("  %s %-11s first %lus ago x%u %s%s\n", e->cs, fmSosChName(e->ch),
                  (unsigned long)((millis() - e->firstAt) / 1000), (unsigned)e->count,
                  e->helpBy[0] ? "help:" : "", e->helpBy[0] ? e->helpBy : "");
  }
}

static void pollSerial() {
  static char line[96];
  static size_t n = 0;
  static bool overflow = false;
  while (Serial.available()) {
    const char c = (char)Serial.read();
    g_lastSerialMs = millis();
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
      Serial.println("[CMD] help | info | prov | sos <0-4> | sos stop | sos list | select <1-4>"
                     " | text <msg> | ping on|off | heard | cover | power auto|on|off"
                     " | sleep on|off | beep | callsign <1-5 of A-Z 0-9> | wifi"
                     " | wifi ssid <name> | wifi pass <pw> | wifi forget | ota | ota off"
                     " | batt | batt cal <volts> | batt cal reset | beep tone <Hz>"
                     " | buzzer | buzzer active | buzzer passive [Hz]");
      Serial.println("[CMD] field log: log | log send | log url <base> | log url default"
                     " | log clear | wifi2 ssid <name> | wifi2 pass <pw> | wifi2 forget");
      Serial.println("[CMD] SOS channels: 0 GENERAL, 1 MEDICAL, 2 EVACUATION, 3 HAZARD, 4 FOOD");
    } else if (strncmp(line, "wifi", 4) == 0) {
      // Every "wifi..." line ends here, so a mistyped one can never reach the
      // "unknown" branch below, which would echo a password to the log.
      // "wifi2 ..." (the field logger's second network) included.
      if (fmLogWifi2Command(line)) {
        // handled, password never echoed
      } else if (strncmp(line, "wifi ssid ", 10) == 0) {
        if (fmOtaSetSsid(line + 10)) Serial.printf("[WIFI] network set to '%s'\n", fmOtaSsid());
        else Serial.println("[WIFI] network refused - 1 to 32 printable characters");
      } else if (strncmp(line, "wifi pass ", 10) == 0 || strcmp(line, "wifi pass") == 0) {
        if (fmOtaSetPass(line[9] ? line + 10 : "")) Serial.println("[WIFI] password saved");
        else Serial.println("[WIFI] password refused - 8 to 63 printable characters");
      } else if (strcmp(line, "wifi forget") == 0) {
        fmOtaForget();
        Serial.println("[WIFI] network forgotten");
      } else if (strcmp(line, "wifi") == 0) {
        Serial.printf("[WIFI] network 1 %s, password %s (updates and field log), "
                      "update window %s\n",
                      fmOtaHasSsid() ? fmOtaSsid() : "not set",
                      fmOtaHasPass() ? "saved" : "not set", fmOtaActive() ? "OPEN" : "closed");
        Serial.printf("[WIFI] network 2 %s, password %s (field log only, tried second)\n",
                      fmLogHasSsid2() ? fmLogSsid2() : "not set",
                      fmLogHasPass2() ? "saved" : "not set");
      } else {
        Serial.println("[WIFI] usage: wifi | wifi ssid <name> | wifi pass <pw> | wifi forget"
                       " | wifi2 ssid <name> | wifi2 pass <pw> | wifi2 forget");
      }
      memset(line, 0, sizeof(line));
    } else if (strcmp(line, "ota") == 0) {
      if (fmOtaStart(FM_OTA_WINDOW_MS)) popup("UPDATE MODE", "joining WiFi", fmOtaSsid(), false, 30000);
    } else if (strcmp(line, "ota off") == 0) {
      fmOtaStop();
    } else if (strcmp(line, "info") == 0) {
      char fp[17];
      fmAdminFingerprint(fp);
      const uint32_t up = millis() ? millis() : 1;
      Serial.printf("[CMD] %s v%s role=%s left=%lus batt=%.2fV power=%s(%s) sleep=%lu%% "
                    "air=%u permille relayed=%lu suppressed=%lu SF%u admin=%s\n",
                    fmCallSign(), FLOODMESH_VERSION, isResponder() ? "responder" : "civilian",
                    (unsigned long)fmRoleRemainingS(), g_battV, g_powered ? "external" : "battery",
                    pwrModeName(g_pwrMode),
                    (unsigned long)((uint64_t)fmSleepTotalMs() * 100u / up), fmAirtimePermille(),
                    (unsigned long)fmMeshFramesRelayed(), (unsigned long)fmMeshFramesSuppressed(),
                    (unsigned)FM_LORA_SF, fp[0] ? fp : "none");
    } else if (strncmp(line, "callsign ", 9) == 0) {
      // Operator override, stored in NVS; * + 0 factory reset returns to the MAC one.
      if (fmSetCallSign(line + 9)) Serial.printf("[CMD] call sign now %s\n", fmCallSign());
      else Serial.printf("[CMD] callsign refused - 1-5 characters, A-Z 0-9 only, still %s\n",
                         fmCallSign());
    } else if (strcmp(line, "prov") == 0) {
      Serial.println("[CMD] rebooting into provisioning mode");
      otaMarkGood("provisioning");
      g_bootReq = FM_BOOTREQ_PROV;
      delay(100);
      ESP.restart();
    } else if (strcmp(line, "sos stop") == 0) {
      stopOwnSos();
      Serial.println("[SOS] retries stopped");
    } else if (strcmp(line, "sos list") == 0) {
      printSos();
    } else if (strncmp(line, "sos ", 4) == 0) {
      const int ch = atoi(line + 4);
      if (line[4] >= '0' && line[4] <= '9' && ch >= 0 && ch < (int)FM_SOS_CH_COUNT) {
        startSos((uint8_t)ch);
      } else {
        Serial.println("[CMD] sos <0-4>: 0 GENERAL, 1 MEDICAL, 2 EVACUATION, 3 HAZARD, 4 FOOD");
      }
    } else if (strncmp(line, "select ", 7) == 0) {
      const int ch = atoi(line + 7);
      if (ch >= 1 && ch < (int)FM_SOS_CH_COUNT && fmSosToggle((uint8_t)ch)) {
        Serial.printf("[SOS] %s %s\n", fmSosChName((uint8_t)ch),
                      fmSosSelected((uint8_t)ch) ? "selected" : "removed");
      } else {
        Serial.println("[CMD] select <1-4> toggles a channel; GENERAL (0) is always selected");
      }
    } else if (strncmp(line, "text ", 5) == 0) {
      sendText(line + 5);
    } else if (strcmp(line, "ping on") == 0) {
      setPing(true);
    } else if (strcmp(line, "ping off") == 0) {
      setPing(false);
    } else if (strcmp(line, "power auto") == 0) {
      setPowerMode(PWR_AUTO);
    } else if (strcmp(line, "power on") == 0) {
      setPowerMode(PWR_ON);
    } else if (strcmp(line, "power off") == 0) {
      setPowerMode(PWR_OFF);
    } else if (strcmp(line, "batt") == 0) {
      const float raw = readBatteryRaw();
      Serial.printf("[BATT] %.3f V (raw %.3f V x factor %.3f), %u%%\n", raw * g_battCal, raw,
                    g_battCal, (unsigned)batteryPercent(raw * g_battCal));
    } else if (strcmp(line, "batt cal reset") == 0) {
      Preferences p;
      if (p.begin(FM_NVS_NAMESPACE, false)) {
        p.remove(FM_NVS_KEY_BATTCAL);
        p.end();
      }
      g_battCal = 1.0f;
      updateBattery(millis(), true);
      Serial.println("[BATT] calibration removed, factor 1.000");
    } else if (strncmp(line, "batt cal ", 9) == 0) {
      const float v = (float)atof(line + 9);
      if (setBatteryCal(v)) {
        updateBattery(millis(), true);
        Serial.printf("[BATT] calibrated: factor %.3f saved, now %.3f V\n", g_battCal, g_battV);
      } else {
        Serial.println("[BATT] calibration refused: give the multimeter reading in volts "
                       "(2.5-4.5), with the battery connected and read by this unit");
      }
    } else if (strcmp(line, "sleep on") == 0) {
      g_sleepEnabled = true;
      Serial.printf("[SLEEP] light sleep %s\n", FM_LIGHT_SLEEP ? "ON (on battery, when idle)"
                                                              : "not built in (FM_LIGHT_SLEEP=0)");
    } else if (strcmp(line, "sleep off") == 0) {
      g_sleepEnabled = false;
      Serial.println("[SLEEP] light sleep OFF until reboot");
    } else if (strcmp(line, "beep") == 0) {
      // Buzzer bench check, 3 s in the unit's own mode (long enough to measure).
      Serial.printf("[BUZZ] GPIO %d on for 3 s (%s) - measure base and collector now\n",
                    PIN_BUZZER, g_buzzHz ? "passive-buzzer tone" : "held HIGH");
      buzzStop();
      buzzPin(true);
      delay(3000);
      buzzPin(false);
      Serial.println("[BUZZ] off");
    } else if (strncmp(line, "beep tone ", 10) == 0) {
      // Try a square wave without saving it: a passive buzzer is loud only this way.
      const int hz = atoi(line + 10);
      if (hz < 500 || hz > 8000) {
        Serial.println("[BUZZ] beep tone <500-8000 Hz>");
      } else {
        Serial.printf("[BUZZ] %d Hz tone for 2 s (not saved)\n", hz);
        buzzStop();
        buzzAttach((uint16_t)hz);
        ledcWrite(kBuzzLedc, 128);
        delay(2000);
        ledcWrite(kBuzzLedc, 0);
        buzzAttach(g_buzzHz);   // back to the saved mode
        Serial.println("[BUZZ] off");
      }
    } else if (strcmp(line, "buzzer active") == 0) {
      if (setBuzzerHz(0)) Serial.println("[BUZZ] active buzzer: pin held HIGH (saved)");
    } else if (strncmp(line, "buzzer passive", 14) == 0) {
      const int hz = line[14] == ' ' ? atoi(line + 15) : 2700;
      if (setBuzzerHz((uint16_t)hz)) Serial.printf("[BUZZ] passive buzzer: %d Hz tone (saved)\n", hz);
      else Serial.println("[BUZZ] buzzer passive [500-8000 Hz, default 2700]");
    } else if (strcmp(line, "buzzer") == 0) {
      if (g_buzzHz) Serial.printf("[BUZZ] passive buzzer, %u Hz tone\n", (unsigned)g_buzzHz);
      else          Serial.println("[BUZZ] active buzzer, pin held HIGH");
    } else if (strcmp(line, "heard") == 0) {
      printHeard();
    } else if (strcmp(line, "cover") == 0) {
      printCover();
    } else if (fmLogCommand(line)) {
      // log, log send, log url, log clear: fm_log.cpp
    } else if (line[0]) {
      Serial.printf("[CMD] unknown '%s' - try help\n", line);
    }
  }
}

// ---------------------------------------------------------------- setup / loop
/** The field logger's status record (fm_log.h): called from fmLogLoop(), in this task. */
static void logStatusFill(FmLogStatus *s) {
  s->battV = g_battV;
  s->battPct = g_battPct;
  s->powered = g_powered;
  s->airPermille = fmAirtimePermille();
  uint8_t n = 0;
  for (uint8_t i = 0; i < HEARD_SLOTS; i++) n += g_heard[i].used ? 1 : 0;
  s->heard = n;
  const uint32_t up = millis() ? millis() : 1;
  const uint64_t pct = (uint64_t)fmSleepTotalMs() * 100u / up;
  s->sleepPct = (uint8_t)(pct > 100 ? 100 : pct);
  s->responder = isResponder();
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  loadBuzzer();   // active (pin HIGH) or passive (tone), a property of the board: kept by factory reset
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
  Serial.printf("  FloodMesh firmware V4 %s - SOS channels, keypad, no voice\n", FLOODMESH_VERSION);
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
    oled.drawStr(6, 28, "FloodMesh V4");
    oled.setFont(u8g2_font_5x8_tf);
    oled.drawStr(6, 44, FLOODMESH_VERSION);
    oled.sendBuffer();
  }

  memset(g_pressedOn, kNoScreen, sizeof(g_pressedOn));
  fmKeypadBegin();
  fmIdsBegin();
  Serial.printf("[ID] call sign %s (%s)\n", fmCallSign(),
                fmCallSignIsDerived() ? "derived from MAC" : "operator-assigned");
  fmRoleBegin();
  if (fmRole() == FM_ROLE_RELAY) {
    Serial.println("[ROLE] 'relay' is not a role in V4: this unit acts as a civilian unit, and "
                   "external power decides whether it stays awake and forwards everything");
  }
  bootCombos();   // may enter provisioning mode, which never returns
  fmOtaBegin(provDraw);
  const bool otaResume = fmOtaResumePending();   // read once: it clears the flag
  // After bootCombos(): BLE provisioning never returns, so the uploader task
  // and its WiFi never exist while BLE runs. The boot record goes out first.
  fmLogBegin(logStatusFill);

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
  fmSosBegin();
  fmEditClear();

  const bool radioOk = fmRadioBegin();
  if (!radioOk) {
    Serial.println("[RADIO] !! SX1262 init FAILED - the device cannot reach the mesh.");
    beepBlocking(400, 3, 150);
  } else {
    fmMeshSetAlarmHandler(onAlarmRx);
    fmMeshSetTextHandler(onTextRx);
    fmMeshSetAckHandler(onAckRx);
    fmMeshSetHeartbeatHandler(onHeartbeatRx);
    fmMeshSetEchoHandler(onEcho);
    fmMeshBegin();
  }
  otaTrialBegin(radioOk);

  fmPowerBegin();                  // look for the GPIO 3 power-sense divider
  loadPowerMode();                 // AUTO / ON / OFF, kept across power-offs
  loadBatteryCal();                // per-unit battery factor, `batt cal`
  updateBattery(millis(), true);   // also decides external power vs battery
  Serial.printf("[BATT] %.3f V (%u%%)\n", g_battV, g_battPct);
  Serial.printf("[SOS] responder channel selection 0x%02X\n", (unsigned)fmSosSelectionMask());
  Serial.println("[CMD] type 'help' for serial commands");
  beepBlocking(100, 2, 80);   // under ~50 ms a buzzer is still starting up: a muffled tick
  g_lastActivity = millis();
  g_lastSerialMs = millis();   // stay awake long enough for a console to attach

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
    if (g_screen == SCR_SOSLIST || g_screen == SCR_SOSVIEW) g_screen = SCR_HOME;
  }

  const FmKeyEvent ev = fmKeypadPoll(now);
  handleKey(ev, now);
  sosTick(now);
  helpTick(now);
  sosToggleTick(now);
  if (g_screen == SCR_COMPOSE) fmEditTick(now);

  sosSenderTick(now);
  escalateTick(now);
  heartbeatTick(now);

  if (g_popup && !g_popupSticky && (int32_t)(now - g_popupUntil) >= 0) g_popup = false;
  if (!g_popup && !g_bzPulses && !g_screenOff && g_oledOk && !g_sosPick &&
      (now - g_lastActivity) >= FM_SCREEN_OFF_MS && g_screen != SCR_COMPOSE) {
    oled.setPowerSave(1);
    g_screenOff = true;
  }

  if (g_otaPendingVerify && now >= FM_OTA_VERIFY_MS) otaMarkGood("self-test passed");

  buzzTick(now);
  pingTick(now);
  updateBattery(now, false);
  fmLogLoop(now);   // this pass's records to flash; status records; upload windows
  digitalWrite(PIN_LED, g_ping && ((now / 100) % 20 == 0) ? HIGH : LOW);
  draw(now, false);

  sleepTick(millis());
}
