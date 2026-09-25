/**
 * FloodMesh V3 - direct-GPIO 4x4 keypad. See fm_keypad.h.
 */
#include "fm_keypad.h"

#include "floodmesh_pins.h"

namespace {

const uint8_t kRows[4] = {PIN_KP_ROW1, PIN_KP_ROW2, PIN_KP_ROW3, PIN_KP_ROW4};
const uint8_t kCols[4] = {PIN_KP_COL1, PIN_KP_COL2, PIN_KP_COL3, PIN_KP_COL4};
const char kChars[FM_KP_KEYS] = {'1', '2', '3', 'A', '4', '5', '6', 'B',
                                 '7', '8', '9', 'C', '*', '0', '#', 'D'};

uint16_t g_raw = 0;          // last raw scan
uint16_t g_stable = 0;       // debounced state
uint32_t g_edgeMs[FM_KP_KEYS];
uint32_t g_downMs[FM_KP_KEYS];
uint16_t g_longSent = 0;     // keys whose LONG already fired this hold

}  // namespace

uint16_t fmKeypadScanRaw() {
  uint16_t mask = 0;
  for (uint8_t r = 0; r < 4; r++) {
    for (uint8_t q = 0; q < 4; q++) pinMode(kRows[q], INPUT);   // all rows high-Z
    pinMode(kRows[r], OUTPUT);
    digitalWrite(kRows[r], LOW);
    delayMicroseconds(40);                                       // let pull-ups settle
    for (uint8_t c = 0; c < 4; c++) {
      if (digitalRead(kCols[c]) == LOW) mask |= FM_KP_BIT(r * 4 + c);
    }
  }
  for (uint8_t q = 0; q < 4; q++) pinMode(kRows[q], INPUT);
  return mask;
}

void fmKeypadBegin() {
  for (uint8_t c = 0; c < 4; c++) pinMode(kCols[c], INPUT_PULLUP);
  for (uint8_t r = 0; r < 4; r++) pinMode(kRows[r], INPUT);
  memset(g_edgeMs, 0, sizeof(g_edgeMs));
  memset(g_downMs, 0, sizeof(g_downMs));
  g_raw = g_stable = fmKeypadScanRaw();
  g_longSent = 0;
  Serial.printf("[KEYS] 4x4 keypad: cols GPIO %u/%u/%u/%u, rows GPIO %u/%u/%u/%u\n",
                PIN_KP_COL1, PIN_KP_COL2, PIN_KP_COL3, PIN_KP_COL4, PIN_KP_ROW1,
                PIN_KP_ROW2, PIN_KP_ROW3, PIN_KP_ROW4);
  if (g_stable) {
    char s[20];
    fmKeypadMaskToString(g_stable, s, sizeof(s));
    Serial.printf("[KEYS] held at boot: %s\n", s);
  }
}

FmKeyEvent fmKeypadPoll(uint32_t now) {
  FmKeyEvent ev = {FM_KEV_NONE, 0, false};
  const uint16_t raw = fmKeypadScanRaw();
  const uint16_t changed = raw ^ g_raw;
  for (uint8_t k = 0; k < FM_KP_KEYS; k++) {
    if (changed & FM_KP_BIT(k)) g_edgeMs[k] = now;
  }
  g_raw = raw;

  // Debounced edges: at most one reported per call, lowest index first.
  for (uint8_t k = 0; k < FM_KP_KEYS; k++) {
    const bool r = (raw & FM_KP_BIT(k)) != 0;
    const bool s = (g_stable & FM_KP_BIT(k)) != 0;
    if (r == s || (now - g_edgeMs[k]) < FM_KP_DEBOUNCE_MS) continue;
    if (r) {
      g_stable |= FM_KP_BIT(k);
      g_downMs[k] = now;
      g_longSent &= (uint16_t)~FM_KP_BIT(k);
      ev.type = FM_KEV_PRESS;
      ev.key = k;
    } else {
      g_stable &= (uint16_t)~FM_KP_BIT(k);
      ev.type = FM_KEV_RELEASE;
      ev.key = k;
      ev.wasLong = (g_longSent & FM_KP_BIT(k)) != 0;
      g_longSent &= (uint16_t)~FM_KP_BIT(k);
    }
    return ev;
  }

  // Long presses.
  for (uint8_t k = 0; k < FM_KP_KEYS; k++) {
    if ((g_stable & FM_KP_BIT(k)) && !(g_longSent & FM_KP_BIT(k)) &&
        (now - g_downMs[k]) >= FM_KP_LONG_MS) {
      g_longSent |= FM_KP_BIT(k);
      ev.type = FM_KEV_LONG;
      ev.key = k;
      return ev;
    }
  }
  return ev;
}

uint16_t fmKeypadMask() { return g_stable; }

bool fmKeypadHeld(uint8_t key) {
  return key < FM_KP_KEYS && (g_stable & FM_KP_BIT(key)) != 0;
}

uint32_t fmKeypadHeldMs(uint8_t key, uint32_t now) {
  return fmKeypadHeld(key) ? (now - g_downMs[key]) : 0;
}

char fmKeypadChar(uint8_t key) { return key < FM_KP_KEYS ? kChars[key] : '?'; }

void fmKeypadMaskToString(uint16_t mask, char *out, size_t n) {
  if (!out || n == 0) return;
  size_t w = 0;
  for (uint8_t i = 0; i < FM_KP_KEYS && w + 1 < n; i++) {
    if (mask & FM_KP_BIT(i)) out[w++] = kChars[i];
  }
  out[w] = '\0';
}
