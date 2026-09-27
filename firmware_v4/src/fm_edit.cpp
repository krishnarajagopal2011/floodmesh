/**
 * FloodMesh V3 - text editor. See fm_edit.h.
 */
#include "fm_edit.h"

#include "fm_keypad.h"
#include "fm_t9_dict.h"

namespace {

const char *kLetters[10] = {
    " 0",        // 0
    ".,?!-'/1",  // 1
    "ABC2", "DEF3", "GHI4", "JKL5", "MNO6", "PQRS7", "TUV8", "WXYZ9",
};

const uint8_t kMaxSeq = 14;
const uint8_t kMaxCands = 16;

char       g_text[FM_TEXT_MAX_CHARS + 1];
size_t     g_len = 0;
FmEditMode g_mode = FM_EDIT_T9;

// T9 state
char       g_seq[kMaxSeq + 1];       // digits typed for the current word
uint8_t    g_seqLen = 0;
uint16_t   g_cand[kMaxCands];        // offsets into kFmT9Words
uint8_t    g_candCount = 0;
uint8_t    g_candIdx = 0;

// multi-tap state (ABC letters and the '1' punctuation key in every mode)
int8_t     g_tapKey = -1;            // digit 0-9 being cycled, -1 = none
uint8_t    g_tapCount = 0;
uint32_t   g_tapAt = 0;

int8_t digitOfKey(uint8_t key) {
  switch (key) {
    case FM_KP_0: return 0;
    case FM_KP_1: return 1;
    case FM_KP_2: return 2;
    case FM_KP_3: return 3;
    case FM_KP_4: return 4;
    case FM_KP_5: return 5;
    case FM_KP_6: return 6;
    case FM_KP_7: return 7;
    case FM_KP_8: return 8;
    case FM_KP_9: return 9;
    default:      return -1;
  }
}

char digitOfLetter(char c) {
  for (uint8_t d = 2; d <= 9; d++) {
    for (const char *p = kLetters[d]; *p; p++) {
      if (*p == c && !(c >= '0' && c <= '9')) return (char)('0' + d);
    }
  }
  return 0;
}

size_t pendingLen() {
  if (g_seqLen) return g_seqLen;
  if (g_tapKey >= 0) return 1;
  return 0;
}

void appendChar(char c) {
  if (g_len < FM_TEXT_MAX_CHARS) {
    g_text[g_len++] = c;
    g_text[g_len] = '\0';
  }
}

/** Find dictionary words whose key sequence equals g_seq, in frequency order. */
void lookup() {
  g_candCount = 0;
  g_candIdx = 0;
  if (!g_seqLen) return;
  const char *p = kFmT9Words;
  for (uint16_t n = 0; n < FM_T9_WORD_COUNT && g_candCount < kMaxCands; n++) {
    const size_t wl = strlen(p);
    if (wl == g_seqLen) {
      bool ok = true;
      for (uint8_t i = 0; i < g_seqLen && ok; i++) ok = digitOfLetter(p[i]) == g_seq[i];
      if (ok) g_cand[g_candCount++] = (uint16_t)(p - kFmT9Words);
    }
    p += wl + 1;
  }
}

/** The word currently shown for g_seq (a suggestion, or first letters). */
void currentWord(char *out) {
  if (g_candCount) {
    memcpy(out, kFmT9Words + g_cand[g_candIdx], g_seqLen);
  } else {
    // No dictionary match: show the first letter of each key, like old phones
    // did, so the user sees something and can switch to ABC.
    for (uint8_t i = 0; i < g_seqLen; i++) out[i] = kLetters[g_seq[i] - '0'][0];
  }
  out[g_seqLen] = '\0';
}

void commitSeq() {
  if (!g_seqLen) return;
  char w[kMaxSeq + 1];
  currentWord(w);
  for (uint8_t i = 0; w[i]; i++) appendChar(w[i]);
  g_seqLen = 0;
  g_candCount = 0;
  g_candIdx = 0;
}

void commitTap() {
  if (g_tapKey < 0) return;
  const char *set = kLetters[g_tapKey];
  appendChar(set[g_tapCount % strlen(set)]);
  g_tapKey = -1;
  g_tapCount = 0;
}

void commitAll() {
  commitSeq();
  commitTap();
}

}  // namespace

void fmEditClear() {
  g_text[0] = '\0';
  g_len = 0;
  g_seqLen = 0;
  g_candCount = 0;
  g_candIdx = 0;
  g_tapKey = -1;
  g_tapCount = 0;
}

bool fmEditKey(uint8_t key, uint32_t now) {
  if (key == FM_KP_HASH) {
    fmEditCycleMode();
    return true;
  }
  if (key == FM_KP_STAR) {
    if (g_mode == FM_EDIT_T9) fmEditCycleCandidate(+1);
    return true;
  }
  const int8_t d = digitOfKey(key);
  if (d < 0) return false;

  // 1 is multi-tap punctuation in T9 and ABC; a digit in 123.
  const bool tapKey = (g_mode == FM_EDIT_ABC) || (g_mode == FM_EDIT_T9 && d == 1);

  if (g_mode == FM_EDIT_123) {
    commitAll();
    if (g_len + pendingLen() < FM_TEXT_MAX_CHARS) appendChar((char)('0' + d));
    return true;
  }

  if (tapKey && !(g_mode == FM_EDIT_ABC && d == 0)) {
    if (g_tapKey == d && (now - g_tapAt) < FM_EDIT_MULTITAP_MS) {
      g_tapCount++;
    } else {
      commitAll();
      if (g_len >= FM_TEXT_MAX_CHARS) return true;
      g_tapKey = d;
      g_tapCount = 0;
    }
    g_tapAt = now;
    return true;
  }

  if (d == 0) {                     // space: accept the pending word first
    commitAll();
    if (g_len > 0 && g_text[g_len - 1] != ' ') appendChar(' ');
    return true;
  }

  // T9 letter key 2-9.
  commitTap();
  if (g_len + g_seqLen >= FM_TEXT_MAX_CHARS || g_seqLen >= kMaxSeq) return true;
  g_seq[g_seqLen++] = (char)('0' + d);
  g_seq[g_seqLen] = '\0';
  lookup();
  return true;
}

void fmEditTick(uint32_t now) {
  if (g_tapKey >= 0 && (now - g_tapAt) >= FM_EDIT_MULTITAP_MS) commitTap();
}

void fmEditAccept() { commitAll(); }

void fmEditBackspace() {
  if (g_seqLen) {
    g_seqLen--;
    g_seq[g_seqLen] = '\0';
    lookup();
    return;
  }
  if (g_tapKey >= 0) {
    g_tapKey = -1;
    g_tapCount = 0;
    return;
  }
  if (g_len) g_text[--g_len] = '\0';
}

void fmEditCycleCandidate(int dir) {
  if (!g_candCount) return;
  g_candIdx = (uint8_t)((g_candIdx + g_candCount + dir) % g_candCount);
}

void fmEditCycleMode() {
  commitAll();
  g_mode = (FmEditMode)((g_mode + 1) % 3);
}

FmEditMode fmEditMode() { return g_mode; }

const char *fmEditModeName() {
  switch (g_mode) {
    case FM_EDIT_T9:  return "T9";
    case FM_EDIT_ABC: return "ABC";
    default:          return "123";
  }
}

void fmEditRender(char *out, size_t n, size_t *pendStart) {
  if (!out || n == 0) return;
  snprintf(out, n, "%s", g_text);
  size_t w = strlen(out);
  if (pendStart) *pendStart = w;
  char p[kMaxSeq + 1] = "";
  if (g_seqLen) {
    currentWord(p);
  } else if (g_tapKey >= 0) {
    const char *set = kLetters[g_tapKey];
    p[0] = set[g_tapCount % strlen(set)];
    p[1] = '\0';
  }
  snprintf(out + w, n - w, "%s", p);
}

void fmEditFinish(char *out, size_t n) {
  commitAll();
  size_t e = g_len;
  while (e > 0 && g_text[e - 1] == ' ') e--;   // trailing spaces cost airtime
  g_text[e] = '\0';
  g_len = e;
  snprintf(out, n, "%s", g_text);
}

size_t fmEditLength() { return g_len + pendingLen(); }
bool   fmEditIsEmpty() { return fmEditLength() == 0; }

void fmEditCandidateInfo(uint8_t *index, uint8_t *count) {
  if (index) *index = g_candCount ? (uint8_t)(g_candIdx + 1) : 0;
  if (count) *count = g_candCount;
}
