/**
 * FloodMesh - unified 5-button input state machine with PTT as a modifier key.
 *
 * The side PTT button does double duty:
 *   PTT up   -> the four face buttons are pager navigation (Mode A).
 *   PTT down -> the four face buttons are Layer 1 life-safety chords (Mode B),
 *               and PTT alone is a voice recording trigger.
 *
 * State machine
 * -------------
 *                 PTT down (mic opens, capture starts immediately)
 *   STANDBY  ----------------------------------------------> RECORDING
 *      ^                                                      |   |   |
 *      |                                        10.0s cap ----+   |   |
 *      |                                              v           |   |
 *      |                                           CAPPED         |   |
 *      |                            (mic off, clip held,          |   |
 *      |                             chords still armed)          |   |
 *      |                                              |           |   |
 *      |                     PTT up                   |  PTT up   |   |
 *      |                                              v           v   |
 *      |                                        PENDING_SEND <--------+
 *      |                                          |    |    |
 *      |   send (countdown expires, or hold [>])  |    |    |  cancel ([<])
 *      +------------------------------------------+    |    +----------+
 *      |                                               |               |
 *      |         PTT down again = redo the clip -------+               |
 *      |                                                               |
 *      |          face button chord: clip dropped, alarm sent          |
 *      +------------------------- ALARM_LATCHED <----------------------+
 *                                  (PTT up sends nothing)
 *
 * Why a confirm window exists at all
 * ----------------------------------
 * One 10-second note is 8 packets, ~2.6 s of transmit time, and every node that
 * repeats it spends the same again - roughly 8-10 s of shared channel for a
 * single press. An unwanted note is not free, so there is a way to stop it.
 *
 * There is deliberately NO automatic filtering (no minimum length, no "was
 * anyone speaking" check). A quiet or very short message - someone whispering
 * a floor number, or a one-word shout - is exactly the traffic that matters
 * most, and a machine gate would throw it away. Only a human cancels a clip.
 *
 * Cancelling cannot happen while PTT is still held, because every face button
 * is an alarm chord in that mode. So the decision window opens on release.
 *
 * Two ordering problems this solves that a naive "if (pttHeld) chord" does not:
 *
 *  1. Reverse chord order. Under stress people press the face button first and
 *     PTT second. A face button press is therefore NOT acted on at press time -
 *     the navigation action fires on RELEASE, and only if PTT stayed up for the
 *     whole press. If PTT arrives while a face button is already down, that is
 *     retroactively treated as a chord (see kChordJoinMs for the time limit, so
 *     that a long-held button followed much later by PTT is not a false alarm).
 *
 *  2. Release-after-chord must not transmit. Once a chord fires, the recording
 *     is discarded and the state latches, so letting go of PTT sends nothing.
 */
#pragma once
#include <Arduino.h>

#include "floodmesh_pins.h"

// ---------------------------------------------------------------- tunables
#ifndef FM_DEBOUNCE_MS
#define FM_DEBOUNCE_MS 25          // mechanical settle for 12x12 tactiles
#endif
#ifndef FM_VOICE_MAX_MS
#define FM_VOICE_MAX_MS 10000      // hard cap on a voice note (Layer 2 airtime)
#endif
#ifndef FM_CHORD_JOIN_MS
#define FM_CHORD_JOIN_MS 1000      // a face button held longer than this when PTT
#endif                             // arrives is deliberate navigation, not a chord
#ifndef FM_ALARM_MIN_GAP_MS
#define FM_ALARM_MIN_GAP_MS 0      // 0 = rate-limit-free, per the Layer 1 contract
#endif
#ifndef FM_SEND_CONFIRM_MS
#define FM_SEND_CONFIRM_MS 3000    // countdown before the clip goes out by itself
#endif
#ifndef FM_SEND_HOLD_MS
#define FM_SEND_HOLD_MS 600        // how long [>] must be held to send immediately
#endif
#ifndef FM_SEND_ON_TIMEOUT
#define FM_SEND_ON_TIMEOUT 1       // 1 = doing nothing sends it (a cancel window).
#endif                             // 0 = doing nothing bins it (a confirm window):
                                   // an unattended pocket press then never
                                   // transmits, at the cost of a real message
                                   // being lost if the operator walks away.

// ---------------------------------------------------------------- types
// 2x2 front keypad, indexed in logical order. The physical layout is
//     L1 [top-left]      R1 [top-right]
//     L2 [bottom-left]   R2 [bottom-right]
// Right column scrolls, left column acts. Under PTT every key is an alarm.
enum FmButton : uint8_t {
  FM_BTN_UP = 0,   // R1 top-right    - channel up    / SAFE
  FM_BTN_DOWN,     // R2 bottom-right - channel down  / WATER GROUND FLOOR
  FM_BTN_PREV,     // L2 bottom-left  - inbox prev    / NEED MEDICAL   ... CANCEL
  FM_BTN_SELECT,   // L1 top-left     - play / select / NEED EVACUATION ... SEND
  FM_BTN_PTT,      // side switch     - modifier + record
  FM_BTN_COUNT
};

enum FmAlarm : uint8_t {
  FM_ALARM_SAFE = 0,
  FM_ALARM_MEDICAL,
  FM_ALARM_WATER,
  FM_ALARM_EVACUATE,
  FM_ALARM_COUNT,
  FM_ALARM_NONE = 0xFF
};

enum FmNav : uint8_t {
  FM_NAV_NONE = 0,
  FM_NAV_CH_UP,
  FM_NAV_CH_DOWN,
  FM_NAV_INBOX_PREV,
  FM_NAV_PLAY
};

enum FmInputState : uint8_t {
  FM_IN_STANDBY = 0,    // Mode A: face buttons navigate
  FM_IN_RECORDING,      // Mode B: PTT held, mic capturing, chords armed
  FM_IN_CAPPED,         // 10 s reached, mic off, clip held, still holding PTT
  FM_IN_PENDING_SEND,   // PTT released, clip held, send/cancel decision open
  FM_IN_ALARM_LATCHED   // a chord fired; PTT release will not transmit voice
};

/** Why the held clip was thrown away. */
enum FmDropReason : uint8_t {
  FM_DROP_NONE = 0,
  FM_DROP_USER_CANCEL,  // operator pressed [<] in the confirm window
  FM_DROP_ALARM_CHORD,  // a Layer 1 chord preempted the recording
  FM_DROP_RERECORD      // operator pressed PTT again to redo the clip
};

/** One poll's worth of edges. All fields are false/NONE when nothing happened. */
struct FmInputEvent {
  FmNav   nav;          // navigation action (Mode A only)

  bool    recStart;     // open I2S, begin filling the capture buffer
  bool    recStop;      // stop capture, KEEP the buffer; recMs is valid
  bool    recCapped;    // 10.0 s hit (pairs with recStop) - chirp here
  bool    recDrop;      // throw the held or in-flight buffer away; see dropReason

  bool    sendPending;  // confirm window opened
  bool    sendCommit;   // transmit the held clip now (Codec2 + 8 fragments)

  bool    alarmFired;   // transmit a Layer 1 PSK-authenticated status packet
  FmAlarm alarm;        // which one, when alarmFired

  FmDropReason dropReason;
  uint32_t recMs;       // captured duration; valid with recStop / recDrop /
                        // sendCommit. Carried in the event because the state
                        // machine has already moved on by the time you read it.
};

// ---------------------------------------------------------------- API
void         fmInputBegin();
FmInputEvent fmInputPoll(uint32_t now);

FmInputState fmInputState();
uint32_t     fmRecordElapsedMs();     // live capture length, clamped to the cap
uint32_t     fmHeldClipMs();          // length of the clip awaiting a decision
uint32_t     fmSendRemainingMs();     // countdown left in the confirm window
bool         fmSendHoldProgress(uint8_t *pct);  // true while [>] is being held

bool         fmButtonHeld(FmButton b);
uint16_t     fmButtonPresses(FmButton b);
FmAlarm      fmLastAlarm();           // FM_ALARM_NONE until one fires

const char  *fmAlarmName(FmAlarm a);  // "NEED MEDICAL"
const char  *fmAlarmShort(FmAlarm a); // "MED" - for the 128px chord legend
