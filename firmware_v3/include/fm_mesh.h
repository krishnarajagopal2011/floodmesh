/**
 * FloodMesh V3 - managed flooding for alarms and text messages (no voice).
 *
 * Propagation is the same SNR-biased contention scheme as the earlier
 * firmware (see ../include/fm_mesh.h in the repo root for the full rationale):
 * every node that hears a frame with hop budget left rebroadcasts it once,
 * after a random delay whose window is SHORTER for weakly-heard frames, and
 * cancels if it hears somebody else relay it first. Alarms use a delay band
 * that always finishes before the text band starts, so an alarm is never
 * queued behind a text.
 *
 * Both alarms and texts are PSK-authenticated and replay-protected.
 *
 * V3 adds an "echo" callback: when a node hears its OWN alarm or text being
 * relayed by a neighbour, that is proof somebody received it. The UI shows it
 * as "passed on by a neighbour".
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

struct FmEchoRx {
  uint8_t type;        // FM_TYPE_ALARM or FM_TYPE_TEXT
  uint8_t msgId;
  float   rssi;
  float   snr;
};

typedef void (*FmAlarmHandler)(const FmAlarmRx *rx);
typedef void (*FmTextHandler)(const FmTextRx *rx);
typedef void (*FmEchoHandler)(const FmEchoRx *rx);

void fmMeshBegin();
void fmMeshLoop(uint32_t now);

void fmMeshSetAlarmHandler(FmAlarmHandler cb);
void fmMeshSetTextHandler(FmTextHandler cb);
void fmMeshSetEchoHandler(FmEchoHandler cb);

/** Send an alarm now. Returns the msgId, or -1 on failure. Never refused for duty cycle. */
int  fmMeshSendAlarm(uint8_t alarmType);

/**
 * Send a text (upper-cased, up to FM_TEXT_MAX_CHARS). Returns the msgId, or -1
 * if it is empty or the duty-cycle budget cannot cover it.
 */
int  fmMeshSendText(const char *text);

uint8_t  fmMeshRelayQueueDepth();
uint32_t fmMeshFramesRelayed();
uint32_t fmMeshFramesSuppressed();
uint32_t fmMeshBackoffMs(float snr, bool isAlarm);
