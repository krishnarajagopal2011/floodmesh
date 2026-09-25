/**
 * FloodMesh - packet deduplication ring for the flooding relay.
 *
 * Every node in a flood mesh rebroadcasts what it hears. With no memory of what
 * it has already forwarded, three nodes in earshot of each other turn one alarm
 * into an unbounded storm: A repeats B's copy, B repeats A's repeat, and the
 * channel is dead for everybody - including the person the alarm was about.
 * This ring is that memory. It is the only thing between the relay logic and a
 * self-inflicted denial of service, so it is deliberately dumb, fixed-size and
 * allocation-free.
 *
 * Adapted from Meshtastic's PacketHistory, with one deliberate change.
 *
 * What Meshtastic does, and why it is not enough here
 * ---------------------------------------------------
 * PacketHistory in current master matches on (sender, packet_id) and NEVER
 * expires an entry. An id, once seen, is dead forever. That is fine while a
 * node runs, and wrong the moment one reboots: a fresh node restarts its
 * message counter at a low value, re-emits ids its neighbours already have on
 * file, and is silently ignored by all of them. A flood-hit rescue radio that
 * brown-outs and comes back is exactly the node whose traffic matters most, so
 * every entry here carries a timestamp and an entry older than FM_DEDUP_TTL_MS
 * does not count as seen. Its slot is reusable from that instant.
 *
 * Choosing the TTL is a real tradeoff, not a formality
 * ---------------------------------------------------
 * Too SHORT and a slow multi-hop flood outlives its own suppression: the last
 * hop of an 8-fragment voice note can land seconds after the first, and if the
 * entry has already aged out, a node forwards a packet it has forwarded before
 * and the loop restarts. Budget for the worst case - full hop limit, an
 * airtime-limited relay chain, retries - not for the median.
 *
 * Too LONG and a rebooted neighbour is muted for the whole window. Ten minutes
 * of silence from the one node that just came back up is a bad failure in a
 * flood. Ten minutes is roughly two orders of magnitude above any plausible
 * flood lifetime and small enough that a reboot costs one window, once.
 *
 * The timestamp is FIRST-seen and is deliberately not refreshed when a
 * duplicate arrives. That buys a guarantee worth stating plainly: no key is
 * suppressed for longer than FM_DEDUP_TTL_MS after it was first heard, no
 * matter how many echoes of it turn up. The price is that an echo still
 * circulating at TTL + 1 ms would be relayed once more; a flood lives seconds,
 * so this costs a packet, not a storm.
 *
 * What a 32-bit hash actually means - say it out loud
 * --------------------------------------------------
 * A slot holds a 32-bit FNV-1a hash of [callSign(5) || msgId(2) || fragIdx(1)],
 * not the key itself. Two different packets can therefore hash alike. With 32
 * occupied slots, a new packet collides with probability 32 / 2^32, about
 * 7.5e-9 - roughly one packet in 134 million. Negligible, but not zero, and the
 * consequence is ugly: the colliding packet is dropped as a duplicate. Silently.
 * Nothing logs it, nothing retries it, and because the collision is a property
 * of the two keys it repeats for every copy of that packet until the older
 * entry expires. This node stops relaying that message for up to one TTL; a
 * mesh with redundant paths routes around it, and a leaf sitting behind this
 * node alone does not.
 *
 * Worth knowing before anyone calls this a space optimisation: it is not one
 * here. An exact 8-byte key plus its stamp packs into the same 12 bytes per
 * slot that hash + stamp + flag occupies once the compiler has padded it, and
 * an exact key cannot collide at all. The hash is kept because a match is a
 * single 32-bit compare on the receive path, and because the slot stays the
 * same size if the key ever grows a field. If collisions ever matter more than
 * those two things, store the key.
 *
 * Concurrency - read this before calling from a second task
 * --------------------------------------------------------
 * fmDedupSeenOrMark() is one call so the caller cannot open a window between
 * "have I seen this?" and "remember it". That closes a logic race in the
 * receive path. It does NOT make the ring thread-safe: there is no mutex, no
 * critical section, nothing atomic. Call it from the single receive path only.
 * Never from an ISR.
 *
 * Cost: FM_DEDUP_SLOTS * 12 bytes of .bss (384 B at the default 32) and a
 * linear scan of at most FM_DEDUP_SLOTS slots per packet. No allocation, ever.
 */
#pragma once
#include <stdint.h>

// ---------------------------------------------------------------- tunables
#ifndef FM_DEDUP_SLOTS
#define FM_DEDUP_SLOTS 32          // entries remembered at once. Must cover the
#endif                             // in-flight traffic of a whole flood: one
                                   // voice note alone is 8 keys.
#ifndef FM_DEDUP_TTL_MS
#define FM_DEDUP_TTL_MS (10UL * 60UL * 1000UL)   // 10 min; see the header note -
#endif                                           // short loops, long mutes a
                                                 // rebooted neighbour.

static_assert(FM_DEDUP_SLOTS >= 8, "fewer than 8 slots cannot hold one voice note");
static_assert(FM_DEDUP_SLOTS <= 255, "fmDedupCount() returns uint8_t");
static_assert(FM_DEDUP_TTL_MS > 0, "a zero TTL disables deduplication entirely");
static_assert(FM_DEDUP_TTL_MS <= 0x7FFFFFFFUL,
              "TTL past half the millis() period breaks unsigned age comparison");

// ---------------------------------------------------------------- API
/** Reset the ring. Safe to call again at any time; see fmDedupClear(). */
void fmDedupBegin();

/**
 * Atomically test-and-record one packet key. THIS is what the receive path
 * calls.
 *
 * Returns true if the key was already in the ring and still live, meaning the
 * caller must drop the packet and must not relay it. Returns false if the key
 * is new, in which case it has just been recorded and the caller owns the
 * packet.
 *
 * callSign is compared as the 5 on-air bytes: a shorter NUL-terminated string
 * is space-padded to 5 first, so a locally built "K7AB" and the "K7AB " that
 * comes back off the air produce the same key. Longer strings are truncated at
 * 5. A null pointer hashes as five spaces rather than faulting.
 *
 * fragIdx must be fmFragIdx(fragInfo) for a voice fragment and 0 for an alarm.
 * It is hashed verbatim - pass the whole fragInfo byte by mistake and the key
 * is simply a different, consistently wrong one.
 */
bool fmDedupSeenOrMark(const char *callSign, uint16_t msgId, uint8_t fragIdx);

/**
 * Same test, no recording. For diagnostics and for display code that wants to
 * know whether something is a repeat without claiming it.
 *
 * Do not build a receive path out of peek-then-mark. That reintroduces exactly
 * the check-then-insert window fmDedupSeenOrMark() exists to close.
 */
bool fmDedupPeek(const char *callSign, uint16_t msgId, uint8_t fragIdx);

/**
 * Forget everything. Every key becomes relayable again immediately, so this is
 * for operator-visible discontinuities - a channel or PSK change, a bench
 * reset - and not something to call on a timer.
 */
void fmDedupClear();

/** Live (non-expired) entries. Diagnostics only; expired slots are not reaped. */
uint8_t fmDedupCount();
