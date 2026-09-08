/**
 * FloodMesh - Layer 1 alarm authentication implementation.
 * See fm_auth.h for why the authenticated region skips byte 2, why the MAC is
 * HMAC-SHA256 rather than AES-CMAC, and what the caller must do in which order.
 *
 * All time arithmetic uses unsigned subtraction (now - then), which is correct
 * across the 49.7-day millis() rollover. Never compare timestamps directly.
 */
#include "fm_auth.h"

#include <stdio.h>
#include <string.h>

#include "mbedtls/md.h"

namespace {

/**
 * The authenticated byte list, in order. This array is the single source of
 * truth for what is covered; both sign and verify go through authMac() below,
 * so they cannot drift apart. Byte 1 (hopLimit) is absent because relays
 * rewrite it.
 */
const uint8_t kAuthBytes[FM_AUTH_REGION_LEN] = {
    0,                      // ver + type nibble
    2,  3,  4,  5,  6,      // callSign
    7,                      // channel
    8,                      // msgId
    9,                      // alarmType
    10, 11, 12, 13          // counter
};                          // byte 1 (hopLimit) is deliberately absent

static_assert(FM_AUTH_REGION_LEN == FM_OFF_MAC - 1,
              "the authenticated region is everything before the MAC except hopLimit");
static_assert(sizeof(kAuthBytes) == FM_AUTH_REGION_LEN,
              "kAuthBytes must list exactly FM_AUTH_REGION_LEN frame offsets");
static_assert(FM_REPLAY_SLOTS >= 1 && FM_REPLAY_SLOTS <= 255,
              "FM_REPLAY_SLOTS is indexed by an int16_t sentinel search");

/** One tracked neighbour. callSign[0] == '\0' marks the slot as free. */
struct ReplaySlot {
  char     callSign[FM_CALLSIGN_LEN + 1];  // NUL-terminated, unlike the wire
  uint32_t counter;      // highest counter accepted from this node so far
  uint32_t lastHeardMs;  // millis() of the last ACCEPTED alarm; drives eviction
};

ReplaySlot           g_slot[FM_REPLAY_SLOTS];
uint8_t              g_psk[FM_PSK_LEN];
mbedtls_md_context_t g_md;              // set up once with hmac=1, then reused
bool                 g_ready = false;   // false => fail closed, never fail open

/**
 * Gather the non-contiguous authenticated region and HMAC it. Returns false if
 * the key was never installed or mbedTLS refused, in which case *out is
 * untouched and the caller must fail closed.
 *
 * mbedtls_md_setup() is the only allocation in this module and it happens in
 * fmAuthBegin(); mbedtls_md_hmac_starts() re-derives the ipad/opad in the
 * context that is already there, so the packet path never touches the heap.
 */
bool authMac(const uint8_t *frame, uint8_t out[FM_MAC_LEN]) {
  if (!g_ready) return false;

  uint8_t scratch[FM_AUTH_REGION_LEN];
  for (uint8_t i = 0; i < FM_AUTH_REGION_LEN; i++) {
    scratch[i] = frame[kAuthBytes[i]];
  }

  // MBEDTLS_MD_MAX_SIZE rather than 32: mbedtls_md_hmac_finish() writes the
  // digest size of whatever md_info the context holds, and a config change
  // that swapped SHA-256 for something longer must not become a stack smash.
  uint8_t full[MBEDTLS_MD_MAX_SIZE];
  if (mbedtls_md_hmac_starts(&g_md, g_psk, FM_PSK_LEN) != 0 ||
      mbedtls_md_hmac_update(&g_md, scratch, sizeof(scratch)) != 0 ||
      mbedtls_md_hmac_finish(&g_md, full) != 0) {
    return false;
  }

  memcpy(out, full, FM_MAC_LEN);   // truncate: leftmost 8 bytes, per RFC 2104
  return true;
}

/** Index of callSign in the table, or -1. Compares the significant 5 bytes. */
int16_t findSlot(const char *callSign) {
  for (uint8_t i = 0; i < FM_REPLAY_SLOTS; i++) {
    if (g_slot[i].callSign[0] != '\0' &&
        strncmp(g_slot[i].callSign, callSign, FM_CALLSIGN_LEN) == 0) {
      return (int16_t)i;
    }
  }
  return -1;
}

/**
 * A free slot, or the least-recently-heard one. Always returns a usable index:
 * a full table must still evict rather than start accepting everything, and the
 * cost of that choice is documented in fm_auth.h.
 */
uint8_t claimSlot(uint32_t now) {
  uint8_t  victim = 0;
  uint32_t oldest = 0;
  for (uint8_t i = 0; i < FM_REPLAY_SLOTS; i++) {
    if (g_slot[i].callSign[0] == '\0') return i;
    const uint32_t age = now - g_slot[i].lastHeardMs;  // rollover-safe
    if (age >= oldest) {
      oldest = age;
      victim = i;
    }
  }
  Serial.printf("[WARN] replay table full (%u slots) - evicting %s; its old "
                "counters become replayable.\n",
                (unsigned)FM_REPLAY_SLOTS, g_slot[victim].callSign);
  return victim;
}

}  // namespace

// ---------------------------------------------------------------- API
void fmAuthBegin(const uint8_t psk[16]) {
  if (g_ready) {
    mbedtls_md_free(&g_md);   // re-key: tear the old context down first
    g_ready = false;
  }
  memset(g_psk, 0, sizeof(g_psk));

  if (psk == NULL) {
    Serial.println("[ERR] fmAuthBegin(NULL) - alarms will neither sign nor verify.");
    return;
  }

  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_init(&g_md);
  if (info == NULL || mbedtls_md_setup(&g_md, info, 1 /* HMAC */) != 0) {
    // Only reachable if mbedTLS was built without SHA-256 or the heap is gone
    // at setup() time. Either way there is no safe fallback: stay closed.
    mbedtls_md_free(&g_md);
    Serial.println("[ERR] HMAC-SHA256 unavailable - alarm authentication is OFF.");
    return;
  }

  memcpy(g_psk, psk, FM_PSK_LEN);
  g_ready = true;
}

void fmAuthSignAlarm(uint8_t *frame24) {
  if (frame24 == NULL) return;

  uint8_t mac[FM_MAC_LEN];
  if (!authMac(frame24, mac)) {
    // Zero rather than leave whatever was in the buffer: a stale tag from a
    // previous frame could be a valid one, and shipping it would be worse than
    // shipping something every receiver rejects.
    memset(frame24 + FM_OFF_MAC, 0, FM_MAC_LEN);
    Serial.println("[WARN] fmAuthSignAlarm before fmAuthBegin - MAC zeroed.");
    return;
  }
  memcpy(frame24 + FM_OFF_MAC, mac, FM_MAC_LEN);
}

bool fmAuthVerifyAlarm(const uint8_t *frame24) {
  if (frame24 == NULL) return false;

  uint8_t mac[FM_MAC_LEN];
  if (!authMac(frame24, mac)) return false;

  // Constant time on purpose - no memcmp, no early exit. memcmp stops at the
  // first differing byte, and an attacker who can measure how long a rejection
  // takes can then guess the tag one byte at a time (256 x 8 tries instead of
  // 2^64). Fold every byte in, decide once at the end.
  uint8_t diff = 0;
  for (uint8_t i = 0; i < FM_MAC_LEN; i++) {
    diff |= (uint8_t)(mac[i] ^ frame24[FM_OFF_MAC + i]);
  }
  return diff == 0;
}

bool fmReplayAccept(const char *callSign, uint32_t counter) {
  // An empty or missing call sign cannot be tracked, and a slot keyed on '\0'
  // would read as free forever, so refuse rather than corrupt the table.
  if (callSign == NULL || callSign[0] == '\0') return false;

  const uint32_t now = millis();

  const int16_t found = findSlot(callSign);
  if (found >= 0) {
    ReplaySlot &s = g_slot[found];
    if (counter <= s.counter) {
      return false;   // replay, or a node that restarted its counter - see the
                      // reboot hazard in fm_auth.h
    }
    s.counter = counter;
    s.lastHeardMs = now;
    return true;
  }

  // Unknown neighbour: admit it. Requiring a handshake before believing a first
  // EVACUATE would be the wrong trade in the only situation that matters.
  ReplaySlot &s = g_slot[claimSlot(now)];
  snprintf(s.callSign, sizeof(s.callSign), "%.*s", (int)FM_CALLSIGN_LEN, callSign);
  s.counter = counter;
  s.lastHeardMs = now;
  return true;
}

void fmReplayReset() {
  memset(g_slot, 0, sizeof(g_slot));
}
