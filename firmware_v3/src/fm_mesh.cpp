/**
 * FloodMesh V3 - managed flooding for alarms and texts. See fm_mesh.h.
 *
 * Derived from the voice-era fm_mesh.cpp: the relay queue, SNR-biased backoff
 * and suppression are unchanged; fragmentation and reassembly are gone, and
 * text frames are added. Single-threaded, driven from fmMeshLoop(); a transmit
 * can block for its CAD backoff, so key polling pauses briefly while sending.
 */
#include "fm_mesh.h"

#include <esp_random.h>

#include "fm_airtime.h"
#include "fm_alarm.h"
#include "fm_auth.h"
#include "fm_dedup.h"
#include "fm_ids.h"
#include "fm_radio.h"
#include "fm_text.h"

#ifndef FM_MESH_SLOT_MS
// Contention slot at SF7 / BW125: 2.5 symbols (1.024 ms each) + 7.6 ms
// turnaround = 10.16 ms. Recompute if the air rate changes.
#define FM_MESH_SLOT_MS 10
#endif
static_assert(FM_LORA_SF == 7, "FM_MESH_SLOT_MS was derived for SF7 - recompute it");
static_assert(FM_TOA_BW_KHZ == 125, "FM_MESH_SLOT_MS was derived for BW125 - recompute it");

namespace {

const uint8_t kChannel = 0;   // V3 has no channels in the UI; everything on 0

struct Relay {
  bool     used;
  uint8_t  len;
  uint32_t dueAt;
  uint8_t  frame[FM_TEXT_LEN_MAX > FM_ALARM_LEN ? FM_TEXT_LEN_MAX : FM_ALARM_LEN];
  char     callSign[FM_CALLSIGN_LEN + 1];
  uint8_t  msgId;
};

Relay g_relay[FM_MESH_RELAY_QUEUE];

FmAlarmHandler g_onAlarm = nullptr;
FmTextHandler  g_onText = nullptr;
FmEchoHandler  g_onEcho = nullptr;

uint32_t g_relayed = 0;
uint32_t g_suppressed = 0;

uint32_t randBelow(uint32_t n) { return n ? (esp_random() % n) : 0; }

long mapLong(long x, long inMin, long inMax, long outMin, long outMax) {
  return (x - inMin) * (outMax - outMin) / (inMax - inMin) + outMin;
}

uint8_t hopsTaken(uint8_t hopLimitNow) {
  return hopLimitNow >= FM_HOP_DEFAULT ? 0 : (uint8_t)(FM_HOP_DEFAULT - hopLimitNow);
}

void queueRelay(const uint8_t *buf, size_t len, const FmHeader &h, float snr, uint32_t now) {
  if (len > sizeof(g_relay[0].frame)) return;
  int slot = -1;
  for (uint8_t i = 0; i < FM_MESH_RELAY_QUEUE; i++) {
    if (!g_relay[i].used) { slot = i; break; }
  }
  if (slot < 0) {
    Serial.println("[MESH] relay queue full - frame dropped");
    return;
  }
  Relay &r = g_relay[slot];
  r.used = true;
  r.len = (uint8_t)len;
  memcpy(r.frame, buf, len);
  r.frame[FM_OFF_HOP] = (uint8_t)(h.hopLimit - 1);   // caller guarantees > 0
  snprintf(r.callSign, sizeof(r.callSign), "%s", h.callSign);
  r.msgId = h.msgId;
  r.dueAt = now + fmMeshBackoffMs(snr, h.type == FM_TYPE_ALARM);
}

bool suppressRelay(const char *cs, uint8_t msgId) {
  for (uint8_t i = 0; i < FM_MESH_RELAY_QUEUE; i++) {
    Relay &r = g_relay[i];
    if (r.used && r.msgId == msgId && memcmp(r.callSign, cs, FM_CALLSIGN_LEN) == 0) {
      r.used = false;
      g_suppressed++;
      return true;
    }
  }
  return false;
}

int16_t sendNow(const uint8_t *buf, size_t len) {
  const uint32_t toa = fmToaMs((uint16_t)len);
  const int16_t st = fmRadioTransmit(buf, len);
  if (st == FM_RADIO_ERR_TX_ACTIVE) return st;
  if (st != FM_RADIO_OK) {
    Serial.printf("[MESH] transmit failed, code %d\n", (int)st);
    return st;
  }
  fmAirtimeCharge(toa);
  return FM_RADIO_OK;
}

void handleFrame(const uint8_t *buf, size_t len, float rssi, float snr, uint32_t now) {
  FmHeader h;
  if (!fmParseHeader(buf, len, &h)) return;
  if (h.type == FM_TYPE_VOICE) return;   // V3 carries no voice

  // Our own frame coming back = a neighbour relayed it. Report once, then drop.
  if (memcmp(h.callSign, fmCallSign(), FM_CALLSIGN_LEN) == 0) {
    if (g_onEcho) {
      FmEchoRx e = {h.type, h.msgId, rssi, snr};
      g_onEcho(&e);
    }
    return;
  }

  // Suppression first: hearing a frame we are waiting to relay means a peer
  // got there first. Must run before dedup, which would return early.
  if (suppressRelay(h.callSign, h.msgId)) return;

  // Authenticate before dedup, so forged frames cannot poison the dedup ring
  // and silence the genuine one.
  const bool isAlarm = (h.type == FM_TYPE_ALARM);
  const bool authOk = isAlarm ? fmAuthVerifyAlarm(buf) : fmAuthVerifyFrame(buf, len);
  if (!authOk) {
    Serial.printf("[MESH] %s from %s FAILED authentication - dropped\n",
                  isAlarm ? "alarm" : "text", h.callSign);
    return;
  }
  const uint32_t counter = fmRd32(buf + (isAlarm ? FM_OFF_COUNTER : FM_OFF_TCOUNTER));
  if (!fmReplayAccept(h.callSign, counter)) {
    Serial.printf("[MESH] %s from %s is a replay - dropped\n", isAlarm ? "alarm" : "text",
                  h.callSign);
    return;
  }
  if (fmDedupSeenOrMark(h.callSign, h.msgId, 0)) return;

  if (isAlarm) {
    if (g_onAlarm) {
      FmAlarmRx rx;
      rx.header = h;
      rx.alarmType = buf[FM_OFF_ALARMTYPE];
      rx.hops = hopsTaken(h.hopLimit);
      rx.snr = snr;
      rx.rssi = rssi;
      g_onAlarm(&rx);
    }
  } else {
    if (g_onText) {
      FmTextRx rx;
      rx.header = h;
      fmTextUnpack(buf + FM_OFF_TEXT, buf[FM_OFF_TEXTLEN], rx.text);
      rx.hops = hopsTaken(h.hopLimit);
      rx.snr = snr;
      rx.rssi = rssi;
      g_onText(&rx);
    }
  }
  if (h.hopLimit > 0) queueRelay(buf, len, h, snr, now);
}

}  // namespace

// ---------------------------------------------------------------- API
uint32_t fmMeshBackoffMs(float snr, bool isAlarm) {
  long s = (long)snr;
  if (s < FM_MESH_SNR_MIN) s = FM_MESH_SNR_MIN;
  if (s > FM_MESH_SNR_MAX) s = FM_MESH_SNR_MAX;
  long cw = mapLong(s, FM_MESH_SNR_MIN, FM_MESH_SNR_MAX, FM_MESH_CW_MIN, FM_MESH_CW_MAX);
  if (cw < FM_MESH_CW_MIN) cw = FM_MESH_CW_MIN;
  if (cw > FM_MESH_CW_MAX) cw = FM_MESH_CW_MAX;
  const uint32_t band = isAlarm ? 0u : (uint32_t)(2 * FM_MESH_CW_MAX) * FM_MESH_SLOT_MS;
  return band + randBelow(1u << (uint32_t)cw) * FM_MESH_SLOT_MS;
}

void fmMeshBegin() {
  memset(g_relay, 0, sizeof(g_relay));
  fmRadioStartReceive();
  Serial.printf("[MESH] ready, call sign %s, hop limit %u, slot %u ms\n", fmCallSign(),
                (unsigned)FM_HOP_DEFAULT, (unsigned)FM_MESH_SLOT_MS);
}

void fmMeshSetAlarmHandler(FmAlarmHandler cb) { g_onAlarm = cb; }
void fmMeshSetTextHandler(FmTextHandler cb) { g_onText = cb; }
void fmMeshSetEchoHandler(FmEchoHandler cb) { g_onEcho = cb; }

int fmMeshSendAlarm(uint8_t alarmType) {
  if (alarmType >= FM_ALARM_COUNT) return -1;
  if (fmRadioIsTransmitting()) fmRadioAbortTx();   // an alarm never waits
  const uint8_t id = (uint8_t)fmNextMsgId();
  uint8_t f[FM_ALARM_LEN];
  memset(f, 0, sizeof(f));
  fmWriteHeader(f, FM_TYPE_ALARM, FM_HOP_DEFAULT, fmCallSign(), kChannel, id);
  f[FM_OFF_ALARMTYPE] = alarmType;
  fmWr32(f + FM_OFF_COUNTER, fmNextAuthCounter());
  fmAuthSignAlarm(f);
  return sendNow(f, sizeof(f)) == FM_RADIO_OK ? id : -1;
}

int fmMeshSendText(const char *text) {
  uint8_t f[FM_TEXT_LEN_MAX];
  memset(f, 0, sizeof(f));
  uint8_t chars = 0;
  const size_t packed = fmTextPack(text, f + FM_OFF_TEXT, &chars);
  if (chars == 0) return -1;
  const size_t len = FM_OFF_TEXT + packed + FM_MAC_LEN;
  if (!fmAirtimeAllows(fmToaMs((uint16_t)len), false)) {
    Serial.printf("[MESH] text REFUSED - duty cycle at %u permille\n", fmAirtimePermille());
    return -1;
  }
  const uint8_t id = (uint8_t)fmNextMsgId();
  fmWriteHeader(f, FM_TYPE_TEXT, FM_HOP_DEFAULT, fmCallSign(), kChannel, id);
  f[FM_OFF_TEXTLEN] = chars;
  fmWr32(f + FM_OFF_TCOUNTER, fmNextAuthCounter());
  fmAuthSignFrame(f, len);
  // A text waits for the radio rather than cutting off anything in flight.
  for (uint8_t tries = 0; tries < 50; tries++) {
    const int16_t st = sendNow(f, len);
    if (st == FM_RADIO_OK) return id;
    if (st != FM_RADIO_ERR_TX_ACTIVE) return -1;
    delay(20);
  }
  return -1;
}

void fmMeshLoop(uint32_t now) {
  uint8_t buf[FM_FRAME_LEN_MAX];
  size_t len = 0;
  float rssi = 0.0f, snr = 0.0f;
  while (fmRadioPoll(buf, sizeof(buf), &len, &rssi, &snr)) {
    handleFrame(buf, len, rssi, snr, now);
  }

  for (uint8_t i = 0; i < FM_MESH_RELAY_QUEUE; i++) {
    Relay &r = g_relay[i];
    if (!r.used || (int32_t)(now - r.dueAt) < 0) continue;
    r.used = false;
    const bool isAlarm = fmFrameType(r.frame) == FM_TYPE_ALARM;
    if (!fmAirtimeAllows(fmToaMs(r.len), isAlarm)) continue;   // texts go first when tight
    const int16_t st = sendNow(r.frame, r.len);
    if (st == FM_RADIO_ERR_TX_ACTIVE) {
      r.used = true;
      return;
    }
    if (st == FM_RADIO_OK) {
      g_relayed++;
      Serial.printf("[MESH] relayed %s %s msg %u (hop left %u)\n", isAlarm ? "alarm" : "text",
                    r.callSign, (unsigned)r.msgId, (unsigned)r.frame[FM_OFF_HOP]);
    }
    return;   // one transmit per pass
  }
}

uint8_t fmMeshRelayQueueDepth() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < FM_MESH_RELAY_QUEUE; i++) n += g_relay[i].used ? 1 : 0;
  return n;
}
uint32_t fmMeshFramesRelayed() { return g_relayed; }
uint32_t fmMeshFramesSuppressed() { return g_suppressed; }
