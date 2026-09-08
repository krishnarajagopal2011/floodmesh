/**
 * FloodMesh - SNR-biased managed flooding with a hard two-tier priority band.
 *
 * This is the only module that decides what goes on air and when. Everything
 * else either produces bytes (audio, codec) or moves them (radio, store).
 *
 * ---------------------------------------------------------------------------
 * How a broadcast propagates
 * ---------------------------------------------------------------------------
 * There is no routing table. Every node that hears a frame and still has hop
 * budget left rebroadcasts it once. The only thing preventing a broadcast
 * storm is that nodes wait different amounts of time before repeating, and
 * cancel if somebody else got there first.
 *
 * The wait is biased by signal strength, and the direction is deliberate:
 * a node that heard the frame WEAKLY waits the SHORTEST time.
 *
 * That seems backwards until you picture the geometry. A node that heard you
 * weakly is far away, at the edge of your range. If it repeats first, the
 * message jumps the maximum possible distance per hop, and every node in
 * between hears the repeat and cancels its own. A strong-signal neighbour
 * repeating first would cover ground you had already covered. Meshtastic
 * arrived at the same conclusion and their source says so outright
 * (RadioInterface.cpp: "low SNR = small CW size (Short Delay)"); this was
 * verified against their tree rather than assumed.
 *
 * ---------------------------------------------------------------------------
 * The priority band - how "Layer 1 never yields" is actually enforced
 * ---------------------------------------------------------------------------
 * Backoff alone would leave alarms competing with voice on equal terms. So the
 * delay window is split into two non-overlapping bands:
 *
 *     0 .............. FM_MESH_L1_BAND_MS .............. band ceiling
 *     |<--- Layer 1 alarms only --->|<--- Layer 2 voice relays only --->|
 *
 * A voice relay can never draw a delay inside the alarm band, no matter how
 * weak its signal. An alarm always transmits before any voice frame that was
 * queued at the same moment. This is structural, not a comparison at queue
 * time that a scheduling bug could get wrong - the arithmetic makes the
 * overlap impossible. (The idea is lifted from Meshtastic's reserved band for
 * ROUTER-role nodes; the split here is by traffic class instead of by role.)
 *
 * On top of that, an alarm generated locally preempts a voice burst already in
 * flight: fm_mesh aborts the current fragment mid-air (SX1262 SetStandby is a
 * documented exit from TX) and abandons the remaining fragments. Voice is
 * best-effort by contract, so a truncated note is an acceptable price.
 *
 * ---------------------------------------------------------------------------
 * Why the contention window is exponential rather than linear
 * ---------------------------------------------------------------------------
 * The obvious formula is linear:
 *     delay = base + maxDelay * normalisedSnr + random(0, 40)
 * It has a flaw that only shows up with more than two nodes. Two neighbours at
 * similar distance get similar SNR, so they get nearly the same delay, and a
 * 40 ms jitter is far too narrow to separate them - a single voice fragment is
 * ~330 ms on air, so a 40 ms spread is under an eighth of one packet. They
 * transmit on top of each other and both are lost.
 *
 * So the window is exponential in the contention exponent and the random draw
 * spans the WHOLE window, which is what actually decorrelates equal-SNR peers:
 *
 *     cw    = map(snr, SNR_MIN..SNR_MAX -> CW_MIN..CW_MAX)   // low snr = low cw
 *     delay = band + random(0, 2^cw) * slotMs
 *
 * slotMs is derived from the symbol time rather than hard-coded, so changing
 * spreading factor or bandwidth rescales the whole scheme automatically.
 *
 * ---------------------------------------------------------------------------
 * Reassembly
 * ---------------------------------------------------------------------------
 * A voice note is 8 fragments that may arrive out of order, interleaved with
 * another sender's fragments, or not at all. Reassembly buffers are per-sender
 * and strictly bounded (FM_MESH_REASM_SLOTS): a flood of forged call signs
 * must not be able to exhaust RAM. A partial clip that stops arriving is
 * abandoned after FM_MESH_REASM_TTL_MS and its slot reused.
 *
 * A clip missing any fragment is still delivered upward if enough arrived to
 * be worth hearing - Codec2 conceals a gap far better than silence does, and
 * a half-received cry for help is not garbage. The handler is told how many
 * fragments were missing so the UI can say so.
 */
#pragma once
#include <Arduino.h>

#include "fm_packet.h"

// ---------------------------------------------------------------- tunables
#ifndef FM_MESH_SNR_MIN
#define FM_MESH_SNR_MIN (-20)    // dB, the weakest signal we still decode
#endif
#ifndef FM_MESH_SNR_MAX
#define FM_MESH_SNR_MAX (10)     // dB, anything stronger is treated as adjacent
#endif
#ifndef FM_MESH_CW_MIN
#define FM_MESH_CW_MIN 3         // contention exponent at SNR_MIN -> 2^3 = 8 slots
#endif
#ifndef FM_MESH_CW_MAX
#define FM_MESH_CW_MAX 8         // contention exponent at SNR_MAX -> 2^8 = 256 slots
#endif
#ifndef FM_MESH_REASM_SLOTS
#define FM_MESH_REASM_SLOTS 2    // concurrent inbound voice notes (1500 B each)
#endif
#ifndef FM_MESH_REASM_TTL_MS
#define FM_MESH_REASM_TTL_MS 30000
#endif
#ifndef FM_MESH_RELAY_QUEUE
#define FM_MESH_RELAY_QUEUE 6    // frames awaiting their backoff window
#endif

/** Why a received clip is being handed up. */
struct FmVoiceRx {
  FmHeader header;
  const uint8_t *clip;      // FM_CLIP_BYTES, zero-filled where fragments were lost
  size_t   len;
  uint8_t  fragsReceived;   // out of FM_FRAG_COUNT
  float    snr;
  float    rssi;
};

struct FmAlarmRx {
  FmHeader header;
  uint8_t  alarmType;
  bool     authenticated;   // false = MAC failed; NEVER show these to the user
  float    snr;
  float    rssi;
};

typedef void (*FmAlarmHandler)(const FmAlarmRx *rx);
typedef void (*FmVoiceHandler)(const FmVoiceRx *rx);

// ---------------------------------------------------------------- API
void fmMeshBegin();

/** Pump the mesh: drain RX, run backoff timers, transmit what is due. */
void fmMeshLoop(uint32_t now);

/**
 * Suspend all transmission (relays and voice bursts); receive keeps running.
 *
 * fmRadioTransmit() blocks for as long as its CAD backoff runs, and the
 * microphone's I2S DMA only holds about 64 ms of audio, so a transmit during
 * capture eats a hole in the recording. The UI asserts this for the duration
 * of a recording.
 *
 * Alarms are NOT inhibited - fmMeshSendAlarm() transmits regardless, which is
 * the point of Layer 1. That will disturb an in-progress capture, but an alarm
 * chord discards the clip anyway.
 */
void fmMeshInhibitTx(bool on);

void fmMeshSetAlarmHandler(FmAlarmHandler cb);
void fmMeshSetVoiceHandler(FmVoiceHandler cb);

/**
 * Originate a Layer 1 alarm. Signed with the PSK, charged to the airtime
 * ledger but never refused by it, and it aborts any voice burst in flight.
 * Returns false only if the radio itself failed.
 */
bool fmMeshSendAlarm(uint8_t alarmType, uint8_t channel);

/**
 * Originate a voice note: 1500 bytes of Codec2, fragmented into 8 packets.
 * Returns false if the duty-cycle budget cannot cover the burst - voice is
 * the traffic class that yields, and it yields here too.
 */
bool fmMeshSendVoice(const uint8_t *clip, size_t len, uint8_t channel);

/** True while a burst is being transmitted or is queued behind a backoff. */
bool     fmMeshBusy();
uint8_t  fmMeshRelayQueueDepth();
uint32_t fmMeshFramesRelayed();
uint32_t fmMeshFramesSuppressed();   // cancelled because a peer relayed first

/** Contention delay in ms for a frame heard at this SNR. Exposed for tests. */
uint32_t fmMeshBackoffMs(float snr, bool isAlarm);
