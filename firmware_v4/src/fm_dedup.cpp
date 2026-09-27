/**
 * FloodMesh - packet deduplication ring implementation.
 * See fm_dedup.h for why the TTL exists and what a hash collision costs.
 *
 * All time arithmetic uses unsigned subtraction (now - then), which is correct
 * across the 49.7-day millis() rollover. Never compare timestamps directly.
 */
#include "fm_dedup.h"

#include <Arduino.h>

#include "fm_packet.h"

namespace {

struct Slot {
  uint32_t hash;      // FNV-1a of the packed key; see keyHash()
  uint32_t stampMs;   // millis() when the key was FIRST seen, never refreshed
  bool     used;      // false only before this slot has ever been written
};

Slot g_ring[FM_DEDUP_SLOTS];

// FNV-1a, 32-bit. Offset basis 2166136261 = 0x811C9DC5, prime 16777619 =
// 0x01000193. Chosen over a CRC because the key is 8 fixed bytes: FNV needs no
// table, avalanches well enough at that length, and costs 8 xor/multiply pairs.
const uint32_t kFnvOffsetBasis = 2166136261UL;  // 0x811C9DC5
const uint32_t kFnvPrime       = 16777619UL;    // 0x01000193

const size_t kKeyLen = FM_CALLSIGN_LEN + 3;     // 5 call sign + 2 msgId + 1 frag

uint32_t fnv1a(const uint8_t *p, size_t n) {
  uint32_t h = kFnvOffsetBasis;
  for (size_t i = 0; i < n; i++) {
    h ^= (uint32_t)p[i];
    h *= kFnvPrime;
  }
  return h;
}

/**
 * Pack the key the way it appears on air, then hash it.
 *
 * The call sign is space-padded, not NUL-padded, because the frame carries 5
 * raw bytes with no terminator: a node comparing its own "K7AB" against the
 * "K7AB " it hears echoed back must land on the same slot or it will relay its
 * own packet forever. msgId goes down through fmWr16 rather than a memcpy so
 * the key is little endian by construction: a port to a big-endian MCU picks
 * up fm_packet.h's conversion instead of silently hashing the bytes the other
 * way round and deduplicating against nothing.
 */
uint32_t keyHash(const char *callSign, uint16_t msgId, uint8_t fragIdx) {
  uint8_t key[kKeyLen];

  size_t i = 0;
  if (callSign != NULL) {
    while (i < FM_CALLSIGN_LEN && callSign[i] != '\0') {
      key[i] = (uint8_t)callSign[i];
      i++;
    }
  }
  while (i < FM_CALLSIGN_LEN) {
    key[i++] = (uint8_t)' ';
  }

  fmWr16(&key[FM_CALLSIGN_LEN], msgId);
  key[FM_CALLSIGN_LEN + 2] = fragIdx;
  return fnv1a(key, kKeyLen);
}

/** Index of a live entry carrying this hash, or -1. Expired entries never match. */
int16_t findLive(uint32_t hash, uint32_t now) {
  for (uint8_t i = 0; i < FM_DEDUP_SLOTS; i++) {
    if (!g_ring[i].used || g_ring[i].hash != hash) continue;
    if ((now - g_ring[i].stampMs) < FM_DEDUP_TTL_MS) return (int16_t)i;
  }
  return -1;
}

/**
 * Pick the slot an insert should take. Always succeeds - a full ring of live
 * entries evicts its oldest rather than refusing to record, because forgetting
 * one old key risks one duplicate relay while failing to record a new key
 * risks the loop this module exists to prevent.
 *
 * The age comparison that finds the oldest only ever runs on entries younger
 * than the TTL, so the values being ordered are bounded by FM_DEDUP_TTL_MS and
 * cannot alias each other across a millis() rollover.
 */
uint8_t pickVictim(uint32_t hash, uint32_t now) {
  int16_t  reusable  = -1;
  uint8_t  oldestIdx = 0;
  uint32_t oldestAge = 0;

  for (uint8_t i = 0; i < FM_DEDUP_SLOTS; i++) {
    const Slot &s = g_ring[i];
    if (!s.used) {
      if (reusable < 0) reusable = (int16_t)i;
      continue;
    }

    const uint32_t age = now - s.stampMs;
    if (age >= FM_DEDUP_TTL_MS) {
      // An expired twin of the key being inserted is the ideal victim. Taking
      // it stops a rebooted neighbour - which recycles its message ids, so it
      // regenerates the same keys - from parking a dead copy of every key it
      // sends in the ring for a further full TTL.
      if (s.hash == hash) return i;
      if (reusable < 0) reusable = (int16_t)i;
      continue;
    }

    if (age >= oldestAge) {
      oldestAge = age;
      oldestIdx = i;
    }
  }

  return reusable >= 0 ? (uint8_t)reusable : oldestIdx;
}

void reset() {
  for (uint8_t i = 0; i < FM_DEDUP_SLOTS; i++) {
    g_ring[i].hash = 0;
    g_ring[i].stampMs = 0;
    g_ring[i].used = false;
  }
}

}  // namespace

// ---------------------------------------------------------------- API
void fmDedupBegin() {
  // Identical to fmDedupClear() today. It exists separately so main.cpp reads
  // symmetrically with the other modules, and so one-time setup has a home
  // that is not on the "operator wiped the ring" path.
  reset();
}

bool fmDedupSeenOrMark(const char *callSign, uint16_t msgId, uint8_t fragIdx) {
  const uint32_t now = millis();
  const uint32_t h = keyHash(callSign, msgId, fragIdx);

  if (findLive(h, now) >= 0) return true;

  const uint8_t v = pickVictim(h, now);
  g_ring[v].hash = h;
  g_ring[v].stampMs = now;
  g_ring[v].used = true;
  return false;
}

bool fmDedupPeek(const char *callSign, uint16_t msgId, uint8_t fragIdx) {
  return findLive(keyHash(callSign, msgId, fragIdx), millis()) >= 0;
}

void fmDedupClear() { reset(); }

uint8_t fmDedupCount() {
  const uint32_t now = millis();
  uint8_t n = 0;
  for (uint8_t i = 0; i < FM_DEDUP_SLOTS; i++) {
    if (g_ring[i].used && (now - g_ring[i].stampMs) < FM_DEDUP_TTL_MS) n++;
  }
  return n;
}
