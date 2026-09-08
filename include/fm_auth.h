/**
 * FloodMesh - Layer 1 alarm authentication and anti-replay.
 *
 * A forged EVACUATE is the worst thing this radio can emit. Layer 1 alarms are
 * therefore the only frames that carry a MAC: 8 bytes of HMAC-SHA256 keyed with
 * a 16-byte network pre-shared key, sitting at FM_OFF_MAC. Voice is left
 * unauthenticated on purpose - see the reasoning in fm_packet.h.
 *
 * Why HMAC-SHA256 and not AES-CMAC
 * --------------------------------
 * CMAC is the obvious choice for a short tag on a short message, and it is not
 * available here. arduino-esp32 2.0.17 ships mbedTLS with CONFIG_MBEDTLS_CMAC_C
 * undefined, so MBEDTLS_CMAC_C is never set in esp_config.h and cmac.c compiles
 * to an object file with no symbols at all - the link then fails with an
 * undefined reference to mbedtls_cipher_cmac, not with a helpful error.
 * Verified against the shipped sdkconfig.h and by running nm over cmac.c.obj in
 * libmbedcrypto.a. Do not "fix" this by calling CMAC; it will not link.
 * MBEDTLS_MD_C and MBEDTLS_SHA256_C are unconditionally defined, so the md API
 * is safe ground, and SHA-256 is hardware-accelerated on the S3 anyway.
 *
 * The tag is truncated to FM_MAC_LEN (8) bytes = 64 bits. Truncating an HMAC is
 * standard practice (HMAC-SHA256-64) and buys back 24 bytes of airtime we do
 * not have. A blind forgery succeeds with probability 2^-64 per attempt, and an
 * attacker has to make those attempts over the air at a few frames per second,
 * so the bound is comfortable for a device whose battery lasts days.
 *
 * THE AUTHENTICATED REGION IS NOT CONTIGUOUS
 * ------------------------------------------
 * It is frame bytes [0,1] followed by [3..15]:
 *
 *      0  1  |2|  3  4  5  6  7  8  9 10 11 12 13 14 15 | 16..23
 *     ver ty |hop| <------- callSign .. counter -------> |  mac
 *      ^--^   ^^  ^------------------------------------^
 *     MACed  skip              MACed                      the tag itself
 *
 * Byte 2 is hopLimit, which every relay decrements. Covering it would break the
 * MAC at the first hop. Skipping it is what lets a multi-hop mesh and
 * end-to-end authentication co-exist; it is not sloppiness. What the hole buys
 * an attacker is spelled out at the bottom of this comment.
 *
 * The scratch buffer is built by ONE private helper that both fmAuthSignAlarm()
 * and fmAuthVerifyAlarm() call. That is deliberate: a signer and a verifier
 * that each build their own byte list will eventually disagree by one byte, and
 * the symptom is "alarms silently stopped being trusted after a firmware bump",
 * which is close to undebuggable in a flood. There is one list, in fm_auth.cpp.
 *
 * Replay protection
 * -----------------
 * The MAC proves an alarm was genuine ONCE. Without a counter check, a recorded
 * "SAFE" from yesterday can be re-broadcast today and everyone stops looking
 * for that person. fmReplayAccept() keeps a per-call-sign high-water mark of
 * the counter at FM_OFF_COUNTER; anything at or below it is a replay.
 *
 * CALLER CONTRACT, AND IT MATTERS: verify the MAC FIRST, and only call
 * fmReplayAccept() on a frame that passed. The replay table cannot tell a
 * genuine new neighbour from an invented one, so calling it on unverified
 * frames lets anyone with a transmitter stuff it with junk call signs and evict
 * the real ones for free.
 *
 *     if (!fmAuthVerifyAlarm(buf))                 return;   // forged
 *     if (!fmReplayAccept(hdr.callSign, counter))  return;   // replay
 *
 * The table holds FM_REPLAY_SLOTS entries. When it is full the least-recently-
 * heard node is evicted - never "accept everything", which would silently
 * disable replay protection exactly when the network is busiest. The honest
 * consequence of eviction: an evicted node's old counters are forgotten, so a
 * recording of one of its earlier alarms will be accepted again (once - it
 * re-inserts the node at that old counter, and anything at or below it is
 * rejected from then on). This can only be triggered by more than
 * FM_REPLAY_SLOTS distinct nodes genuinely being heard, because an attacker
 * cannot insert a row without a valid MAC. Size the table above the real head
 * count if you can spare the 16 bytes per slot.
 *
 * THE REBOOT HAZARD - read this before shipping
 * ---------------------------------------------
 * The counter is the sender's business; this module only compares. A node that
 * reboots and restarts its counter at 0 will have every alarm it sends REJECTED
 * as a replay by every neighbour that heard it before the reboot, until the
 * counter climbs back past the old high-water mark. On a device that may brown
 * out mid-flood that is a life-safety failure, not a nuisance. The sender MUST
 * persist its counter in NVS and reload it at boot, and should jump it forward
 * by a margin on every boot so that a lost write can never go backwards.
 *
 * What the unauthenticated hopLimit buys an attacker
 * --------------------------------------------------
 * CAN: change how far an alarm travels - set hopLimit to 0 to stop a genuine
 * alarm one hop short of the person who needed to hear it, or to FM_HOP_MAX to
 * make it flood further and burn airtime and battery across the colony. Both
 * are denial of service, and a jammer with the same radio does worse without
 * touching a single bit.
 * CANNOT: forge an alarm, change which alarm it is, change who it claims to be
 * from, change the channel or msgId, or replay an old one. Everything that
 * decides meaning is inside the MAC; only reach is malleable.
 *
 * Not reentrant. One mbedTLS context is set up in fmAuthBegin() and reused, so
 * no packet path allocates - call every fmAuth/fmReplay function from a single
 * task, the same way fm_input is polled from one loop.
 */
#pragma once
#include <Arduino.h>

#include "fm_packet.h"

// ---------------------------------------------------------------- tunables
#ifndef FM_REPLAY_SLOTS
#define FM_REPLAY_SLOTS 32   // one relief camp / apartment block inside a 3-hop
#endif                       // mesh. 32 x 16 B = 512 B of DRAM. Raise it before
                             // eviction starts costing you replay protection.

// ---------------------------------------------------------------- constants
#define FM_PSK_LEN         16  // fixed by the API: fmAuthBegin(const uint8_t[16])
// Everything before the MAC except the hop byte: [0] + [2..13]. Derived rather
// than written out, so a change to the frame layout moves it automatically and
// the static_assert in fm_auth.cpp catches any table that fell behind.
#define FM_AUTH_REGION_LEN (FM_OFF_MAC - 1)

// ---------------------------------------------------------------- API
/**
 * Install the network PSK and set up the HMAC context. Call once from setup(),
 * before any alarm is sent or received. Safe to call again to re-key.
 *
 * Until this succeeds the module fails CLOSED: signing writes an all-zero MAC
 * (which no receiver will accept) and verification returns false. It does not
 * touch the replay table - re-keying and forgetting counters are separate
 * decisions, and fmReplayReset() is the one that forgets.
 */
void fmAuthBegin(const uint8_t psk[16]);

/**
 * Fill frame bytes [16..23] with the truncated HMAC over the authenticated
 * region. frame24 must already hold a complete FM_ALARM_LEN alarm frame; the
 * hopLimit at byte 2 may be anything, it is not covered.
 */
void fmAuthSignAlarm(uint8_t *frame24);

/**
 * True if the tag at [16..23] matches. The comparison is constant-time, so a
 * listener cannot walk the tag out one byte at a time by timing the reply.
 *
 * The caller must already have established that this is FM_ALARM_LEN bytes of
 * alarm (fmParseHeader) - this function reads exactly 24 bytes and checks
 * nothing else about them. It does not need to: ver and type are both inside
 * the MAC, so a frame signed as one type can never verify as another.
 */
bool fmAuthVerifyAlarm(const uint8_t *frame24);

/**
 * True if this (callSign, counter) pair is fresh; false if it is a replay.
 * Accepting also records it, so calling twice with the same pair returns true
 * and then false. Only the first FM_CALLSIGN_LEN characters of callSign are
 * significant, and it must be NUL-terminated (FmHeader.callSign is; the raw 5
 * bytes off the air are NOT - do not pass those).
 *
 * An unknown call sign is accepted and inserted: a neighbour who walks into
 * range mid-flood has to be able to join without a handshake.
 *
 * MAC-VERIFY BEFORE CALLING THIS. See the header comment.
 */
bool fmReplayAccept(const char *callSign, uint32_t counter);

/**
 * Forget every counter. Intended for tests and for a deliberate re-key; on a
 * live node it re-opens the replay window for every alarm ever recorded off the
 * air, so do not wire it to anything an operator can press by accident.
 */
void fmReplayReset();
