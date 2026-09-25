/**
 * FloodMesh V3 - 4x4 keypad wired straight to Heltec GPIOs, with debounce and
 * press / long-press / release events.
 *
 * Scanning: one row at a time is driven LOW while the other rows are left as
 * inputs (high-Z), then the four pulled-up columns are read. Driving idle rows
 * HIGH would short a HIGH output to a LOW one through two pressed keys that
 * share a column. Between scans every row is high-Z.
 *
 * Key indices are row-major:
 *        col1 col2 col3 col4
 *   row1   1    2    3    A        0  1  2  3
 *   row2   4    5    6    B        4  5  6  7
 *   row3   7    8    9    C        8  9 10 11
 *   row4   *    0    #    D       12 13 14 15
 */
#pragma once
#include <Arduino.h>

#define FM_KP_KEYS 16

#define FM_KP_1     0
#define FM_KP_2     1
#define FM_KP_3     2
#define FM_KP_A     3    // UP
#define FM_KP_4     4
#define FM_KP_5     5
#define FM_KP_6     6
#define FM_KP_B     7    // DOWN
#define FM_KP_7     8
#define FM_KP_8     9
#define FM_KP_9    10
#define FM_KP_C    11    // OK
#define FM_KP_STAR 12
#define FM_KP_0    13
#define FM_KP_HASH 14
#define FM_KP_D    15    // BACK

#define FM_KP_BIT(i) ((uint16_t)1u << (i))

#ifndef FM_KP_DEBOUNCE_MS
#define FM_KP_DEBOUNCE_MS 25
#endif
#ifndef FM_KP_LONG_MS
#define FM_KP_LONG_MS 700      // hold this long for a long-press event
#endif

enum FmKeyEvtType : uint8_t {
  FM_KEV_NONE = 0,
  FM_KEV_PRESS,        // debounced press edge
  FM_KEV_LONG,         // still held after FM_KP_LONG_MS (fires once per hold)
  FM_KEV_RELEASE,      // debounced release; `wasLong` says if LONG already fired
};

struct FmKeyEvent {
  FmKeyEvtType type;
  uint8_t      key;      // FM_KP_*
  bool         wasLong;  // on RELEASE: a LONG event was already delivered
};

void     fmKeypadBegin();
/** Poll the matrix; returns at most one event per call (call every loop). */
FmKeyEvent fmKeypadPoll(uint32_t nowMs);
/** Debounced state of all keys. */
uint16_t fmKeypadMask();
bool     fmKeypadHeld(uint8_t key);
/** Raw scan, no debounce (boot-time combos). */
uint16_t fmKeypadScanRaw();
/** Milliseconds the key has been held (0 if not held). */
uint32_t fmKeypadHeldMs(uint8_t key, uint32_t nowMs);
char     fmKeypadChar(uint8_t key);
void     fmKeypadMaskToString(uint16_t mask, char *out, size_t n);
