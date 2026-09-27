/**
 * FloodMesh V4 - managed flooding for SOS, texts, ACKs and heartbeats.
 *
 * Propagation is the same contention scheme as V3: every node that hears a
 * frame with hop budget left rebroadcasts it once, after a random delay whose
 * window is SHORTER for weakly-heard frames, and cancels if it hears somebody
 * else relay it first. V4 adds the node's own battery to that delay (a flat
 * battery waits longer; a unit on external power counts as full), which is the
 * battery weighting in docs/architecture.md §4.4.
 *
 * Forwarding rules (docs/architecture.md §13.3):
 *   - SOS, ACK and heartbeat frames: every unit forwards them.
 *   - Text: a unit on external power always forwards it. A battery unit
 *     forwards it only while it has NOT heard a powered unit's heartbeat
 *     directly (0 hops) in the last FM_MESH_POWERED_NEARBY_MS.
 *   - An ACK is not forwarded by the unit it is addressed to.
 *
 * Priority bands, earliest first: SOS and ACK, then text, then heartbeat.
 * Heartbeats are also the first thing refused when the airtime budget is low.
 *
 * ACK race: a responder unit that receives an SOS queues a "delivered" ACK
 * after the same kind of random delay, and cancels it if it hears another
 * responder's ACK for the same SOS first, so an area answers once.
 *
 * All authenticated frame types are PSK-signed and replay-protected. The
 * "echo" callback reports our OWN frame being relayed by a neighbour.
 */
#pragma once
#include <Arduino.h>

#include "fm_packet.h"

#ifndef FM_MESH_SNR_MIN
#define FM_MESH_SNR_MIN (-20)
#endif
#ifndef FM_MESH_SNR_MAX
#define FM_MESH_SNR_MAX (10)
#endif
#ifndef FM_MESH_CW_MIN
#define FM_MESH_CW_MIN 3
#endif
#ifndef FM_MESH_CW_MAX
#define FM_MESH_CW_MAX 8
#endif
#ifndef FM_MESH_RELAY_QUEUE
#define FM_MESH_RELAY_QUEUE 8
#endif
#ifndef FM_MESH_ACK_QUEUE
#define FM_MESH_ACK_QUEUE 4
#endif
#ifndef FM_MESH_BATT_SLOTS
#define FM_MESH_BATT_SLOTS 8       // extra slots of delay at 0% battery, none at 100%
#endif
#ifndef FM_MESH_POWERED_NEARBY_MS
#define FM_MESH_POWERED_NEARBY_MS (65UL * 60UL * 1000UL)   // two heartbeat periods
#endif

enum FmMeshPrio : uint8_t {
  FM_PRIO_SOS = 0,         // SOS and ACK
  FM_PRIO_TEXT = 1,
  FM_PRIO_HEARTBEAT = 2,
};

struct FmAlarmRx {
  FmHeader header;
  uint8_t  alarmType;
  uint8_t  hops;       // relays it passed through (0 = heard directly)
  float    snr;
  float    rssi;
};

struct FmTextRx {
  FmHeader header;
  char     text[FM_TEXT_MAX_CHARS + 1];
  uint8_t  hops;
  float    snr;
  float    rssi;
};

struct FmAckRx {
  FmHeader header;     // header.callSign = the responder that sent it
  uint8_t  kind;       // FM_ACK_DELIVERED / FM_ACK_HELP
  char     target[FM_CALLSIGN_LEN + 1];   // the SOS sender, space padded
  uint8_t  targetMsgId;
  uint8_t  hops;
  float    snr;
  float    rssi;
};

struct FmHeartbeatRx {
  FmHeader header;     // header.callSign = the powered unit
  uint8_t  battPct;    // 255 = unknown
  uint8_t  flags;
  uint8_t  hops;
  float    snr;
  float    rssi;
};

struct FmEchoRx {
  uint8_t type;        // FM_TYPE_*
  uint8_t msgId;
  float   rssi;
  float   snr;
};

typedef void (*FmAlarmHandler)(const FmAlarmRx *rx);
typedef void (*FmTextHandler)(const FmTextRx *rx);
typedef void (*FmAckHandler)(const FmAckRx *rx);
typedef void (*FmHeartbeatHandler)(const FmHeartbeatRx *rx);
typedef void (*FmEchoHandler)(const FmEchoRx *rx);

void fmMeshBegin();
void fmMeshLoop(uint32_t now);

void fmMeshSetAlarmHandler(FmAlarmHandler cb);
void fmMeshSetTextHandler(FmTextHandler cb);
void fmMeshSetAckHandler(FmAckHandler cb);
void fmMeshSetHeartbeatHandler(FmHeartbeatHandler cb);
void fmMeshSetEchoHandler(FmEchoHandler cb);

/** This unit's power state and battery, for the forwarding rules and relay race. */
void fmMeshSetPowered(bool powered);
void fmMeshSetBattery(uint8_t pct);

/** True while a powered unit's heartbeat was heard directly within the window. */
bool     fmMeshPoweredNearby(uint32_t now);
/** Age of the last direct heartbeat, or UINT32_MAX if none was ever heard. */
uint32_t fmMeshPoweredNearbyAgeMs(uint32_t now);

/** Send an alarm (SOS) now. Returns the msgId, or -1 on failure. Never refused for duty cycle. */
int  fmMeshSendAlarm(uint8_t alarmType);

/**
 * Send a text (upper-cased, up to FM_TEXT_MAX_CHARS). Returns the msgId, or -1
 * if it is empty or the duty-cycle budget cannot cover it.
 */
int  fmMeshSendText(const char *text);

/** Send an ACK now (used for "Help coming"). Returns the msgId, or -1. */
int  fmMeshSendAck(uint8_t kind, const char *target, uint8_t targetMsgId);

/**
 * Queue a "delivered" ACK for an SOS, sent after a random delay unless another
 * responder's ACK for the same SOS is heard first. snr is the SOS's, so the
 * delay follows the same rule as relaying.
 */
void fmMeshQueueAck(uint8_t kind, const char *target, uint8_t targetMsgId, float snr);

/** Send a heartbeat now. Returns the msgId, or -1 (radio busy or budget spent). */
int  fmMeshSendHeartbeat(uint8_t battPct);

/** Anything queued or on the air: the unit must not sleep yet. */
bool     fmMeshBusy();
uint8_t  fmMeshRelayQueueDepth();
uint32_t fmMeshFramesRelayed();
uint32_t fmMeshFramesSuppressed();
uint32_t fmMeshBackoffMs(float snr, uint8_t prio);
