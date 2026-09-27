/**
 * FloodMesh - LoRa time-on-air arithmetic and the regulatory duty-cycle ledger.
 *
 * Why this module exists
 * ----------------------
 * FloodMesh is licence-exempt in India under G.S.R. 853(E) (2021), but Table II
 * of that notification is not a free pass. For this device class it caps the
 * transmitter at 500 mW e.r.p., a 200 kHz channel, and - the clause that
 * actually constrains firmware - a 2.5% duty cycle. 2.5% of an hour is
 * 90 seconds of transmit. Everything below exists to keep an honest count
 * against those 90 seconds.
 *
 * The failure this prevents is not one user talking too much. It is a relay.
 * A node that happens to sit on a rooftop with line of sight across a
 * neighbourhood repeats OTHER people's traffic, and one 10 s clip is 8
 * fragments, ~2.6 s of air, every time it passes through. Thirty-five relayed
 * clips in an hour and the box is transmitting illegally with nothing on screen
 * to say so, because no single user action ever looked excessive. Without a
 * ledger the device becomes non-compliant silently, which is the worst way for
 * it to happen: nobody can see it, so nobody can fix it in the field.
 *
 * THE ALARM CARVE-OUT - READ THIS BEFORE CHANGING ANYTHING HERE
 * -------------------------------------------------------------
 * fmAirtimeAllows() returns true for a Layer 1 alarm UNCONDITIONALLY. There is
 * no budget check, no soft limit, no reserve that the alarm path draws down.
 * This is a deliberate product decision, not an oversight:
 *
 *   A device that refuses to send NEED EVACUATION because it has already
 *   used 90.1 seconds this hour is worse than useless. It is a box that
 *   killed someone to stay compliant.
 *
 * The consequence, stated plainly so nobody discovers it in a lab report: a
 * node under sustained alarm load CAN exceed 2.5%. Twenty-four alarms in an
 * hour is ~1.7 s of air (nothing), but a stuck button, a chord-happy operator,
 * or a dense relay mesh in a real flood can push past the cap. When that
 * happens the ledger records the overrun HONESTLY - alarms are always charged,
 * fmAirtimeUsedMs() will read above the budget, and the UI can show it - rather
 * than hiding the overrun by declining to count the packets that caused it. A
 * ledger that only counts the traffic it is willing to throttle is a lie.
 *
 * Layer 2 (voice) is what actually pays for this. Because alarm airtime lands
 * in the same window, an alarm burst suppresses voice until it ages out. Voice
 * yields; life safety does not.
 *
 * Time on air
 * -----------
 * fmToaMs() implements the Semtech integer formula (DS.SX1261-2.W.APP rev 1.2
 * section 6.1.4) for the FloodMesh radio config, and is arithmetically
 * IDENTICAL to RadioLib's SX126x::calculateTimeOnAir() - same operation order,
 * same integer truncation, same ceil-by-adding-divisor-minus-one. It is
 * duplicated here rather than called through RadioLib on purpose: the ledger
 * must be able to price a packet BEFORE the radio object exists (boot-time
 * budget display) and without a live SPI transaction, and the duty-cycle
 * decision must not depend on which radio driver a future port happens to use.
 *
 * Reference values for this config (SF7 / BW125 / CR 4/5 / explicit hdr /
 * CRC on / LDRO off / 16-symbol preamble):
 *
 *     24 B alarm frame     ->  69888 us  ( 70 ms charged)
 *    204 B voice fragment  -> 331008 us  (332 ms charged)
 *    156 B last fragment   -> 264448 us  (265 ms charged)
 *    whole 10 s clip       -> 7 x 331008 + 264448 = 2581504 us (~2.58 s)
 *
 * fmToaMs() rounds UP to the next whole millisecond. That is deliberate: every
 * rounding decision in a regulatory ledger should go against us, never in our
 * favour. Use fmToaUs() when the exact figure matters - comparing against
 * RadioLib, or printing a number a compliance test will re-derive.
 *
 * THE TRAP: these constants must match the radio
 * ---------------------------------------------
 * FM_TOA_* below describe what the radio is ASSUMED to be doing. Nothing here
 * reads the SX1262. If a future radio module drops to SF9 for range and does
 * not override FM_TOA_SF to match, this file keeps pricing packets at SF7 and
 * the ledger under-counts by roughly 4x while still reporting green. Whoever
 * touches the radio config owns these defines too.
 *
 * The ledger, and where it is approximate
 * ---------------------------------------
 * A rolling window built from FM_DUTY_BUCKETS per-minute buckets in a ring, not
 * one entry per packet. 60 uint32_t is 240 bytes and self-expiring; a
 * per-packet log would be unbounded exactly during a relay storm, which is the
 * one time it would matter.
 *
 * The cost of buckets is granularity, and it errs the safe way: the oldest
 * bucket is only partially expired, so the effective window is 60 to 61 minutes
 * of history rather than exactly 60. The ledger therefore counts slightly MORE
 * than an hour and permits slightly LESS than the law allows.
 *
 * The window advances lazily, inside whichever entry point is called. Every
 * function that reads or writes the ledger rolls it first, so a node that
 * transmits nothing for two hours still reads 0% without needing a periodic
 * tick. The one requirement is that SOMETHING calls into this module at least
 * once per millis() epoch (49.7 days), which the UI loop does thousands of
 * times a second. All time arithmetic is unsigned subtraction, so the epoch
 * wrap itself is handled.
 *
 * fmAirtimePermille() returns uint16_t, not uint8_t
 * -------------------------------------------------
 * The original module contract said uint8_t for a documented 0..1000 range,
 * which cannot be represented. That was a mistake in the contract rather than
 * in this module, and it was widened before anything was written against it.
 * The range matters: a sustained-alarm overrun legitimately exceeds 255
 * permille, and a compliance log that silently reads "25.5%" for every value
 * above it would be worse than no log.
 */
#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------- tunables
#ifndef FM_DUTY_PERMILLE
#define FM_DUTY_PERMILLE 25          // G.S.R. 853(E) Table II: 2.5% duty cycle
#endif
#ifndef FM_DUTY_WINDOW_MS
#define FM_DUTY_WINDOW_MS 3600000UL  // the "per hour" that the 2.5% is measured over
#endif
#ifndef FM_DUTY_BUCKETS
#define FM_DUTY_BUCKETS 60           // ring granularity; 60 = one bucket per minute
#endif

// Radio configuration ASSUMED by the time-on-air formula. See "THE TRAP" above:
// these are never read back from the SX1262, so a radio module that changes the
// modem settings must override them to match or the ledger silently lies.
#ifndef FM_TOA_SF
#define FM_TOA_SF 7                  // spreading factor, 5..12
#endif
#ifndef FM_TOA_BW_KHZ
#define FM_TOA_BW_KHZ 125            // the 200 kHz channel ceiling permits 125, not 250
#endif
#ifndef FM_TOA_CR_DENOM
#define FM_TOA_CR_DENOM 5            // 4/5 coding rate -> 5. Range 5..8.
#endif
#ifndef FM_TOA_PREAMBLE
#define FM_TOA_PREAMBLE 16           // symbols; long enough for RX duty-cycled peers
#endif
#ifndef FM_TOA_EXPLICIT_HDR
#define FM_TOA_EXPLICIT_HDR 1        // 1 = explicit header (variable length frames)
#endif
#ifndef FM_TOA_CRC
#define FM_TOA_CRC 1                 // 1 = hardware CRC appended
#endif
#ifndef FM_TOA_LDRO
#define FM_TOA_LDRO 0                // low data rate optimise; off below SF11 at BW125
#endif

// Tripwire for the trap described above. This module deliberately does NOT
// include the radio header - the ledger must work before a radio exists - so
// the two sets of constants are only coupled by convention. But when both
// headers land in the same translation unit we can at least refuse to build a
// ledger that disagrees with the transmitter it is supposed to be metering.
// Silence is not an option here: a mismatch does not misbehave, it just quietly
// reports the wrong number forever. Bandwidth is excluded because fm_radio.h
// carries it as a float and the preprocessor cannot compare those.
#if defined(FM_LORA_SF) && (FM_LORA_SF != FM_TOA_SF)
#error "FM_LORA_SF and FM_TOA_SF disagree: the duty-cycle ledger would price every packet at the wrong spreading factor. Override both."
#endif
#if defined(FM_LORA_CR) && (FM_LORA_CR != FM_TOA_CR_DENOM)
#error "FM_LORA_CR and FM_TOA_CR_DENOM disagree: the duty-cycle ledger would mis-price every packet. Override both."
#endif
#if defined(FM_LORA_PREAMBLE) && (FM_LORA_PREAMBLE != FM_TOA_PREAMBLE)
#error "FM_LORA_PREAMBLE and FM_TOA_PREAMBLE disagree: the duty-cycle ledger would mis-price every packet. Override both."
#endif

/** Transmit budget inside the window, in ms. 3600000 * 25 / 1000 = 90000. */
#define FM_DUTY_BUDGET_MS \
  ((uint32_t)(((uint64_t)(FM_DUTY_WINDOW_MS) * (uint64_t)(FM_DUTY_PERMILLE)) / 1000u))

/** SX1262 hard limit on a LoRa payload. Longer lengths are clamped to this. */
#define FM_TOA_LEN_MAX 255

// ---------------------------------------------------------------- API
/** Zero the ledger and anchor the window. Call once from setup(). */
void fmAirtimeBegin();

/**
 * Time on air of a LoRa frame carrying payloadLen bytes under the FM_TOA_*
 * config, rounded UP to whole milliseconds. payloadLen is clamped to
 * FM_TOA_LEN_MAX.
 */
uint32_t fmToaMs(uint16_t payloadLen);

/** The same computation, exact, in microseconds. Bit-identical to RadioLib. */
uint32_t fmToaUs(uint16_t payloadLen);

/**
 * Record an actual transmission. Charge everything that keys the PA, alarms
 * included - see the carve-out note above. Prefer the measured wall-clock TX
 * duration when the radio driver reports one: fmToaMs() is a prediction and
 * excludes PA ramp and TX-mode entry.
 */
void fmAirtimeCharge(uint32_t ms);

/**
 * May a transmission of ms milliseconds proceed?
 *
 * isAlarm == true  -> ALWAYS true. Layer 1 is never throttled.
 * isAlarm == false -> true only while used + ms stays inside the budget. Alarm
 *                     airtime counts against this, so an alarm burst suppresses
 *                     voice until it ages out of the window.
 *
 * This charges nothing. Call fmAirtimeCharge() once the transmission is done.
 */
bool fmAirtimeAllows(uint32_t ms, bool isAlarm);

/** Airtime spent in the rolling window, in ms. MAY exceed FM_DUTY_BUDGET_MS. */
uint32_t fmAirtimeUsedMs();

/** Window occupancy in tenths of a percent, 0..1000. */
uint16_t fmAirtimePermille();
