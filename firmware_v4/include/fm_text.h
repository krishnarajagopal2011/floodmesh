/**
 * FloodMesh V3 - 6-bit text alphabet for over-the-air messages.
 *
 * 64 symbols: space, A-Z, 0-9 and 27 punctuation marks. Lower case is folded
 * to upper case; anything else becomes '?'. 60 characters pack into 45 bytes,
 * which keeps a full message at ~0.45 s on air at SF9 (~0.14 s at SF7).
 *
 * Tanglish is typed letter by letter in the same alphabet. Tamil script is out
 * of scope by decision (docs/architecture.md section 7.3).
 */
#pragma once
#include <Arduino.h>

#include "fm_packet.h"

/** The 64-symbol alphabet, index = 6-bit code. */
extern const char kFmTextAlphabet[65];

/** Upper-case c if needed; '?' if it is not in the alphabet. */
char   fmTextNormalize(char c);

/**
 * Pack up to FM_TEXT_MAX_CHARS characters of `in` (NUL-terminated) into `out`
 * (at least FM_TEXT_PACKED_MAX bytes). Returns the number of characters packed
 * via *chars and the number of bytes written.
 */
size_t fmTextPack(const char *in, uint8_t *out, uint8_t *chars);

/** Unpack `chars` characters from `in` into `out` (chars + 1 bytes, NUL added). */
void   fmTextUnpack(const uint8_t *in, uint8_t chars, char *out);
