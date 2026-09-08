/**
 * FloodMesh - node identity implementation.
 * See fm_ids.h for the base-36 arithmetic, the eFuse byte-order trap, and the
 * counter reservation scheme.
 *
 * Everything here is deliberately callable before fmIdsBegin(): a caller that
 * gets the ordering wrong sees a placeholder call sign and a counter that
 * starts at zero, not a null pointer or a crash on the alarm path.
 */
#include "fm_ids.h"

#include <Preferences.h>
#include <esp_random.h>

namespace {

// 36^5. The whole point of the module's header comment: this is ~2^25.85, so a
// 32-bit MAC does not fit and the derivation reduces rather than encodes.
constexpr uint32_t kIdSpace = 36u * 36u * 36u * 36u * 36u;
static_assert(kIdSpace == 60466176u, "base-36 name space arithmetic drifted");
static_assert(FM_CALLSIGN_LEN == 5, "fm_ids renders exactly five base-36 digits");
static_assert(FM_AUTH_BLOCK >= 1, "a reservation block must hand out something");

// Digit value 0..25 -> 'A'..'Z', 26..35 -> '0'..'9'. Letters first so the low
// digits of a small value read as letters, which look more like a call sign.
const char kB36[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
static_assert(sizeof(kB36) == 37, "base-36 alphabet must be 36 characters + NUL");

Preferences g_prefs;
bool        g_nvsOpen = false;

// Spaces rather than 'A's for the pre-begin() placeholder: space is a legal pad
// byte in the on-air alphabet, so a frame built too early still parses, and a
// blank row on the OLED is obviously unset instead of impersonating a station
// that could really exist.
char        g_callSign[FM_CALLSIGN_LEN + 1] = "     ";
bool        g_derived   = true;
uint16_t    g_msgId     = 0;
uint32_t    g_authNext  = 0;   // next counter to hand out
uint32_t    g_authLimit = 0;   // first counter NOT yet reserved in NVS

/** Fixed-width base-36. Leading zero digits render as 'A', which is the "left
 *  padding" - there is no separate pad step to get wrong. */
void renderB36(uint32_t v, char *out) {
  v %= kIdSpace;
  for (int8_t i = FM_CALLSIGN_LEN - 1; i >= 0; i--) {
    out[i] = kB36[v % 36u];
    v /= 36u;
  }
  out[FM_CALLSIGN_LEN] = '\0';
}

bool isCsChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/** Length of a valid operator call sign, or 0 if it is not one. */
uint8_t operatorCallSignLen(const char *cs) {
  if (cs == NULL) {
    return 0;
  }
  uint8_t n = 0;
  while (n < FM_CALLSIGN_LEN && cs[n] != '\0') {
    if (!isCsChar(cs[n])) {
      return 0;  // lowercase, punctuation, space - all refused, never repaired
    }
    n++;
  }
  // cs[n] is only reached after n accepted characters, so the terminator of a
  // full-length string is read and nothing beyond it.
  return (n > 0 && cs[n] == '\0') ? n : 0;
}

/**
 * Check a call sign that came back out of flash. NVS is not hostile the way the
 * radio is, but a half-written or downgraded record is still input we did not
 * produce this boot, so it gets the same treatment as bytes off the air.
 */
bool storedCallSignOk(const char *s, size_t len) {
  if (len != FM_CALLSIGN_LEN || s[0] == ' ') {
    return false;  // wrong width, or a name made of nothing but padding
  }
  bool padding = false;
  for (size_t i = 0; i < FM_CALLSIGN_LEN; i++) {
    if (s[i] == ' ') {
      padding = true;
      continue;
    }
    if (padding || !isCsChar(s[i])) {
      return false;  // a gap in the middle, or a byte outside the alphabet
    }
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------- API
void fmIdsBegin() {
  // Seeded before anything can fail, so a dead NVS still gets fresh message ids.
  g_msgId = (uint16_t)esp_random();

  g_nvsOpen = g_prefs.begin(FM_NVS_NAMESPACE, false);
  if (!g_nvsOpen) {
    Serial.printf("[WARN] NVS namespace \"%s\" would not open - the call sign "
                  "cannot be overridden and the replay counter restarts at 0 "
                  "on every boot.\n",
                  FM_NVS_NAMESPACE);
  }

  // One byte of slack past the padded form so an over-long record is READ and
  // rejected with a warning rather than silently failing to fit.
  char stored[FM_CALLSIGN_LEN + 2];
  memset(stored, 0, sizeof(stored));
  const size_t raw =
      g_nvsOpen ? g_prefs.getString(FM_NVS_KEY_CALLSIGN, stored, sizeof(stored)) : 0;

  // getString() returns the NVS record length, which counts the terminator.
  const size_t chars = (raw > 0) ? (raw - 1) : 0;
  if (chars > 0 && storedCallSignOk(stored, chars)) {
    // Explicit precision, not plain "%s": storedCallSignOk() has already proved
    // chars <= FM_CALLSIGN_LEN, but the compiler cannot see that and warns the
    // 7-byte source may not fit the 6-byte destination. Bounding it here keeps
    // the warning honest rather than suppressed.
    snprintf(g_callSign, sizeof(g_callSign), "%.*s", (int)FM_CALLSIGN_LEN, stored);
    g_derived = false;
  } else {
    if (raw > 0) {
      Serial.printf("[WARN] stored call sign is malformed - falling back to the "
                    "MAC-derived one.\n");
    }
    fmDeriveCallSign(g_callSign);
    g_derived = true;
  }

  // Read the high-water mark but reserve nothing yet: most boots raise no alarm
  // at all and should cost no flash write. The first fmNextAuthCounter() call
  // takes the commit.
  g_authNext  = g_nvsOpen ? g_prefs.getUInt(FM_NVS_KEY_AUTH_HW, 0) : 0;
  g_authLimit = g_authNext;

  Serial.printf("[ids] call sign %s (%s), replay counter resumes at %u\n",
                g_callSign, g_derived ? "derived" : "operator", g_authNext);
}

const char *fmCallSign() { return g_callSign; }

bool fmCallSignIsDerived() { return g_derived; }

void fmDeriveCallSign(char out[FM_CALLSIGN_LEN + 1]) {
  const uint64_t mac = ESP.getEfuseMac();

#if FM_ID_STRICT_LOW32
  // Literal low 32 bits. On this silicon that is 24 bits of Espressif OUI plus
  // one unique byte, so the entire fleet shares 256 possible call signs.
  const uint32_t v = (uint32_t)mac;
#else
  // Bring the device-unique bytes (mac[3..5], bits 24..47) down into the low
  // half where they survive the reduction, and fold the OUI bytes back in on
  // top rather than throwing them away. Then avalanche, so one flipped MAC bit
  // changes the whole call sign instead of only its last character - two boards
  // off the same reel should not read as near-identical strings on a 128 px
  // display.
  uint32_t v = (uint32_t)(mac >> 16) ^ ((uint32_t)mac << 16);
  v ^= v >> 16;
  v *= 0x85EBCA6Bu;
  v ^= v >> 13;
  v *= 0xC2B2AE35u;
  v ^= v >> 16;
#endif

  renderB36(v, out);
}

bool fmSetCallSign(const char *cs) {
  if (operatorCallSignLen(cs) == 0) {
    return false;
  }

  char padded[FM_CALLSIGN_LEN + 1];
  snprintf(padded, sizeof(padded), "%-*s", (int)FM_CALLSIGN_LEN, cs);

  // Persist first. If the write does not stick, the old call sign stays in
  // force, so "false" always means exactly one thing: nothing changed.
  if (!g_nvsOpen || g_prefs.putString(FM_NVS_KEY_CALLSIGN, padded) == 0) {
    Serial.printf("[WARN] could not persist call sign \"%s\" - still %s.\n",
                  padded, g_callSign);
    return false;
  }

  snprintf(g_callSign, sizeof(g_callSign), "%s", padded);
  g_derived = false;
  return true;
}

uint16_t fmNextMsgId() { return g_msgId++; }

uint32_t fmNextAuthCounter() {
  if (g_authNext >= g_authLimit) {
    const uint32_t room = UINT32_MAX - g_authNext;
    if (room == 0) {
      Serial.printf("[WARN] replay counter space exhausted - this alarm reuses "
                    "a counter receivers have already seen.\n");
      return UINT32_MAX;
    }

    // Reserve forward, never from zero: the mark that goes to flash is the
    // first counter this boot promises NOT to use.
    const uint32_t step =
        (room < (uint32_t)FM_AUTH_BLOCK) ? room : (uint32_t)FM_AUTH_BLOCK;
    const uint32_t limit = g_authNext + step;

    if (!g_nvsOpen || g_prefs.putUInt(FM_NVS_KEY_AUTH_HW, limit) == 0) {
      // Degraded, not blocked. The alarm goes out with a counter that is valid
      // for this boot; only a later reboot can hand the block out twice, and a
      // suppressed alarm now is worse than a rejected one after a power cycle.
      Serial.printf("[WARN] replay high-water mark %u not persisted - counters "
                    "may regress after a reboot.\n",
                    limit);
    }
    g_authLimit = limit;
  }
  return g_authNext++;
}
