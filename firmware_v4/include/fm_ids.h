/**
 * FloodMesh - deterministic node identity, message ids, and the replay counter.
 *
 * A rescue box pulled out of a drawer must already have a name. There is no
 * enrolment step, no server to ask, and in the field nobody is going to plug a
 * laptop in to name eight radios. So identity is DERIVED from the factory eFuse
 * MAC - the one number that is unique per chip, readable without flash, and
 * identical on every boot - exactly the trick Meshtastic uses for its default
 * node id. An operator override is stored in NVS and wins when present.
 *
 * Rendering a MAC as five characters: the arithmetic, honestly
 * ------------------------------------------------------------
 * The call sign field is 5 bytes of 'A'-'Z' / '0'-'9' (fm_packet.h), so the
 * whole name space is
 *
 *     36^5 = 60,466,176  ~= 2^25.85
 *
 * That is SMALLER than 2^32 = 4,294,967,296, not larger. Five base-36
 * characters cannot carry 32 bits; roughly 6 bits have to go. Seven characters
 * would be needed (36^7 = 78,364,164,096 > 2^32) and we do not have seven.
 * So this module does not pretend to preserve the MAC - it reduces it, and the
 * only question worth answering is how gracefully.
 *
 * Collisions are therefore a birthday problem over 60.5 M: two nodes in a
 * 50-node deployment share a call sign with probability ~2e-5. Acceptable for
 * a neighbourhood mesh, and an operator override exists precisely for the day
 * it is not. Nothing in the protocol assumes call signs are globally unique.
 *
 * The eFuse byte-order trap (read this before "simplifying" the derivation)
 * ------------------------------------------------------------------------
 * ESP.getEfuseMac() memcpys the 6 MAC bytes into the LOW end of a little-endian
 * uint64_t, so the bits land like this:
 *
 *     bits  0..23   mac[0..2]   Espressif OUI - IDENTICAL on every board
 *     bits 24..31   mac[3]      device-unique
 *     bits 32..47   mac[4..5]   device-unique
 *
 * Taking "the low 32 bits" therefore takes 24 constant bits plus ONE unique
 * byte: 256 possible call signs across a whole production run, with a 50%
 * chance of a collision by the 19th node. That is a field failure, not a
 * theoretical one, so the default derivation folds all 48 bits together and
 * runs them through an avalanche mix before reducing mod 36^5. Every MAC bit
 * then influences every output character.
 *
 * Set FM_ID_STRICT_LOW32=1 to get the literal low-32-bits-only behaviour back
 * (kept so the choice is visible and reversible, not because it is a good idea).
 *
 * Case: rejected, not folded
 * --------------------------
 * fmSetCallSign() REJECTS lowercase instead of quietly upper-casing it. Silent
 * correction means the label on the box, the string the operator typed, and the
 * bytes on air can all disagree, and it makes "fm01" and "FM01" the same station
 * without ever saying so. A refusal is one extra keystroke; a station that is
 * not the one you think you are talking to is a wrong rescue address.
 *
 * Why msgId is seeded randomly rather than starting at zero
 * ---------------------------------------------------------
 * Receivers dedup on (callSign, msgId) and their caches outlive our reboot. A
 * node that always restarts at 0 replays ids its neighbours still remember, and
 * its first few real messages get silently dropped as duplicates - worst right
 * after a brownout, which is exactly when it has something to say. Seeding from
 * the hardware RNG costs nothing and removes the whole class of bug.
 *
 * The anti-replay counter: block reservation
 * ------------------------------------------
 * fmNextAuthCounter() feeds the Layer 1 alarm counter, which MUST NOT go
 * backwards across a power cycle - there is no RTC to fall back on, and a
 * counter that regresses lets a recorded EVACUATE be replayed at a receiver
 * that has already forgotten it. But an NVS commit costs ~10-20 ms and burns a
 * flash erase cycle, and doing one per alarm is both slow on the path that
 * matters most and a wear problem.
 *
 * So the persisted value is a HIGH-WATER MARK, not the counter: "everything
 * below this may already have been used; nothing at or above it has been".
 * Boot reads it, reserves a block of FM_AUTH_BLOCK by writing mark+block, and
 * then hands counters out of RAM until the block runs dry.
 *
 *     FM_AUTH_BLOCK   NVS writes per 1000 alarms   worst-case skip per reboot
 *     -------------   --------------------------   --------------------------
 *     16                63                           15
 *     256 (default)      4                          255
 *     4096              <1                          4095
 *
 * The cost is that an unclean reboot abandons the unused remainder of the
 * block: up to FM_AUTH_BLOCK-1 = 255 counter values are skipped. Receivers must
 * treat the counter as STRICTLY INCREASING WITH GAPS and never as a contiguous
 * sequence - "counter == last+1" is not a valid check anywhere.
 *
 * The reservation is lazy: a boot that never raises an alarm never writes NVS,
 * which matters because most boots are exactly that. The first alarm of a
 * session therefore pays one commit - ~15 ms, against 25 ms of button debounce
 * ahead of it and ~90 ms of LoRa airtime behind it, so it is not on the
 * critical path in any meaningful sense.
 *
 * If that commit FAILS the counter is still handed out. A device that cannot
 * write flash is broken, but refusing to raise an alarm because bookkeeping
 * failed is worse than raising one whose counter may regress after the next
 * reboot. The failure is loud on the serial console and degraded, never silent
 * and never blocking.
 */
#pragma once
#include <Arduino.h>

#include "fm_packet.h"

// ---------------------------------------------------------------- tunables
#ifndef FM_NVS_NAMESPACE
#define FM_NVS_NAMESPACE "floodmesh"  // NVS namespaces are capped at 15 chars
#endif
#ifndef FM_NVS_KEY_CALLSIGN
#define FM_NVS_KEY_CALLSIGN "callsign"  // absent = no operator override
#endif
#ifndef FM_NVS_KEY_AUTH_HW
#define FM_NVS_KEY_AUTH_HW "authhw"     // reserved-through high-water mark
#endif
#ifndef FM_AUTH_BLOCK
#define FM_AUTH_BLOCK 256          // counters reserved per NVS write; the skip
#endif                             // after an unclean reboot is this minus one
#ifndef FM_ID_STRICT_LOW32
#define FM_ID_STRICT_LOW32 0       // 1 = literal "low 32 bits of the MAC", which
#endif                             // on this silicon means 256 distinct ids.
                                   // See the eFuse byte-order note above.

// ---------------------------------------------------------------- API
/**
 * Open NVS, adopt the stored call sign or derive one, and load the counter
 * high-water mark. Safe to call once from setup(); everything below returns
 * sane values before it is called, so nothing crashes if the order slips.
 */
void fmIdsBegin();

/**
 * The active call sign: always exactly FM_CALLSIGN_LEN characters plus a NUL,
 * never NULL, space-padded on the right. Points at module-owned storage that
 * stays valid for the life of the program; the contents change only inside
 * fmSetCallSign(). Copy the first FM_CALLSIGN_LEN bytes (no NUL) onto the air.
 */
const char *fmCallSign();

/** True while the call sign comes from the MAC, i.e. no override is stored. */
bool fmCallSignIsDerived();

/**
 * Validate, persist, and adopt an operator call sign. Accepts 1..5 characters,
 * each 'A'-'Z' or '0'-'9', in a NUL-terminated string; lowercase is rejected
 * rather than converted. Stored space-padded to FM_CALLSIGN_LEN.
 *
 * Returns false if the call sign was NOT adopted - either it failed validation
 * or the NVS write failed. Either way nothing changed: the previous call sign
 * is still in force, so there is no half-applied state to reason about.
 *
 * There is deliberately no "go back to derived" call; clearing an override is a
 * bench operation, not a field one.
 */
bool fmSetCallSign(const char *cs);

/**
 * Pure MAC -> call sign derivation. Writes 5 characters and a NUL into out and
 * touches nothing else, so it is usable before fmIdsBegin() and for showing an
 * operator what the factory identity would be. Same chip, same answer, always.
 */
void fmDeriveCallSign(char out[FM_CALLSIGN_LEN + 1]);

/** Rolling per-boot message id for the frame header. Wraps; that is expected. */
uint16_t fmNextMsgId();

/**
 * Next Layer 1 anti-replay counter. Monotonic within a boot and across reboots,
 * with gaps of up to FM_AUTH_BLOCK-1 at every restart. Saturates at UINT32_MAX,
 * which at one alarm per second is 136 years away.
 */
uint32_t fmNextAuthCounter();
