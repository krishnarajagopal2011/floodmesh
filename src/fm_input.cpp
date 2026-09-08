/**
 * FloodMesh - 5-key input state machine over a scanned 2x2 matrix.
 * See fm_input.h for the state diagram and the ordering problems it solves.
 *
 * The keypad is a MATRIX with no ground pin: a key bridges one column line to
 * one row line. Reading four pulled-up inputs detects nothing, because a press
 * merely ties two high pins together. Each poll therefore drives one row low at
 * a time and reads the columns.
 *
 * The idle row is left high-Z rather than driven high. Driving it would put two
 * outputs in opposition through any pressed key on the other row - a dead short
 * between a driven high and a driven low.
 *
 * All time arithmetic uses unsigned subtraction (now - then), which is correct
 * across the 49.7-day millis() rollover. Never compare timestamps directly.
 */
#include "fm_input.h"

namespace {

struct Btn {
  bool     raw;          // last scanned level (true = pressed)
  bool     stable;       // debounced state
  uint32_t lastEdgeMs;   // when raw last changed
  uint32_t pressedAt;    // millis() at the debounced press edge
  uint16_t presses;      // debounced press count since boot
  bool     navPending;   // face key is down and still eligible to navigate
};

Btn g_btn[FM_BTN_COUNT];

FmInputState g_state       = FM_IN_STANDBY;
uint32_t     g_recStartMs  = 0;   // when the current capture began
uint32_t     g_heldClipMs  = 0;   // length of the clip awaiting a send decision
uint32_t     g_pendingAt   = 0;   // when the confirm window opened
uint32_t     g_lastAlarmMs = 0;
FmAlarm      g_lastAlarm   = FM_ALARM_NONE;

// Face keys are indexed by physical position, and fm_input.h's FM_BTN_* order
// matches floodmesh_pins.h's FM_KEY_* exactly:
//   0 top-right, 1 bottom-right, 2 bottom-left, 3 top-left
const FmNav kNavFor[4] = {FM_NAV_CH_UP, FM_NAV_CH_DOWN, FM_NAV_INBOX_PREV, FM_NAV_PLAY};
const FmAlarm kAlarmFor[4] = {FM_ALARM_SAFE,      // top-right
                              FM_ALARM_WATER,     // bottom-right
                              FM_ALARM_MEDICAL,   // bottom-left
                              FM_ALARM_EVACUATE}; // top-left

const char *kKeyName[4] = {"top-right", "bottom-right", "bottom-left", "top-left"};

/**
 * Read the whole matrix. out[] is indexed by FM_KEY_* / FM_BTN_*.
 *
 * Ghosting: with no diodes, three keys held at once would fabricate a fourth.
 * In a 2x2 no two-key combination can ghost, and every FloodMesh gesture is at
 * most two keys, so no rejection logic is needed.
 */
void scanKeys(bool out[FM_KEY_COUNT]) {
  const uint8_t rows[2] = {PIN_KEY_ROW_R1, PIN_KEY_ROW_R2};
  const uint8_t cols[2] = {PIN_KEY_COL_L1, PIN_KEY_COL_L2};
  bool k[2][2] = {{false, false}, {false, false}};

  for (uint8_t r = 0; r < 2; r++) {
    pinMode(rows[r ^ 1], INPUT);          // idle row floats - see file header
    pinMode(cols[0], INPUT_PULLUP);
    pinMode(cols[1], INPUT_PULLUP);
    pinMode(rows[r], OUTPUT);
    digitalWrite(rows[r], LOW);
    delayMicroseconds(60);                // let the pull-ups charge the lines
    k[r][0] = (digitalRead(cols[0]) == LOW);
    k[r][1] = (digitalRead(cols[1]) == LOW);
  }
  pinMode(rows[0], INPUT);                // leave nothing driven between polls
  pinMode(rows[1], INPUT);

  out[FM_KEY_TL] = k[0][0];   // R1 x L1
  out[FM_KEY_TR] = k[0][1];   // R1 x L2
  out[FM_KEY_BL] = k[1][0];   // R2 x L1
  out[FM_KEY_BR] = k[1][1];   // R2 x L2
}

/**
 * True if face index i is an independent key on this build.
 *
 * In bench test mode the bottom-right key doubles as the PTT. Letting it act as
 * both would make every PTT press look like a simultaneous face press and fire
 * an alarm chord nobody asked for, so that index is skipped.
 */
bool faceActive(uint8_t i) {
#if FM_PTT_IS_MATRIX_KEY
  return i != FM_PTT_KEY;
#else
  (void)i;
  return true;
#endif
}

/** Debounce one key from its scanned level. True on a fresh press edge. */
bool sample(Btn &b, bool raw, uint32_t now, bool *released) {
  *released = false;
  if (raw != b.raw) {
    b.raw = raw;
    b.lastEdgeMs = now;
  }
  if ((now - b.lastEdgeMs) < FM_DEBOUNCE_MS || b.stable == b.raw) {
    return false;
  }
  b.stable = b.raw;
  if (b.stable) {
    b.pressedAt = now;
    b.presses++;
    return true;
  }
  *released = true;
  return false;
}

/** Fire a Layer 1 chord. Drops any recording or held clip, then latches. */
void fireChord(uint8_t face, uint32_t now, FmInputEvent &ev) {
  if (FM_ALARM_MIN_GAP_MS > 0 && g_lastAlarm != FM_ALARM_NONE &&
      (now - g_lastAlarmMs) < FM_ALARM_MIN_GAP_MS) {
    return;  // Layer 1 is rate-limit-free by default; this compiles out.
  }

  if (ev.recStart) {
    // PTT and the face key landed in the same poll tick: cancel the start
    // rather than emitting open-then-immediately-drop to the caller.
    ev.recStart = false;
  } else if (g_state == FM_IN_RECORDING) {
    ev.recStop = true;
    ev.recDrop = true;
    ev.dropReason = FM_DROP_ALARM_CHORD;
    ev.recMs = now - g_recStartMs;
  } else if (g_state == FM_IN_CAPPED) {
    ev.recDrop = true;                 // mic already stopped at the cap
    ev.dropReason = FM_DROP_ALARM_CHORD;
    ev.recMs = g_heldClipMs;
  }

  g_heldClipMs = 0;
  ev.alarmFired = true;
  ev.alarm = kAlarmFor[face];
  g_lastAlarm = ev.alarm;
  g_lastAlarmMs = now;
  g_state = FM_IN_ALARM_LATCHED;

  for (uint8_t i = 0; i < 4; i++) {
    g_btn[i].navPending = false;
  }
}

/** Stop capturing and open the send/cancel decision window. */
void openConfirm(uint32_t now, uint32_t clipMs, FmInputEvent &ev) {
  g_heldClipMs = clipMs;
  g_pendingAt = now;
  g_state = FM_IN_PENDING_SEND;
  ev.sendPending = true;
  ev.recMs = clipMs;
  for (uint8_t i = 0; i < 4; i++) {
    g_btn[i].navPending = false;
  }
}

}  // namespace

// ---------------------------------------------------------------- API
void fmInputBegin() {
  memset(g_btn, 0, sizeof(g_btn));

#if !FM_PTT_IS_MATRIX_KEY
  if (PTT_HARDWARE_INSTALLED) {
    pinMode(PIN_BTN_PTT, INPUT_PULLUP);
  } else {
    Serial.println("[WARN] PTT_HARDWARE_INSTALLED=0: no recording and NO ALARMS.");
    Serial.println("[WARN] Every Layer 1 alarm is a PTT chord, so this build is");
    Serial.println("[WARN] a receive-only pager until the switch is fitted.");
  }
#endif

  Serial.printf("[KEYS] 2x2 matrix: cols L1=GPIO%u L2=GPIO%u, rows R1=GPIO%u R2=GPIO%u\n",
                PIN_KEY_COL_L1, PIN_KEY_COL_L2, PIN_KEY_ROW_R1, PIN_KEY_ROW_R2);

  // A key held (or a shorted line) at boot is a wiring fault worth naming now.
  bool keys[FM_KEY_COUNT];
  scanKeys(keys);
  for (uint8_t i = 0; i < 4; i++) {
    g_btn[i].raw = g_btn[i].stable = keys[i];
    if (keys[i]) {
      Serial.printf("[WARN] %s key reads pressed at boot - held down, or a "
                    "shorted matrix line.\n",
                    kKeyName[i]);
    }
  }
  g_state = FM_IN_STANDBY;
}

FmInputEvent fmInputPoll(uint32_t now) {
  FmInputEvent ev = {FM_NAV_NONE, false, false, false, false,
                     false,       false, false, FM_ALARM_NONE,
                     FM_DROP_NONE, 0};

  bool keys[FM_KEY_COUNT];
  scanKeys(keys);

  bool pttReleased = false;
#if PTT_HARDWARE_INSTALLED
  #if FM_PTT_IS_MATRIX_KEY
  const bool pttRaw = keys[FM_PTT_KEY];
  #else
  const bool pttRaw = (digitalRead(PIN_BTN_PTT) == LOW);
  #endif
  const bool pttPressed = sample(g_btn[FM_BTN_PTT], pttRaw, now, &pttReleased);
  const bool pttHeld = g_btn[FM_BTN_PTT].stable;
#else
  // No switch fitted: the modifier is permanently up, so the keypad is pure
  // navigation and every chord path below compiles out to dead code.
  const bool pttPressed = false;
  const bool pttHeld = false;
#endif

  // --- PTT down: enter Mode B and start capturing immediately ------------
  if (pttPressed) {
    // Reverse-order chord: a face key pressed shortly BEFORE PTT counts.
    int8_t joined = -1;
    for (uint8_t i = 0; i < 4; i++) {
      if (!faceActive(i)) continue;
      if (g_btn[i].stable && g_btn[i].navPending &&
          (now - g_btn[i].pressedAt) <= FM_CHORD_JOIN_MS) {
        joined = (int8_t)i;
        break;
      }
    }
    if (joined >= 0) {
      fireChord((uint8_t)joined, now, ev);
      return ev;
    }

    // PTT during the confirm window means "scrap that, let me say it again".
    if (g_state == FM_IN_PENDING_SEND) {
      ev.recDrop = true;
      ev.dropReason = FM_DROP_RERECORD;
      ev.recMs = g_heldClipMs;
      g_heldClipMs = 0;
    }

    g_state = FM_IN_RECORDING;
    g_recStartMs = now;
    ev.recStart = true;
    for (uint8_t i = 0; i < 4; i++) {
      g_btn[i].navPending = false;
    }
  }

  // --- face keys ---------------------------------------------------------
  for (uint8_t i = 0; i < 4; i++) {
    if (!faceActive(i)) continue;
    bool released = false;
    const bool pressed = sample(g_btn[i], keys[i], now, &released);

    if (pressed) {
      if (pttHeld) {
        fireChord(i, now, ev);   // Mode B: chord, in the natural press order
        return ev;               // one alarm per poll; the rest waits a tick
      }
      if (g_state == FM_IN_PENDING_SEND) {
        if (i == FM_BTN_PREV) {        // bottom-left cancels immediately
          ev.recDrop = true;
          ev.dropReason = FM_DROP_USER_CANCEL;
          ev.recMs = g_heldClipMs;
          g_heldClipMs = 0;
          g_state = FM_IN_STANDBY;
          return ev;
        }
        continue;  // top-left is judged on hold duration below, not on the edge
      }
      g_btn[i].navPending = true;  // Mode A: decide at release, not now
    }

    if (released) {
      if (g_btn[i].navPending && !pttHeld && g_state == FM_IN_STANDBY) {
        ev.nav = kNavFor[i];
      }
      g_btn[i].navPending = false;
    }
  }

  // --- confirm window: hold top-left to send now, or let the countdown run
  if (g_state == FM_IN_PENDING_SEND) {
    const bool holdSend = g_btn[FM_BTN_SELECT].stable &&
                          (now - g_btn[FM_BTN_SELECT].pressedAt) >= FM_SEND_HOLD_MS;
    const bool expired = (now - g_pendingAt) >= FM_SEND_CONFIRM_MS;

    if (holdSend || (expired && FM_SEND_ON_TIMEOUT)) {
      ev.sendCommit = true;
      ev.recMs = g_heldClipMs;
      g_heldClipMs = 0;
      g_state = FM_IN_STANDBY;
    } else if (expired) {
      // FM_SEND_ON_TIMEOUT == 0: silence is a refusal, not consent.
      ev.recDrop = true;
      ev.dropReason = FM_DROP_USER_CANCEL;
      ev.recMs = g_heldClipMs;
      g_heldClipMs = 0;
      g_state = FM_IN_STANDBY;
    }
    return ev;
  }

  // --- recording lifecycle ----------------------------------------------
  if (g_state == FM_IN_RECORDING && (now - g_recStartMs) >= FM_VOICE_MAX_MS) {
    // Hard cap. The mic stops here and the clip is held, but the send decision
    // waits for PTT to come up - while PTT is down every face key is an alarm
    // chord and cannot mean "cancel".
    g_state = FM_IN_CAPPED;
    g_heldClipMs = FM_VOICE_MAX_MS;
    ev.recStop = true;
    ev.recCapped = true;
    ev.recMs = FM_VOICE_MAX_MS;
  }

  if (pttReleased) {
    if (g_state == FM_IN_RECORDING) {
      const uint32_t e = now - g_recStartMs;
      ev.recStop = true;
      openConfirm(now, e > FM_VOICE_MAX_MS ? FM_VOICE_MAX_MS : e, ev);
    } else if (g_state == FM_IN_CAPPED) {
      openConfirm(now, g_heldClipMs, ev);  // mic already stopped at the cap
    } else {
      // FM_IN_ALARM_LATCHED -> the clip was dropped; releasing sends nothing.
      g_state = FM_IN_STANDBY;
    }
  }

  return ev;
}

FmInputState fmInputState() { return g_state; }

uint32_t fmRecordElapsedMs() {
  if (g_state == FM_IN_CAPPED) return FM_VOICE_MAX_MS;
  if (g_state != FM_IN_RECORDING) return 0;
  const uint32_t e = millis() - g_recStartMs;
  return e > FM_VOICE_MAX_MS ? FM_VOICE_MAX_MS : e;
}

uint32_t fmHeldClipMs() { return g_heldClipMs; }

uint32_t fmSendRemainingMs() {
  if (g_state != FM_IN_PENDING_SEND) return 0;
  const uint32_t e = millis() - g_pendingAt;
  return e >= FM_SEND_CONFIRM_MS ? 0 : (FM_SEND_CONFIRM_MS - e);
}

bool fmSendHoldProgress(uint8_t *pct) {
  if (g_state != FM_IN_PENDING_SEND || !g_btn[FM_BTN_SELECT].stable) return false;
  const uint32_t held = millis() - g_btn[FM_BTN_SELECT].pressedAt;
  if (pct) {
    *pct = held >= FM_SEND_HOLD_MS ? 100 : (uint8_t)((held * 100) / FM_SEND_HOLD_MS);
  }
  return true;
}

bool fmButtonHeld(FmButton b) { return b < FM_BTN_COUNT && g_btn[b].stable; }

uint16_t fmButtonPresses(FmButton b) { return b < FM_BTN_COUNT ? g_btn[b].presses : 0; }

FmAlarm fmLastAlarm() { return g_lastAlarm; }

const char *fmAlarmName(FmAlarm a) {
  switch (a) {
    case FM_ALARM_SAFE:     return "SAFE";
    case FM_ALARM_MEDICAL:  return "NEED MEDICAL";
    case FM_ALARM_WATER:    return "WATER GND FLOOR";
    case FM_ALARM_EVACUATE: return "NEED EVACUATION";
    default:                return "-";
  }
}

const char *fmAlarmShort(FmAlarm a) {
  switch (a) {
    case FM_ALARM_SAFE:     return "SAFE";
    case FM_ALARM_MEDICAL:  return "MED";
    case FM_ALARM_WATER:    return "WATR";
    case FM_ALARM_EVACUATE: return "EVAC";
    default:                return "-";
  }
}
