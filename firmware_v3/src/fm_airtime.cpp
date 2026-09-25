/**
 * FloodMesh - time-on-air and duty-cycle ledger implementation.
 * See fm_airtime.h for the regulatory background and the alarm carve-out.
 *
 * All time arithmetic uses unsigned subtraction (now - then), which is correct
 * across the 49.7-day millis() rollover. Never compare timestamps directly.
 *
 * The time-on-air helpers are constexpr so the reference values in the header
 * can be checked by static_assert below rather than by a test that someone has
 * to remember to run. If the arithmetic ever drifts from RadioLib, the build
 * breaks - which is the only kind of cross-check that survives a refactor.
 */
#include "fm_airtime.h"

#include <string.h>

namespace {

// ------------------------------------------------------------ configuration
static_assert(FM_TOA_SF >= 5 && FM_TOA_SF <= 12, "LoRa spreading factor is 5..12");
static_assert(FM_TOA_BW_KHZ > 0, "bandwidth must be a positive integer kHz");
static_assert(FM_TOA_CR_DENOM >= 5 && FM_TOA_CR_DENOM <= 8,
              "LoRa coding rate denominator is 5..8 (4/5 .. 4/8)");
static_assert(FM_TOA_PREAMBLE >= 6, "SX126x will not transmit fewer than 6 preamble symbols");

static_assert(FM_DUTY_PERMILLE > 0 && FM_DUTY_PERMILLE <= 1000,
              "duty cycle is tenths of a percent, 1..1000");
static_assert(FM_DUTY_BUCKETS >= 1 && FM_DUTY_BUCKETS <= 3600,
              "bucket count must be sane - each one costs 4 bytes of DRAM");
static_assert(FM_DUTY_WINDOW_MS % FM_DUTY_BUCKETS == 0,
              "window must divide evenly into buckets or the ring drifts against "
              "the window it is supposed to represent");

/** Width of one ring bucket. 3600000 / 60 = 60000 ms. */
const uint32_t kBucketMs = (uint32_t)FM_DUTY_WINDOW_MS / (uint32_t)FM_DUTY_BUCKETS;
const uint32_t kBudgetMs = FM_DUTY_BUDGET_MS;

// ------------------------------------------------------------ time on air
// Semtech DS.SX1261-2.W.APP rev 1.2 section 6.1.4, in the integer form
// RadioLib uses (SX126x::calculateTimeOnAir). Everything is microseconds;
// quantities carrying a .25 term are held pre-multiplied by four and named
// _x4, which is why the final division by 4 is the last thing that happens.
//
// One deliberate difference from RadioLib: it stores bandwidth as a float and
// therefore divides in floating point. For an integer kHz bandwidth the two
// agree exactly (1280000 / 1250.0f is 1024.0f on the nose), so this config is
// bit-identical. A fractional bandwidth - 62.5, 41.7, 31.25 kHz - cannot even
// be expressed by FM_TOA_BW_KHZ, and would be the one case where the results
// could part company by a symbol.

constexpr uint32_t toaSymbolUs() {
  return ((uint32_t)10000u << FM_TOA_SF) / ((uint32_t)FM_TOA_BW_KHZ * 10u);
}

/** 4.25 symbols of sync, or 6.25 at SF5/SF6, held x4 to stay in integers. */
constexpr int32_t toaSfCoeff1X4() {
  return (FM_TOA_SF == 5 || FM_TOA_SF == 6) ? 25 : 17;
}
constexpr int32_t toaSfCoeff2() {
  return (FM_TOA_SF == 5 || FM_TOA_SF == 6) ? 0 : 8;
}

/** Low data rate optimise steals two bits per symbol from the payload. */
constexpr int32_t toaSfDivisor() {
  return FM_TOA_LDRO ? 4 * (FM_TOA_SF - 2) : 4 * FM_TOA_SF;
}

constexpr int32_t toaBitCountRaw(uint32_t len) {
  return (int32_t)(8u * len) + (FM_TOA_CRC ? 16 : 0) - 4 * FM_TOA_SF +
         toaSfCoeff2() + (FM_TOA_EXPLICIT_HDR ? 20 : 0);
}

/** A very short frame can drive the numerator negative; the floor is zero. */
constexpr int32_t toaBitCount(uint32_t len) {
  return toaBitCountRaw(len) < 0 ? 0 : toaBitCountRaw(len);
}

/** Integer CEIL, done by adding divisor-1 before the divide. */
constexpr uint32_t toaPayloadSymbols(uint32_t len) {
  return (uint32_t)(toaBitCount(len) + toaSfDivisor() - 1) / (uint32_t)toaSfDivisor();
}

constexpr uint32_t toaSymbolsX4(uint32_t len) {
  return (uint32_t)((FM_TOA_PREAMBLE + 8) * 4 + toaSfCoeff1X4()) +
         toaPayloadSymbols(len) * (uint32_t)FM_TOA_CR_DENOM * 4u;
}

constexpr uint32_t toaUs(uint32_t len) {
  return (toaSymbolUs() * toaSymbolsX4(len)) / 4u;
}

// The numbers quoted in the header, pinned to the arithmetic that produces
// them. Only meaningful for the stock config - an override legitimately moves
// every one of these, so the checks compile out rather than blocking the port.
#if (FM_TOA_SF == 7) && (FM_TOA_BW_KHZ == 125) && (FM_TOA_CR_DENOM == 5) && \
    (FM_TOA_PREAMBLE == 16) && FM_TOA_EXPLICIT_HDR && FM_TOA_CRC && !FM_TOA_LDRO
static_assert(toaSymbolUs() == 1024u, "SF7/BW125 symbol is 1024 us");
static_assert(toaUs(0) == 34048u, "empty frame must be 34048 us");
static_assert(toaUs(24) == 69888u,
              "24 B Layer 1 alarm must be 69888 us - cross-checked against "
              "RadioLib SX126x::calculateTimeOnAir");
static_assert(toaUs(156) == 264448u, "156 B last voice fragment must be 264448 us");
static_assert(toaUs(204) == 331008u,
              "204 B voice fragment must be 331008 us - cross-checked against "
              "RadioLib SX126x::calculateTimeOnAir");
static_assert(toaUs(255) == 407808u, "255 B maximum frame must be 407808 us");
// A whole 10 s clip costs ~2.58 s of air, which is 2.9% of the hourly budget.
static_assert(7u * toaUs(204) + toaUs(156) == 2581504u,
              "one 10 s clip is 8 fragments totalling 2581504 us");
#endif

// ------------------------------------------------------------ ledger state
// A ring of per-minute buckets. Each transmission lands in whichever bucket is
// current; advancing the ring zeroes the bucket it steps onto, so history
// expires by itself with no timestamps to keep and nothing to scan.
uint32_t g_bucketMs[FM_DUTY_BUCKETS];
uint16_t g_bucketIdx     = 0;
uint32_t g_bucketStartMs = 0;   // millis() at which g_bucketIdx opened
uint32_t g_usedMs        = 0;   // running sum of g_bucketMs, kept incrementally

/**
 * Roll the window forward to `now`. Called by every entry point that reads or
 * writes the ledger, which is what lets the module work without a periodic
 * tick: an idle node's buckets expire the moment anyone looks at them.
 */
void advance(uint32_t now) {
  const uint32_t elapsed = now - g_bucketStartMs;
  if (elapsed < kBucketMs) {
    return;
  }
  const uint32_t steps = elapsed / kBucketMs;

  if (steps >= (uint32_t)FM_DUTY_BUCKETS) {
    // Idle for at least a whole window: nothing that was in the ring is still
    // inside the hour, so skip the walk entirely.
    memset(g_bucketMs, 0, sizeof(g_bucketMs));
    g_usedMs = 0;
    g_bucketIdx = (uint16_t)((g_bucketIdx + steps) % (uint32_t)FM_DUTY_BUCKETS);
  } else {
    for (uint32_t i = 0; i < steps; i++) {
      g_bucketIdx = (uint16_t)((g_bucketIdx + 1u) % (uint32_t)FM_DUTY_BUCKETS);
      g_usedMs -= g_bucketMs[g_bucketIdx];
      g_bucketMs[g_bucketIdx] = 0;
    }
  }

  // Advance by whole buckets, not to `now`, so the ring stays phase-locked to
  // its original anchor instead of drifting a few ms later on every call.
  // steps * kBucketMs <= elapsed, so this cannot overflow.
  g_bucketStartMs += steps * kBucketMs;
}

}  // namespace

// ---------------------------------------------------------------- API
void fmAirtimeBegin() {
  memset(g_bucketMs, 0, sizeof(g_bucketMs));
  g_bucketIdx = 0;
  g_usedMs = 0;
  g_bucketStartMs = millis();

  Serial.printf("[airtime] budget %lu ms per %lu ms window (%u permille), "
                "%u buckets of %lu ms\n",
                (unsigned long)kBudgetMs, (unsigned long)FM_DUTY_WINDOW_MS,
                (unsigned)FM_DUTY_PERMILLE, (unsigned)FM_DUTY_BUCKETS,
                (unsigned long)kBucketMs);
}

uint32_t fmToaUs(uint16_t payloadLen) {
  // Clamp rather than trust: a length that came off the air or out of a
  // fragment index must not be able to invent airtime the radio cannot spend.
  const uint32_t len =
      payloadLen > (uint16_t)FM_TOA_LEN_MAX ? (uint32_t)FM_TOA_LEN_MAX : (uint32_t)payloadLen;
  return toaUs(len);
}

uint32_t fmToaMs(uint16_t payloadLen) {
  // Round up. Under-charging the ledger is a compliance failure; over-charging
  // by under a millisecond in ~70 costs nothing anyone can measure.
  return (fmToaUs(payloadLen) + 999u) / 1000u;
}

void fmAirtimeCharge(uint32_t ms) {
  if (ms == 0) {
    return;
  }
  advance(millis());

  // Saturate instead of wrapping. A wrapped total would read as a near-empty
  // budget, i.e. the ledger would report compliance precisely when the device
  // had transmitted more than the counter can hold.
  const uint32_t room = 0xFFFFFFFFu - g_usedMs;
  if (ms > room) {
    ms = room;
  }
  // The bucket can never overflow ahead of the total, since every bucket is a
  // subset of the sum that was just bounded.
  g_bucketMs[g_bucketIdx] += ms;
  g_usedMs += ms;
}

bool fmAirtimeAllows(uint32_t ms, bool isAlarm) {
  // THE CARVE-OUT. Layer 1 is never throttled, never rate-limited, and never
  // asked whether there is budget left. A device that will not shout for help
  // because it has spent 90 seconds this hour is a device that failed at the
  // only job it has. The overrun is still charged by fmAirtimeCharge() and
  // still shows up in fmAirtimeUsedMs() - the ledger reports the truth, it just
  // does not get a vote. See fm_airtime.h.
  if (isAlarm) {
    return true;
  }

  advance(millis());

  // Rearranged to keep both sides inside uint32_t: `used + ms > budget` would
  // wrap for an absurd ms and wrongly permit the transmission.
  if (ms > kBudgetMs) {
    return false;
  }
  return g_usedMs <= (kBudgetMs - ms);
}

uint32_t fmAirtimeUsedMs() {
  advance(millis());
  return g_usedMs;
}

uint16_t fmAirtimePermille() {
  advance(millis());
  // 64-bit only to keep an overridden window honest; with the stock 3600000 ms
  // window the compiler folds the divisor and this is a single 32-bit divide.
  const uint32_t p =
      (uint32_t)(((uint64_t)g_usedMs * 1000u) / (uint64_t)FM_DUTY_WINDOW_MS);
  return p > 1000u ? (uint16_t)1000 : (uint16_t)p;
}

