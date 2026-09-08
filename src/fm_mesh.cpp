/**
 * FloodMesh - managed flooding, fragmentation, reassembly and priority.
 * See fm_mesh.h for the propagation model and why the delay bands are shaped
 * the way they are.
 *
 * THREADING: this module is single-threaded and driven entirely from
 * fmMeshLoop(). It calls fmRadioTransmit(), which CAN BLOCK for as long as the
 * CAD backoff runs (worst case a few seconds on a saturated channel). That
 * stalls whatever calls fmMeshLoop().
 *
 * Two consequences the caller must respect, both handled by the UI layer today:
 *   - Do not call fmMeshLoop() while the microphone is capturing. The I2S DMA
 *     holds only ~64 ms of audio, so a blocking transmit eats the recording.
 *     Use fmMeshInhibitTx(true) around capture; receive still works.
 *   - Button polling stops for the duration of a transmit. At 25 ms debounce a
 *     multi-second stall is user-visible.
 * The real fix is a dedicated radio task at higher priority than the UI, which
 * is the documented next refactor. It is deliberately not done here: getting
 * the protocol correct single-threaded first means a later concurrency bug can
 * be told apart from a protocol bug.
 */
#include "fm_mesh.h"

#include <esp_random.h>

#include "fm_airtime.h"
#include "fm_auth.h"
#include "fm_dedup.h"
#include "fm_ids.h"
#include "fm_radio.h"

// The clip geometry in fm_packet.h is derived from a hard-coded 10 s, while the
// recording cap is an overridable build flag. If they ever disagree the device
// records a length it cannot fragment, so make that a build failure.
#include "fm_input.h"
static_assert(FM_VOICE_MAX_MS == FM_VOICE_MAX_MS_,
              "recording cap (FM_VOICE_MAX_MS) and clip geometry "
              "(FM_VOICE_MAX_MS_ in fm_packet.h) must agree");

// ---------------------------------------------------------------- tunables
#ifndef FM_MESH_SLOT_MS
// Contention slot, derived the way Meshtastic derives it:
//   slot = max(2.25, NUM_SYM_CAD + 0.5) * (2^SF / BW) + 7.6 ms
// At SF7 / BW 125 kHz the symbol is 1.024 ms, so 2.5 * 1.024 + 7.6 = 10.16 ms.
// The 7.6 ms term is transceiver turnaround; the rest scales with the air rate.
#define FM_MESH_SLOT_MS 10
#endif

// A slot derived for one air rate is meaningless at another, so pin it.
static_assert(FM_LORA_SF == 7, "FM_MESH_SLOT_MS was derived for SF7 - recompute it");
static_assert(FM_TOA_BW_KHZ == 125, "FM_MESH_SLOT_MS was derived for BW125 - recompute it");

namespace {

// ---------------------------------------------------------------- relay queue
struct Relay {
  bool     used;
  uint8_t  len;
  uint32_t dueAt;
  uint8_t  frame[FM_FRAME_LEN_MAX];
  // Identity kept unpacked so suppression does not have to re-parse.
  char     callSign[FM_CALLSIGN_LEN + 1];
  uint8_t  msgId;
  uint8_t  fragIdx;
};

Relay g_relay[FM_MESH_RELAY_QUEUE];

// ---------------------------------------------------------------- reassembly
struct Reasm {
  bool     used;
  char     callSign[FM_CALLSIGN_LEN + 1];
  uint8_t  msgId;
  uint8_t  channel;
  uint8_t  got;                       // bitmask of received fragments
  uint8_t  count;
  uint8_t  hop;                       // hop limit seen on arrival; 0 = do not relay
  uint32_t firstMs;
  float    snr;
  float    rssi;
  uint8_t  clip[FM_CLIP_BYTES];
};

Reasm g_reasm[FM_MESH_REASM_SLOTS];

// ---------------------------------------------------------------- outbound
// One structure serves both cases: a note we originate and a note we relay for
// someone else. A relay keeps the ORIGINATOR's call sign and message id - only
// the hop limit changes - so the rest of the mesh still dedups it as one
// message rather than treating every hop as fresh traffic.
struct Burst {
  bool     active;
  uint8_t  next;                      // next fragment index to send
  uint8_t  channel;
  uint8_t  msgId;
  uint8_t  hop;                       // hop limit stamped on each fragment
  char     callSign[FM_CALLSIGN_LEN + 1];
  uint8_t  gotMask;                   // fragments we actually hold; a relay of
                                      // a partial note skips what it never got
  uint8_t  clip[FM_CLIP_BYTES];
};

Burst g_burst;

FmAlarmHandler g_onAlarm = nullptr;
FmVoiceHandler g_onVoice = nullptr;

bool     g_inhibitTx = false;
uint32_t g_relayed = 0;
uint32_t g_suppressed = 0;

// ---------------------------------------------------------------- helpers
uint32_t randBelow(uint32_t n) { return n ? (esp_random() % n) : 0; }

/** Meshtastic's integer map(), which is what the CW ladder is calibrated to. */
long mapLong(long x, long inMin, long inMax, long outMin, long outMax) {
  return (x - inMin) * (outMax - outMin) / (inMax - inMin) + outMin;
}

bool sameFrame(const Relay &r, const char *cs, uint8_t msgId, uint8_t frag) {
  return r.used && r.msgId == msgId && r.fragIdx == frag &&
         memcmp(r.callSign, cs, FM_CALLSIGN_LEN) == 0;
}

/** Queue a frame for rebroadcast after its contention delay. */
void queueRelay(const uint8_t *buf, size_t len, const FmHeader &h, uint8_t fragIdx,
                float snr, uint32_t now) {
  if (len > FM_FRAME_LEN_MAX) return;

  int slot = -1;
  for (uint8_t i = 0; i < FM_MESH_RELAY_QUEUE; i++) {
    if (!g_relay[i].used) { slot = i; break; }
  }
  if (slot < 0) {
    // Full. Drop the newcomer rather than an already-waiting frame: the
    // waiting one is closer to air and dropping it wastes the delay spent.
    // Voice fragments dominate this queue, and losing one is survivable.
    Serial.println("[MESH] relay queue full - frame dropped");
    return;
  }

  Relay &r = g_relay[slot];
  r.used = true;
  r.len = (uint8_t)len;
  memcpy(r.frame, buf, len);
  r.frame[FM_OFF_HOP] = (uint8_t)(h.hopLimit - 1);  // caller guarantees > 0
  snprintf(r.callSign, sizeof(r.callSign), "%s", h.callSign);
  r.msgId = h.msgId;
  r.fragIdx = fragIdx;
  r.dueAt = now + fmMeshBackoffMs(snr, h.type == FM_TYPE_ALARM);
}

/** Cancel a pending relay because somebody else already sent it. */
bool suppressRelay(const char *cs, uint8_t msgId, uint8_t frag) {
  for (uint8_t i = 0; i < FM_MESH_RELAY_QUEUE; i++) {
    if (sameFrame(g_relay[i], cs, msgId, frag)) {
      g_relay[i].used = false;
      g_suppressed++;
      return true;
    }
  }
  return false;
}

Reasm *findReasm(const char *cs, uint8_t msgId) {
  for (uint8_t i = 0; i < FM_MESH_REASM_SLOTS; i++) {
    if (g_reasm[i].used && g_reasm[i].msgId == msgId &&
        memcmp(g_reasm[i].callSign, cs, FM_CALLSIGN_LEN) == 0) {
      return &g_reasm[i];
    }
  }
  return nullptr;
}

/** Deliver whatever arrived and free the slot. */
void flushReasm(Reasm &r) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < FM_FRAG_COUNT; i++) {
    if (r.got & (1u << i)) n++;
  }
  if (n && g_onVoice) {
    FmVoiceRx rx;
    memset(&rx.header, 0, sizeof(rx.header));
    snprintf(rx.header.callSign, sizeof(rx.header.callSign), "%s", r.callSign);
    rx.header.type = FM_TYPE_VOICE;
    rx.header.channel = r.channel;
    rx.header.msgId = r.msgId;
    rx.clip = r.clip;
    rx.len = FM_CLIP_BYTES;
    rx.fragsReceived = n;
    rx.snr = r.snr;
    rx.rssi = r.rssi;
    g_onVoice(&rx);
  }

  // Relay the WHOLE note now that nothing more is inbound, rather than echoing
  // each fragment as it landed. The originator's call sign and message id are
  // preserved so the rest of the mesh still sees one message; only the hop
  // limit changes. Fragments we never received are skipped.
  if (n && r.hop > 0 && !g_burst.active) {
    memcpy(g_burst.clip, r.clip, FM_CLIP_BYTES);
    snprintf(g_burst.callSign, sizeof(g_burst.callSign), "%s", r.callSign);
    g_burst.channel = r.channel;
    g_burst.msgId   = r.msgId;
    g_burst.hop     = (uint8_t)(r.hop - 1);
    g_burst.gotMask = r.got;
    g_burst.next    = 0;
    g_burst.active  = true;
    Serial.printf("[MESH] relaying %s msg %u (%u/%u fragments, hop %u)\n",
                  r.callSign, (unsigned)r.msgId, n, (unsigned)FM_FRAG_COUNT,
                  (unsigned)g_burst.hop);
  }
  r.used = false;
}

Reasm *claimReasm(const char *cs, uint8_t msgId, uint8_t channel, uint32_t now) {
  for (uint8_t i = 0; i < FM_MESH_REASM_SLOTS; i++) {
    if (!g_reasm[i].used) {
      Reasm &r = g_reasm[i];
      r.used = true;
      snprintf(r.callSign, sizeof(r.callSign), "%s", cs);
      r.msgId = msgId;
      r.channel = channel;
      r.got = 0;
      r.count = 0;
      r.hop = 0;
      r.firstMs = now;
      memset(r.clip, 0, sizeof(r.clip));
      return &r;
    }
  }
  // All slots busy. Evict the oldest - a stalled partial clip must not be able
  // to lock out a new one, or one lost fragment mutes the node indefinitely.
  Reasm *oldest = &g_reasm[0];
  for (uint8_t i = 1; i < FM_MESH_REASM_SLOTS; i++) {
    if ((now - g_reasm[i].firstMs) > (now - oldest->firstMs)) oldest = &g_reasm[i];
  }
  flushReasm(*oldest);
  return claimReasm(cs, msgId, channel, now);
}

// ---------------------------------------------------------------- receive
void handleFrame(const uint8_t *buf, size_t len, float rssi, float snr, uint32_t now) {
  FmHeader h;
  if (!fmParseHeader(buf, len, &h)) return;   // malformed or hostile - already logged

  // Ignore our own frames coming back from a neighbour's relay.
  if (memcmp(h.callSign, fmCallSign(), FM_CALLSIGN_LEN) == 0) return;

  const uint8_t frag =
      (h.type == FM_TYPE_VOICE) ? fmFragIdx(buf[FM_OFF_FRAGINFO]) : 0;

  // Suppression comes FIRST. Hearing a frame we are still waiting to relay
  // means a peer beat us to it, so cancel ours. This must run before the dedup
  // check, because dedup will report the frame as already seen and return.
  if (suppressRelay(h.callSign, h.msgId, frag)) return;

  if (h.type == FM_TYPE_ALARM) {
    // AUTHENTICATE BEFORE DEDUP. The reverse order is cheaper - no HMAC on
    // duplicates - but it lets an attacker poison the dedup ring with forged
    // frames carrying a real node's call sign and message id, so that the
    // genuine alarm arrives and is discarded as a duplicate. Silencing an
    // EVACUATE broadcast is exactly the attack this PSK exists to stop, so the
    // HMAC cost is paid on every alarm. Alarms are 24 bytes and rare.
    const bool authOk = fmAuthVerifyAlarm(buf);
    if (!authOk) {
      Serial.printf("[MESH] alarm from %s FAILED authentication - dropped\n", h.callSign);
      return;
    }
    if (!fmReplayAccept(h.callSign, fmRd32(buf + FM_OFF_COUNTER))) {
      Serial.printf("[MESH] alarm from %s is a replay - dropped\n", h.callSign);
      return;
    }
    if (fmDedupSeenOrMark(h.callSign, h.msgId, 0)) return;

    if (g_onAlarm) {
      FmAlarmRx rx;
      rx.header = h;
      rx.alarmType = buf[FM_OFF_ALARMTYPE];
      rx.authenticated = true;
      rx.snr = snr;
      rx.rssi = rssi;
      g_onAlarm(&rx);
    }
    if (h.hopLimit > 0) queueRelay(buf, len, h, 0, snr, now);
    return;
  }

  // ---- voice ----
  // No MAC on voice, so dedup is the only gate and it goes first.
  if (fmDedupSeenOrMark(h.callSign, h.msgId, frag)) return;

  Reasm *r = findReasm(h.callSign, h.msgId);
  if (!r) r = claimReasm(h.callSign, h.msgId, h.channel, now);

  const uint16_t off = fmFragOffset(frag);
  const uint16_t n = fmFragBytes(frag);
  // fmParseHeader already proved len - FM_VOICE_HDR_LEN == n, but the copy
  // target is a fixed 1500-byte buffer and this is the last place to be sure.
  if ((size_t)off + n <= FM_CLIP_BYTES) {
    memcpy(r->clip + off, buf + FM_VOICE_HDR_LEN, n);
    if (!(r->got & (1u << frag))) r->count++;
    r->got |= (uint8_t)(1u << frag);
    r->snr = snr;
    r->rssi = rssi;
  }

  // NOTE: no per-fragment relay here, deliberately. Rebroadcasting fragment N
  // while fragment N+1 is still arriving makes the node deaf to its own inbound
  // burst - measured on hardware as 3 of 8 fragments received. The whole note is
  // relayed once, after reassembly, from flushReasm().
  if (h.hopLimit > r->hop) r->hop = h.hopLimit;
  if (r->count >= FM_FRAG_COUNT) flushReasm(*r);
}

/**
 * Launch one frame, charging the airtime ledger. Returns the radio status.
 *
 * fmRadioTransmit() is ASYNCHRONOUS: it starts the packet and returns while it
 * is still on the air. A caller offering the next frame too soon therefore gets
 * FM_RADIO_ERR_TX_ACTIVE, which means "not yet", NOT "failed". Treating it as a
 * failure aborted every voice note after its first fragment.
 */
int16_t sendNow(const uint8_t *buf, size_t len) {
  const uint32_t toa = fmToaMs((uint16_t)len);
  const int16_t st = fmRadioTransmit(buf, len);
  if (st == FM_RADIO_ERR_TX_ACTIVE) return st;      // still busy; retry next pass
  if (st != FM_RADIO_OK) {
    Serial.printf("[MESH] transmit failed, code %d\n", (int)st);
    return st;
  }
  fmAirtimeCharge(toa);
  return FM_RADIO_OK;
}

}  // namespace

// ---------------------------------------------------------------- API
uint32_t fmMeshBackoffMs(float snr, bool isAlarm) {
  long s = (long)snr;
  if (s < FM_MESH_SNR_MIN) s = FM_MESH_SNR_MIN;
  if (s > FM_MESH_SNR_MAX) s = FM_MESH_SNR_MAX;

  // Low SNR -> low exponent -> short delay -> the far node relays first and
  // carries the message furthest per hop. See the header for why.
  long cw = mapLong(s, FM_MESH_SNR_MIN, FM_MESH_SNR_MAX, FM_MESH_CW_MIN, FM_MESH_CW_MAX);
  if (cw < FM_MESH_CW_MIN) cw = FM_MESH_CW_MIN;
  if (cw > FM_MESH_CW_MAX) cw = FM_MESH_CW_MAX;

  // The band is what makes "Layer 1 never yields" structural: voice starts
  // after the widest possible alarm window, so the two can never interleave.
  const uint32_t band = isAlarm ? 0u
                                : (uint32_t)(2 * FM_MESH_CW_MAX) * FM_MESH_SLOT_MS;
  return band + randBelow(1u << (uint32_t)cw) * FM_MESH_SLOT_MS;
}

void fmMeshBegin() {
  memset(g_relay, 0, sizeof(g_relay));
  memset(g_reasm, 0, sizeof(g_reasm));
  memset(&g_burst, 0, sizeof(g_burst));
  g_inhibitTx = false;
  fmRadioStartReceive();
  Serial.printf("[MESH] ready, call sign %s, slot %u ms, alarm band 0-%u ms\n",
                fmCallSign(), (unsigned)FM_MESH_SLOT_MS,
                (unsigned)(2 * FM_MESH_CW_MAX * FM_MESH_SLOT_MS));
}

void fmMeshSetAlarmHandler(FmAlarmHandler cb) { g_onAlarm = cb; }
void fmMeshSetVoiceHandler(FmVoiceHandler cb) { g_onVoice = cb; }
void fmMeshInhibitTx(bool on) { g_inhibitTx = on; }

bool fmMeshSendAlarm(uint8_t alarmType, uint8_t channel) {
  if (alarmType >= FM_ALARM_COUNT) return false;

  // A Layer 1 alarm takes the radio away from whatever voice was using it.
  // SetStandby is a documented exit from TX on the SX1262, so this really does
  // cut in mid-packet rather than waiting out the current fragment.
  if (g_burst.active || fmRadioIsTransmitting()) {
    fmRadioAbortTx();
    if (g_burst.active) {
      Serial.printf("[MESH] voice burst ABORTED at fragment %u/%u for a Layer 1 alarm\n",
                    g_burst.next, (unsigned)FM_FRAG_COUNT);
      g_burst.active = false;
    }
  }

  uint8_t f[FM_ALARM_LEN];
  memset(f, 0, sizeof(f));
  fmWriteHeader(f, FM_TYPE_ALARM, FM_HOP_DEFAULT, fmCallSign(), channel, (uint8_t)fmNextMsgId());
  f[FM_OFF_ALARMTYPE] = alarmType;
  fmWr32(f + FM_OFF_COUNTER, fmNextAuthCounter());
  fmAuthSignAlarm(f);

  // Charged, never refused: the duty-cycle ledger records an alarm overrun
  // honestly rather than suppressing the alarm. See fm_airtime.h.
  const bool ok = (sendNow(f, sizeof(f)) == FM_RADIO_OK);
  return ok;
}

bool fmMeshSendVoice(const uint8_t *clip, size_t len, uint8_t channel) {
  if (!clip || len != FM_CLIP_BYTES) return false;
  if (g_burst.active) return false;

  // Eight fragments' worth of airtime, checked as a whole. Committing to half
  // a voice note and then running out of budget is worse than not starting.
  const uint32_t cost =
      (uint32_t)(FM_FRAG_COUNT - 1) * fmToaMs(FM_VOICE_HDR_LEN + FM_FRAG_PAYLOAD_MAX) +
      fmToaMs(FM_VOICE_HDR_LEN + FM_FRAG_LAST_BYTES);
  if (!fmAirtimeAllows(cost, false)) {
    Serial.printf("[MESH] voice REFUSED - duty cycle at %u permille, burst needs %lu ms\n",
                  fmAirtimePermille(), (unsigned long)cost);
    return false;
  }

  memcpy(g_burst.clip, clip, FM_CLIP_BYTES);
  snprintf(g_burst.callSign, sizeof(g_burst.callSign), "%s", fmCallSign());
  g_burst.channel = channel;
  g_burst.msgId   = (uint8_t)fmNextMsgId();
  g_burst.hop     = FM_HOP_DEFAULT;
  g_burst.gotMask = 0xFF;          // we originated it, so we hold every fragment
  g_burst.next    = 0;
  g_burst.active  = true;
  return true;
}

void fmMeshLoop(uint32_t now) {
  // --- receive ---------------------------------------------------------
  uint8_t buf[FM_FRAME_LEN_MAX];
  size_t len = 0;
  float rssi = 0.0f, snr = 0.0f;
  while (fmRadioPoll(buf, sizeof(buf), &len, &rssi, &snr)) {
    handleFrame(buf, len, rssi, snr, now);
  }

  // --- expire stalled reassemblies -------------------------------------
  for (uint8_t i = 0; i < FM_MESH_REASM_SLOTS; i++) {
    if (g_reasm[i].used && (now - g_reasm[i].firstMs) > FM_MESH_REASM_TTL_MS) {
      Serial.printf("[MESH] partial clip from %s: %u/%u fragments, delivering anyway\n",
                    g_reasm[i].callSign, g_reasm[i].count, (unsigned)FM_FRAG_COUNT);
      flushReasm(g_reasm[i]);
    }
  }

  if (g_inhibitTx) return;   // capture in progress: receive only

  // --- relays whose backoff has expired --------------------------------
  for (uint8_t i = 0; i < FM_MESH_RELAY_QUEUE; i++) {
    Relay &r = g_relay[i];
    if (!r.used || (int32_t)(now - r.dueAt) < 0) continue;
    r.used = false;
    const bool isAlarm = fmFrameType(r.frame) == FM_TYPE_ALARM;
    if (!fmAirtimeAllows(fmToaMs(r.len), isAlarm)) {
      // Relaying other people's voice is the first thing to go when the
      // regulatory budget runs low. Alarms are never refused.
      continue;
    }
    const int16_t st = sendNow(r.frame, r.len);
    if (st == FM_RADIO_ERR_TX_ACTIVE) {
      r.used = true;      // put it back; the radio is simply still busy
      return;
    }
    if (st == FM_RADIO_OK) {
      g_relayed++;
      // Relaying means transmitting, and transmitting means deaf. If these
      // lines interleave with an inbound burst, the node is talking over the
      // very fragments it is trying to collect.
      Serial.printf("[MESH] relayed %s msg %u frag %u\n", r.callSign,
                    (unsigned)r.msgId, (unsigned)r.fragIdx);
    }
    return;  // one transmit per pass, so receive keeps getting serviced
  }

  // --- voice burst, one fragment per pass -------------------------------
  // Serves both an originated note and a relayed one; g_burst carries the call
  // sign and hop limit so this loop does not need to know which it is.
  if (g_burst.active) {
    // Skip fragments we never received, which only happens when relaying a
    // partial note. Sending zeroed filler would be worse than a gap: the
    // receiver would decode it as sound.
    while (g_burst.next < FM_FRAG_COUNT &&
           !(g_burst.gotMask & (1u << g_burst.next))) {
      g_burst.next++;
    }
    if (g_burst.next >= FM_FRAG_COUNT) {
      g_burst.active = false;
      Serial.printf("[MESH] burst done: %lu ms airtime used, duty now %u permille\n",
                    (unsigned long)fmAirtimeUsedMs(), fmAirtimePermille());
      return;
    }

    const uint8_t idx = g_burst.next;
    const uint16_t n = fmFragBytes(idx);
    uint8_t f[FM_FRAME_LEN_MAX];
    fmWriteHeader(f, FM_TYPE_VOICE, g_burst.hop, g_burst.callSign, g_burst.channel,
                  g_burst.msgId);
    f[FM_OFF_FRAGINFO] = fmFragInfo(idx, FM_FRAG_COUNT);
    memcpy(f + FM_VOICE_HDR_LEN, g_burst.clip + fmFragOffset(idx), n);

    const int16_t st = sendNow(f, FM_VOICE_HDR_LEN + n);
    if (st == FM_RADIO_ERR_TX_ACTIVE) return;   // previous fragment still on air
    if (st != FM_RADIO_OK) {
      g_burst.active = false;                   // a real fault; do not spin on it
      return;
    }
    g_burst.next++;
  }
}

bool fmMeshBusy() { return g_burst.active || fmMeshRelayQueueDepth() > 0; }

uint8_t fmMeshRelayQueueDepth() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < FM_MESH_RELAY_QUEUE; i++) {
    if (g_relay[i].used) n++;
  }
  return n;
}

uint32_t fmMeshFramesRelayed() { return g_relayed; }
uint32_t fmMeshFramesSuppressed() { return g_suppressed; }
