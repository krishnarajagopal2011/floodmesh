/**
 * FloodMesh V4 - managed flooding for SOS, texts, ACKs and heartbeats. See fm_mesh.h.
 *
 * Derived from V3's fm_mesh.cpp: the relay queue, SNR-biased backoff and
 * suppression are unchanged. V4 adds the ACK and heartbeat frames, the
 * responder ACK race, the text-forwarding rule for battery units, a battery
 * term in the backoff, and a contention slot computed from the spreading
 * factor instead of being fixed for SF7. Single-threaded, driven from
 * fmMeshLoop(); a transmit can block for its CAD backoff, so key polling
 * pauses briefly while sending.
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
// Contention slot: 2.5 symbols + ~7.6 ms turnaround, rounded down, from the
// symbol time 2^SF / BW. SF7/BW125 gives 10 ms (V3's value), SF8 13, SF9 18,
// SF10 28.
#define FM_MESH_SLOT_MS \
  ((uint32_t)(((1UL << FM_LORA_SF) * 25UL) / (10UL * (uint32_t)FM_TOA_BW_KHZ)) + 8UL)
#endif
static_assert(FM_TOA_BW_KHZ == 125, "FM_MESH_SLOT_MS was checked for BW125 - recheck it");

// Heartbeats are refused once the hour's airtime passes this share of the
// budget, so they are always the first traffic to go quiet (§13.5).
#ifndef FM_HB_BUDGET_PERCENT
#define FM_HB_BUDGET_PERCENT 80
#endif

namespace {

const uint8_t kChannel = 0;   // the header's channel byte is unused; everything on 0

constexpr size_t kFrameMax =
    FM_TEXT_LEN_MAX > FM_ACK_LEN ? FM_TEXT_LEN_MAX : FM_ACK_LEN;
static_assert(kFrameMax >= FM_ALARM_LEN && kFrameMax >= FM_HEARTBEAT_LEN,
              "the relay slot must hold every frame type V4 forwards");

struct Relay {
  bool     used;
  uint8_t  len;
  uint32_t dueAt;
  uint8_t  frame[kFrameMax];
  char     callSign[FM_CALLSIGN_LEN + 1];
  uint8_t  msgId;
};

struct PendingAck {
  bool     used;
  uint8_t  kind;
  char     target[FM_CALLSIGN_LEN + 1];
  uint8_t  targetMsgId;
  uint32_t dueAt;
};

Relay      g_relay[FM_MESH_RELAY_QUEUE];
PendingAck g_ack[FM_MESH_ACK_QUEUE];

FmAlarmHandler     g_onAlarm = nullptr;
FmTextHandler      g_onText = nullptr;
FmAckHandler       g_onAck = nullptr;
FmHeartbeatHandler g_onHeartbeat = nullptr;
FmEchoHandler      g_onEcho = nullptr;

uint32_t g_relayed = 0;
uint32_t g_suppressed = 0;

bool     g_powered = false;
uint8_t  g_batt = 100;
bool     g_poweredHeard = false;   // a heartbeat was ever heard directly
uint32_t g_poweredAt = 0;

uint32_t randBelow(uint32_t n) { return n ? (esp_random() % n) : 0; }

long mapLong(long x, long inMin, long inMax, long outMin, long outMax) {
  return (x - inMin) * (outMax - outMin) / (inMax - inMin) + outMin;
}

uint8_t hopsTaken(uint8_t hopLimitNow) {
  return hopLimitNow >= FM_HOP_DEFAULT ? 0 : (uint8_t)(FM_HOP_DEFAULT - hopLimitNow);
}

/** SOS and ACK frames are the life-safety class: never refused for duty cycle. */
bool isAlarmClass(uint8_t type) { return type == FM_TYPE_ALARM || type == FM_TYPE_ACK; }

const char *typeName(uint8_t type) {
  switch (type) {
    case FM_TYPE_ALARM:     return "alarm";
    case FM_TYPE_TEXT:      return "text";
    case FM_TYPE_ACK:       return "ack";
    case FM_TYPE_HEARTBEAT: return "heartbeat";
    default:                return "frame";
  }
}

bool heartbeatAllowed(uint32_t toaMs) {
  const uint32_t cap = (uint32_t)(((uint64_t)FM_DUTY_BUDGET_MS * FM_HB_BUDGET_PERCENT) / 100u);
  return fmAirtimeUsedMs() + toaMs <= cap;
}

/** Space-pad a call sign to the 5 on-air bytes (no NUL written). */
void writeCallSign(uint8_t *dst, const char *cs) {
  char pad[FM_CALLSIGN_LEN + 1];
  snprintf(pad, sizeof(pad), "%-5.5s", cs ? cs : "");
  memcpy(dst, pad, FM_CALLSIGN_LEN);
}

void queueRelay(const uint8_t *buf, size_t len, const FmHeader &h, float snr, uint8_t prio,
                uint32_t now) {
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
  r.dueAt = now + fmMeshBackoffMs(snr, prio);
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

/**
 * Another responder answered this SOS: drop our own queued ACK for it. A
 * "help coming" also stands in for "delivered", since it proves delivery.
 */
void cancelQueuedAck(uint8_t heardKind, const char *target, uint8_t targetMsgId) {
  for (uint8_t i = 0; i < FM_MESH_ACK_QUEUE; i++) {
    PendingAck &a = g_ack[i];
    if (!a.used || a.targetMsgId != targetMsgId) continue;
    if (memcmp(a.target, target, FM_CALLSIGN_LEN) != 0) continue;
    if (heardKind == a.kind || heardKind == FM_ACK_HELP) {
      a.used = false;
      Serial.printf("[MESH] queued ack for %s msg %u dropped - already answered\n", target,
                    (unsigned)targetMsgId);
    }
  }
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

/** Wait for the radio rather than cutting off anything in flight. */
int16_t sendWaiting(const uint8_t *buf, size_t len) {
  for (uint8_t tries = 0; tries < 50; tries++) {
    const int16_t st = sendNow(buf, len);
    if (st != FM_RADIO_ERR_TX_ACTIVE) return st;
    delay(20);
  }
  return FM_RADIO_ERR_TX_ACTIVE;
}

size_t buildAck(uint8_t *f, uint8_t kind, const char *target, uint8_t targetMsgId, uint8_t *idOut) {
  memset(f, 0, FM_ACK_LEN);
  const uint8_t id = (uint8_t)fmNextMsgId();
  fmWriteHeader(f, FM_TYPE_ACK, FM_HOP_DEFAULT, fmCallSign(), kChannel, id);
  f[FM_OFF_ACKKIND] = kind;
  writeCallSign(f + FM_OFF_ACKTARGET, target);
  f[FM_OFF_ACKMSGID] = targetMsgId;
  fmWr32(f + FM_OFF_ACOUNTER, fmNextAuthCounter());
  fmAuthSignFrame(f, FM_ACK_LEN);
  if (idOut) *idOut = id;
  return FM_ACK_LEN;
}

void handleFrame(const uint8_t *buf, size_t len, float rssi, float snr, uint32_t now) {
  FmHeader h;
  if (!fmParseHeader(buf, len, &h)) return;
  if (h.type == FM_TYPE_VOICE) return;   // V4 carries no voice

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
  // and silence the genuine one. Byte 0 (type) is inside the MAC, so a frame
  // signed as one type never verifies as another.
  const bool authOk = h.type == FM_TYPE_ALARM ? fmAuthVerifyAlarm(buf) : fmAuthVerifyFrame(buf, len);
  if (!authOk) {
    Serial.printf("[MESH] %s from %s FAILED authentication - dropped\n", typeName(h.type),
                  h.callSign);
    return;
  }
  const uint32_t counter = fmRd32(buf + fmCounterOffset(h.type));
  if (!fmReplayAccept(h.callSign, counter)) {
    Serial.printf("[MESH] %s from %s is a replay - dropped\n", typeName(h.type), h.callSign);
    return;
  }
  if (fmDedupSeenOrMark(h.callSign, h.msgId, 0)) return;

  const uint8_t hops = hopsTaken(h.hopLimit);
  bool relay = h.hopLimit > 0;
  uint8_t prio = FM_PRIO_SOS;

  switch (h.type) {
    case FM_TYPE_ALARM:
      if (g_onAlarm) {
        FmAlarmRx rx;
        rx.header = h;
        rx.alarmType = buf[FM_OFF_ALARMTYPE];
        rx.hops = hops;
        rx.snr = snr;
        rx.rssi = rssi;
        g_onAlarm(&rx);
      }
      break;

    case FM_TYPE_TEXT:
      prio = FM_PRIO_TEXT;
      if (g_onText) {
        FmTextRx rx;
        rx.header = h;
        fmTextUnpack(buf + FM_OFF_TEXT, buf[FM_OFF_TEXTLEN], rx.text);
        rx.hops = hops;
        rx.snr = snr;
        rx.rssi = rssi;
        g_onText(&rx);
      }
      // §13.3: a battery unit leaves texts to the powered unit it can hear.
      if (!g_powered && fmMeshPoweredNearby(now)) relay = false;
      break;

    case FM_TYPE_ACK: {
      FmAckRx rx;
      rx.header = h;
      rx.kind = buf[FM_OFF_ACKKIND];
      memcpy(rx.target, buf + FM_OFF_ACKTARGET, FM_CALLSIGN_LEN);
      rx.target[FM_CALLSIGN_LEN] = '\0';
      rx.targetMsgId = buf[FM_OFF_ACKMSGID];
      rx.hops = hops;
      rx.snr = snr;
      rx.rssi = rssi;
      cancelQueuedAck(rx.kind, rx.target, rx.targetMsgId);
      if (g_onAck) g_onAck(&rx);
      // The unit it is addressed to is the end of the line.
      if (memcmp(rx.target, fmCallSign(), FM_CALLSIGN_LEN) == 0) relay = false;
      break;
    }

    case FM_TYPE_HEARTBEAT: {
      prio = FM_PRIO_HEARTBEAT;
      if (hops == 0) {
        g_poweredHeard = true;
        g_poweredAt = now;
      }
      if (g_onHeartbeat) {
        FmHeartbeatRx rx;
        rx.header = h;
        rx.battPct = buf[FM_OFF_HBBATT];
        rx.flags = buf[FM_OFF_HBFLAGS];
        rx.hops = hops;
        rx.snr = snr;
        rx.rssi = rssi;
        g_onHeartbeat(&rx);
      }
      break;
    }

    default:
      return;
  }
  if (relay) queueRelay(buf, len, h, snr, prio, now);
}

/** Send one due ACK. Returns true if the radio was used (one transmit per pass). */
bool serviceAcks(uint32_t now) {
  for (uint8_t i = 0; i < FM_MESH_ACK_QUEUE; i++) {
    PendingAck &a = g_ack[i];
    if (!a.used || (int32_t)(now - a.dueAt) < 0) continue;
    uint8_t f[FM_ACK_LEN];
    uint8_t id = 0;
    buildAck(f, a.kind, a.target, a.targetMsgId, &id);
    const int16_t st = sendNow(f, sizeof(f));
    if (st == FM_RADIO_ERR_TX_ACTIVE) return true;   // keep it; try next pass
    a.used = false;
    if (st == FM_RADIO_OK) {
      Serial.printf("[MESH] ack %s -> %s msg %u (our msg %u)\n",
                    a.kind == FM_ACK_HELP ? "HELP" : "DELIVERED", a.target,
                    (unsigned)a.targetMsgId, (unsigned)id);
    }
    return true;
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------- API
uint32_t fmMeshBackoffMs(float snr, uint8_t prio) {
  long s = (long)snr;
  if (s < FM_MESH_SNR_MIN) s = FM_MESH_SNR_MIN;
  if (s > FM_MESH_SNR_MAX) s = FM_MESH_SNR_MAX;
  long cw = mapLong(s, FM_MESH_SNR_MIN, FM_MESH_SNR_MAX, FM_MESH_CW_MIN, FM_MESH_CW_MAX);
  if (cw < FM_MESH_CW_MIN) cw = FM_MESH_CW_MIN;
  if (cw > FM_MESH_CW_MAX) cw = FM_MESH_CW_MAX;
  const uint32_t band = (uint32_t)prio * (uint32_t)(2 * FM_MESH_CW_MAX) * FM_MESH_SLOT_MS;
  // §4.4: each unit weighs its own battery; external power counts as full.
  const uint32_t eff = g_powered ? 100u : (g_batt > 100 ? 100u : g_batt);
  const uint32_t batt = (100u - eff) * (uint32_t)FM_MESH_BATT_SLOTS * FM_MESH_SLOT_MS / 100u;
  return band + batt + randBelow(1u << (uint32_t)cw) * FM_MESH_SLOT_MS;
}

void fmMeshBegin() {
  memset(g_relay, 0, sizeof(g_relay));
  memset(g_ack, 0, sizeof(g_ack));
  fmRadioStartReceive();
  Serial.printf("[MESH] ready, call sign %s, hop limit %u, SF%u, slot %lu ms\n", fmCallSign(),
                (unsigned)FM_HOP_DEFAULT, (unsigned)FM_LORA_SF, (unsigned long)FM_MESH_SLOT_MS);
}

void fmMeshSetAlarmHandler(FmAlarmHandler cb) { g_onAlarm = cb; }
void fmMeshSetTextHandler(FmTextHandler cb) { g_onText = cb; }
void fmMeshSetAckHandler(FmAckHandler cb) { g_onAck = cb; }
void fmMeshSetHeartbeatHandler(FmHeartbeatHandler cb) { g_onHeartbeat = cb; }
void fmMeshSetEchoHandler(FmEchoHandler cb) { g_onEcho = cb; }

void fmMeshSetPowered(bool powered) { g_powered = powered; }
void fmMeshSetBattery(uint8_t pct) { g_batt = pct; }

bool fmMeshPoweredNearby(uint32_t now) {
  return g_poweredHeard && (now - g_poweredAt) < FM_MESH_POWERED_NEARBY_MS;
}

uint32_t fmMeshPoweredNearbyAgeMs(uint32_t now) {
  return g_poweredHeard ? (now - g_poweredAt) : UINT32_MAX;
}

int fmMeshSendAlarm(uint8_t alarmType) {
  if (alarmType >= FM_ALARM_COUNT) return -1;
  if (fmRadioIsTransmitting()) fmRadioAbortTx();   // an SOS never waits
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
  return sendWaiting(f, len) == FM_RADIO_OK ? id : -1;
}

int fmMeshSendAck(uint8_t kind, const char *target, uint8_t targetMsgId) {
  if (kind > FM_ACK_HELP || target == nullptr) return -1;
  uint8_t f[FM_ACK_LEN];
  uint8_t id = 0;
  buildAck(f, kind, target, targetMsgId, &id);
  // Answering it ourselves makes any queued ACK for the same SOS redundant.
  char pad[FM_CALLSIGN_LEN + 1];
  snprintf(pad, sizeof(pad), "%-5.5s", target);
  cancelQueuedAck(kind, pad, targetMsgId);
  return sendWaiting(f, sizeof(f)) == FM_RADIO_OK ? id : -1;
}

void fmMeshQueueAck(uint8_t kind, const char *target, uint8_t targetMsgId, float snr) {
  if (kind > FM_ACK_HELP || target == nullptr) return;
  char pad[FM_CALLSIGN_LEN + 1];
  snprintf(pad, sizeof(pad), "%-5.5s", target);
  int slot = -1;
  for (uint8_t i = 0; i < FM_MESH_ACK_QUEUE; i++) {
    PendingAck &a = g_ack[i];
    if (a.used && a.kind == kind && a.targetMsgId == targetMsgId &&
        memcmp(a.target, pad, FM_CALLSIGN_LEN) == 0) {
      return;   // already queued
    }
    if (slot < 0 && !a.used) slot = i;
  }
  if (slot < 0) {
    Serial.println("[MESH] ack queue full - ack dropped");
    return;
  }
  PendingAck &a = g_ack[slot];
  a.used = true;
  a.kind = kind;
  memcpy(a.target, pad, sizeof(a.target));
  a.targetMsgId = targetMsgId;
  a.dueAt = millis() + fmMeshBackoffMs(snr, FM_PRIO_SOS);
}

int fmMeshSendHeartbeat(uint8_t battPct) {
  uint8_t f[FM_HEARTBEAT_LEN];
  memset(f, 0, sizeof(f));
  if (!heartbeatAllowed(fmToaMs(sizeof(f)))) {
    Serial.printf("[MESH] heartbeat skipped - airtime at %u permille\n", fmAirtimePermille());
    return -1;
  }
  const uint8_t id = (uint8_t)fmNextMsgId();
  fmWriteHeader(f, FM_TYPE_HEARTBEAT, FM_HOP_DEFAULT, fmCallSign(), kChannel, id);
  f[FM_OFF_HBBATT] = battPct;
  f[FM_OFF_HBFLAGS] = FM_HB_FLAG_POWERED;
  fmWr32(f + FM_OFF_HCOUNTER, fmNextAuthCounter());
  fmAuthSignFrame(f, sizeof(f));
  return sendWaiting(f, sizeof(f)) == FM_RADIO_OK ? id : -1;
}

void fmMeshLoop(uint32_t now) {
  uint8_t buf[FM_FRAME_LEN_MAX];
  size_t len = 0;
  float rssi = 0.0f, snr = 0.0f;
  while (fmRadioPoll(buf, sizeof(buf), &len, &rssi, &snr)) {
    handleFrame(buf, len, rssi, snr, now);
  }

  if (serviceAcks(now)) return;   // ACKs go before relays; one transmit per pass

  for (uint8_t i = 0; i < FM_MESH_RELAY_QUEUE; i++) {
    Relay &r = g_relay[i];
    if (!r.used || (int32_t)(now - r.dueAt) < 0) continue;
    r.used = false;
    const uint8_t type = fmFrameType(r.frame);
    const uint32_t toa = fmToaMs(r.len);
    const bool ok = type == FM_TYPE_HEARTBEAT ? heartbeatAllowed(toa)
                                              : fmAirtimeAllows(toa, isAlarmClass(type));
    if (!ok) continue;   // texts and heartbeats go first when tight
    const int16_t st = sendNow(r.frame, r.len);
    if (st == FM_RADIO_ERR_TX_ACTIVE) {
      r.used = true;
      return;
    }
    if (st == FM_RADIO_OK) {
      g_relayed++;
      Serial.printf("[MESH] relayed %s %s msg %u (hop left %u)\n", typeName(type), r.callSign,
                    (unsigned)r.msgId, (unsigned)r.frame[FM_OFF_HOP]);
    }
    return;   // one transmit per pass
  }
}

bool fmMeshBusy() {
  if (fmMeshRelayQueueDepth() > 0) return true;
  for (uint8_t i = 0; i < FM_MESH_ACK_QUEUE; i++) {
    if (g_ack[i].used) return true;
  }
  return fmRadioIsTransmitting();
}

uint8_t fmMeshRelayQueueDepth() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < FM_MESH_RELAY_QUEUE; i++) n += g_relay[i].used ? 1 : 0;
  return n;
}
uint32_t fmMeshFramesRelayed() { return g_relayed; }
uint32_t fmMeshFramesSuppressed() { return g_suppressed; }
