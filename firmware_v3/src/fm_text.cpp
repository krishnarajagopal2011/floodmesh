/**
 * FloodMesh V3 - 6-bit text packing. See fm_text.h.
 */
#include "fm_text.h"

//                               0         1         2         3         4         5         6
//                               0123456789012345678901234567890123456789012345678901234567890123
const char kFmTextAlphabet[65] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,?!-'/:;()+@#*=&%\"_<>$~[]";

static_assert(sizeof(kFmTextAlphabet) == 65, "the text alphabet must have exactly 64 symbols");

namespace {
int8_t codeOf(char c) {
  for (uint8_t i = 0; i < 64; i++) {
    if (kFmTextAlphabet[i] == c) return (int8_t)i;
  }
  return -1;
}
}  // namespace

char fmTextNormalize(char c) {
  if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
  return codeOf(c) >= 0 ? c : '?';
}

size_t fmTextPack(const char *in, uint8_t *out, uint8_t *chars) {
  uint8_t n = 0;
  uint32_t acc = 0;
  uint8_t bits = 0;
  size_t w = 0;
  while (in && in[n] && n < FM_TEXT_MAX_CHARS) {
    const int8_t code = codeOf(fmTextNormalize(in[n]));
    acc = (acc << 6) | (uint32_t)(code < 0 ? codeOf('?') : code);
    bits = (uint8_t)(bits + 6);
    while (bits >= 8) {
      bits = (uint8_t)(bits - 8);
      out[w++] = (uint8_t)(acc >> bits);
    }
    n++;
  }
  if (bits > 0) out[w++] = (uint8_t)(acc << (8 - bits));
  if (chars) *chars = n;
  return w;
}

void fmTextUnpack(const uint8_t *in, uint8_t chars, char *out) {
  uint32_t acc = 0;
  uint8_t bits = 0;
  size_t r = 0;
  for (uint8_t i = 0; i < chars; i++) {
    while (bits < 6) {
      acc = (acc << 8) | in[r++];
      bits = (uint8_t)(bits + 8);
    }
    bits = (uint8_t)(bits - 6);
    out[i] = kFmTextAlphabet[(acc >> bits) & 0x3F];
  }
  out[chars] = '\0';
}
