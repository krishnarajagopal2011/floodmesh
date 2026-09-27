/**
 * FloodMesh V3 - on-device text editor: T9 prediction, ABC multi-tap, 123.
 *
 * Keys (keypad indices from fm_keypad.h):
 *   2-9   letters (T9: one press per letter; ABC: press repeatedly to cycle)
 *   0     space (T9 also accepts the current word first)
 *   1     punctuation . , ? ! - ' / (press repeatedly to cycle), '1' in 123
 *   *     T9: next suggestion for the current word
 *   #     cycle mode T9 -> ABC -> 123
 *
 * The caller handles OK / BACK / UP / DOWN and maps them to fmEditAccept(),
 * fmEditBackspace() and fmEditCycleCandidate().
 *
 * Text is upper case only (the on-air alphabet is 6-bit, see fm_text.h).
 * Tanglish is typed in ABC mode; the dictionary is English only.
 */
#pragma once
#include <Arduino.h>

#include "fm_packet.h"

enum FmEditMode : uint8_t { FM_EDIT_T9 = 0, FM_EDIT_ABC, FM_EDIT_123 };

#ifndef FM_EDIT_MULTITAP_MS
#define FM_EDIT_MULTITAP_MS 900   // same key within this long cycles letters
#endif

void        fmEditClear();
/** Feed a keypad key (FM_KP_*). Returns true if the key was used. */
bool        fmEditKey(uint8_t key, uint32_t nowMs);
/** Commit a pending multi-tap letter after its timeout. Call every loop. */
void        fmEditTick(uint32_t nowMs);
/** Commit the pending T9 word / multi-tap letter (no space). */
void        fmEditAccept();
/** Delete: last T9 key, pending letter, or last committed character. */
void        fmEditBackspace();
/** T9: step through suggestions (+1 / -1). */
void        fmEditCycleCandidate(int dir);
void        fmEditCycleMode();
FmEditMode  fmEditMode();
const char *fmEditModeName();
/** Committed text plus whatever is pending, for display. `pendStart` receives
 *  the index where the pending part begins (== strlen if none). */
void        fmEditRender(char *out, size_t n, size_t *pendStart);
/** Commit everything and copy the final text (<= FM_TEXT_MAX_CHARS). */
void        fmEditFinish(char *out, size_t n);
size_t      fmEditLength();          // including pending
bool        fmEditIsEmpty();
/** T9 candidate position, e.g. 1 of 3 (0 of 0 when no word pending). */
void        fmEditCandidateInfo(uint8_t *index, uint8_t *count);
