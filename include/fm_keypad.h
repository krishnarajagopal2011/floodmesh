/**
 * FloodMesh - 3x4 keypad behind an MCP23017 I2C expander (proto v2 board).
 *
 * Wiring (docs/prototype-v2-build.md section 3):
 *   GPA0..GPA2  columns 1..3   inputs, internal pull-ups, interrupt on change
 *   GPA3..GPA6  rows 1..4      held LOW while idle
 *   GPA7        unused         recent datasheets make GPA7/GPB7 output-only
 *   INTA        -> PIN_KP_INT  open-drain, external 10 k pull-up
 *
 * Idle state: every row is an output driven LOW. Any key press pulls its
 * column low, which fires INTA - the hook for waking from deep sleep.
 *
 * Scanning: one row at a time is driven LOW while the other rows are switched
 * to INPUT (high-Z), then the columns are read. Driving the other rows HIGH
 * instead would short a HIGH output to a LOW one through any two pressed keys
 * that share a column. After the scan every row goes back to LOW.
 *
 * Key indices are row-major on the physical pad:
 *
 *        col1 col2 col3
 *   row1   1    2    3        index  0  1  2
 *   row2   4    5    6               3  4  5
 *   row3   7    8    9               6  7  8
 *   row4   *    0    #               9 10 11
 *
 * If a keypad's cable order differs, fix it with FM_KP_ROW_ORDER /
 * FM_KP_COL_ORDER rather than by re-soldering.
 */
#pragma once
#include <Arduino.h>

#include "floodmesh_pins.h"

#define FM_KP_KEYS 12

// Index of each key in the 12-bit mask.
#define FM_KP_1     0
#define FM_KP_2     1
#define FM_KP_3     2
#define FM_KP_4     3
#define FM_KP_5     4
#define FM_KP_6     5
#define FM_KP_7     6
#define FM_KP_8     7
#define FM_KP_9     8
#define FM_KP_STAR  9
#define FM_KP_0    10
#define FM_KP_HASH 11

#define FM_KP_BIT(i) ((uint16_t)1u << (i))

#ifndef FM_KP_SCAN_MS
#define FM_KP_SCAN_MS 8     // minimum gap between I2C scans; debounce is in fm_input
#endif

/**
 * Set up the second I2C bus and the expander. Returns false if the MCP23017
 * does not answer at FM_KP_I2C_ADDR; every other call then reports no keys.
 */
bool     fmKeypadBegin();
bool     fmKeypadPresent();

/** Scan now (ignores FM_KP_SCAN_MS) and return the raw 12-bit pressed mask. */
uint16_t fmKeypadScanNow();

/** Rate-limited scan: returns the cached mask if scanned less than FM_KP_SCAN_MS ago. */
uint16_t fmKeypadScan(uint32_t nowMs);

/** Last scanned mask, without touching the bus. */
uint16_t fmKeypadMask();

bool     fmKeypadHeld(uint8_t idx);

/** '1'..'9', '*', '0', '#' for an index; '?' if out of range. */
char     fmKeypadChar(uint8_t idx);

/** Write the pressed keys as characters into out (NUL-terminated). */
void     fmKeypadMaskToString(uint16_t mask, char *out, size_t n);

/** Rows LOW, interrupt armed, INTA cleared: call before deep sleep. */
void     fmKeypadArmWake();
