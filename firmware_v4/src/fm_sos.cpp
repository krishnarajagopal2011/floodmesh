/**
 * FloodMesh V4 - SOS sender retries and the responder SOS list. See fm_sos.h.
 */
#include "fm_sos.h"

#include <Preferences.h>

namespace {

// ---------------------------------------------------------------- sender
const uint8_t kIdRing = 8;   // msgIds of the current episode an ACK may name

FmSosState g_state = FM_SOS_IDLE;
uint8_t    g_ch = FM_SOS_GENERAL;
uint8_t    g_tries = 0;
uint32_t   g_start = 0;
uint32_t   g_next = 0;
bool       g_final = false;   // the last retry is out; g_next is the "not delivered" deadline
uint8_t    g_ids[kIdRing];
uint8_t    g_nIds = 0;
char       g_by[FM_CALLSIGN_LEN + 1] = "";

bool episodeHasId(uint8_t id) {
  const uint8_t n = g_nIds < kIdRing ? g_nIds : kIdRing;
  for (uint8_t i = 0; i < n; i++) {
    if (g_ids[i] == id) return true;
  }
  return false;
}

// ---------------------------------------------------------------- responder
const char *kNvsNs = "fmsos";
const char *kNvsMask = "mask";
const uint8_t kAllChannels = (uint8_t)((1u << FM_SOS_CH_COUNT) - 1u);

FmSosEntry g_tab[FM_SOS_TABLE];
uint8_t    g_mask = kAllChannels;

int findEntry(const char *cs) {
  for (uint8_t i = 0; i < FM_SOS_TABLE; i++) {
    if (g_tab[i].used && memcmp(g_tab[i].cs, cs, FM_CALLSIGN_LEN) == 0) return i;
  }
  return -1;
}

void saveMask() {
  Preferences p;
  if (p.begin(kNvsNs, false)) {
    p.putUChar(kNvsMask, g_mask);
    p.end();
  }
}

}  // namespace

// ================================================================ sender
void fmSosStart(uint8_t ch, uint32_t now) {
  g_state = FM_SOS_WAITING;
  g_ch = ch < FM_SOS_CH_COUNT ? ch : (uint8_t)FM_SOS_GENERAL;
  g_tries = 0;
  g_start = now;
  g_next = now;
  g_final = false;
  g_nIds = 0;
  g_by[0] = '\0';
}

bool fmSosDue(uint32_t now) {
  if (g_state == FM_SOS_DELIVERED) return (int32_t)(now - g_next) >= 0;   // keep-alive
  if (g_state != FM_SOS_WAITING || (int32_t)(now - g_next) < 0) return false;
  if (g_final) {
    g_state = FM_SOS_NOT_DELIVERED;
    Serial.printf("[SOS] no answer after %u retries - NOT DELIVERED\n",
                  (unsigned)FM_SOS_RETRY_MAX);
    return false;
  }
  return true;
}

void fmSosSent(int msgId, uint32_t now) {
  if (g_state != FM_SOS_WAITING && g_state != FM_SOS_DELIVERED) return;
  if (msgId < 0) {
    g_next = now + 30000UL;   // radio busy or down: try again soon, not in minutes
    return;
  }
  // Remember it: a "Help is coming" may name this newest frame.
  g_ids[g_nIds % kIdRing] = (uint8_t)msgId;
  g_nIds++;
  if (g_state == FM_SOS_DELIVERED) {
    g_next = now + FM_SOS_KEEPALIVE_MS;
    return;
  }
  g_tries++;
  if (g_tries > FM_SOS_RETRY_MAX) {
    // That was the last retry: wait for its answer, then give up.
    g_final = true;
    g_next = now + FM_SOS_ACK_WAIT_MS;
    return;
  }
  // Gap before retry n: 1, 2, 4, 8, then 15 min.
  const uint8_t shift = g_tries - 1 < 4 ? (uint8_t)(g_tries - 1) : 4;
  uint32_t gap = 60000UL << shift;
  if (gap > FM_SOS_RETRY_MAX_GAP_MS) gap = FM_SOS_RETRY_MAX_GAP_MS;
  g_next = now + gap;
}

bool fmSosOnAck(uint8_t kind, uint8_t targetMsgId, const char *by) {
  if (g_state == FM_SOS_IDLE || g_state == FM_SOS_STOPPED) return false;
  if (!episodeHasId(targetMsgId)) return false;
  if (kind == FM_ACK_HELP) {
    if (g_state == FM_SOS_HELP) return false;
    g_state = FM_SOS_HELP;
  } else {
    // A late answer after "not delivered" still counts; a second "delivered" does not.
    if (g_state != FM_SOS_WAITING && g_state != FM_SOS_NOT_DELIVERED) return false;
    g_state = FM_SOS_DELIVERED;
    g_next = millis() + FM_SOS_KEEPALIVE_MS;   // keep the SOS alive until HELP IS COMING
  }
  snprintf(g_by, sizeof(g_by), "%s", by ? by : "");
  return true;
}

void fmSosStop() {
  if (g_state != FM_SOS_IDLE) g_state = FM_SOS_STOPPED;
}

FmSosState  fmSosState() { return g_state; }
uint8_t     fmSosChannel() { return g_ch; }
uint8_t     fmSosTries() { return g_tries; }
const char *fmSosAnsweredBy() { return g_by; }

uint32_t fmSosNextInMs(uint32_t now) {
  // Waiting: the next retry, or after the last one the deadline. Delivered:
  // the next keep-alive.
  if (g_state != FM_SOS_WAITING && g_state != FM_SOS_DELIVERED) return UINT32_MAX;
  return (int32_t)(g_next - now) > 0 ? (g_next - now) : 0;
}

bool fmSosActive() {
  return g_state == FM_SOS_WAITING || g_state == FM_SOS_DELIVERED || g_state == FM_SOS_HELP;
}

// ================================================================ responder
void fmSosBegin() {
  memset(g_tab, 0, sizeof(g_tab));
  Preferences p;
  if (p.begin(kNvsNs, true)) {
    g_mask = p.getUChar(kNvsMask, kAllChannels);
    p.end();
  }
  g_mask = (uint8_t)((g_mask & kAllChannels) | (1u << FM_SOS_GENERAL));
}

int fmSosRxAdd(const char *cs, uint8_t ch, uint8_t msgId, uint8_t hops, float rssi, float snr,
               uint32_t now, bool *isNew) {
  if (ch >= FM_SOS_CH_COUNT) ch = FM_SOS_GENERAL;
  bool fresh = false;
  int idx = findEntry(cs);
  if (idx < 0) {
    // Free slot, else the entry heard from least recently.
    int oldest = 0;
    for (uint8_t i = 0; i < FM_SOS_TABLE; i++) {
      if (!g_tab[i].used) { idx = i; break; }
      if ((int32_t)(g_tab[i].lastAt - g_tab[oldest].lastAt) < 0) oldest = i;
    }
    if (idx < 0) idx = oldest;
    FmSosEntry &e = g_tab[idx];
    memset(&e, 0, sizeof(e));
    e.used = true;
    memcpy(e.cs, cs, FM_CALLSIGN_LEN);
    e.cs[FM_CALLSIGN_LEN] = '\0';
    e.firstAt = now;
    fresh = true;
  } else {
    FmSosEntry &e = g_tab[idx];
    if ((now - e.lastAt) >= FM_SOS_EPISODE_MS) {
      // Long silence, then a new SOS: a new request, answered afresh.
      e.firstAt = now;
      e.count = 0;
      e.escalated = false;
      e.alarms = 0;
      e.alarmAt = 0;
      e.helpMine = false;
      e.helpBy[0] = '\0';
      fresh = true;
    } else if (e.ch != ch) {
      fresh = true;   // same sender, different help needed: alert again
    }
  }
  FmSosEntry &e = g_tab[idx];
  if (fresh) e.seen = false;
  e.ch = ch;
  e.msgId = msgId;
  e.hops = hops;
  e.rssi = (int16_t)rssi;
  e.snr = snr;
  e.lastAt = now;
  e.count++;
  if (isNew) *isNew = fresh;
  return idx;
}

void fmSosRxHelp(const char *targetCs, const char *byCs, bool mine) {
  const int idx = findEntry(targetCs);
  if (idx < 0) return;
  FmSosEntry &e = g_tab[idx];
  snprintf(e.helpBy, sizeof(e.helpBy), "%s", byCs ? byCs : "");
  if (mine) e.helpMine = true;
}

void fmSosRxAlarmed(int idx, uint32_t now) {
  FmSosEntry *e = fmSosEntryAt(idx);
  if (!e) return;
  e->alarms++;
  e->alarmAt = now;
}

int fmSosRxEscalateTick(uint32_t now, bool *first) {
  for (uint8_t i = 0; i < FM_SOS_TABLE; i++) {
    FmSosEntry &e = g_tab[i];
    if (!e.used || e.helpBy[0]) continue;               // answered: nothing more to do
    if ((now - e.lastAt) >= FM_SOS_EPISODE_MS) continue; // sender gone quiet: listed, silent
    if (!fmSosEntryVisible(e)) {
      // A hidden channel: escalate once nobody has answered in time.
      const uint32_t after =
          e.ch == FM_SOS_MEDICAL ? FM_SOS_ESCALATE_MED_MS : FM_SOS_ESCALATE_MS;
      if ((now - e.firstAt) < after) continue;
      e.escalated = true;
      e.seen = false;
      if (first) *first = true;
    } else {
      // In the main list and unanswered: remind every FM_SOS_REALARM_MS after
      // the last alarm (or after arrival, if it never alarmed, e.g. a channel
      // selected later).
      const uint32_t since = e.alarms ? e.alarmAt : e.firstAt;
      if ((now - since) < FM_SOS_REALARM_MS) continue;
      e.seen = false;
      if (first) *first = false;
    }
    e.alarms++;
    e.alarmAt = now;
    return i;
  }
  return -1;
}

FmSosEntry *fmSosEntryAt(int idx) {
  if (idx < 0 || idx >= FM_SOS_TABLE || !g_tab[idx].used) return nullptr;
  return &g_tab[idx];
}

uint8_t fmSosList(int8_t tab, uint8_t *idxOut, uint8_t cap) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < FM_SOS_TABLE && n < cap; i++) {
    const FmSosEntry &e = g_tab[i];
    if (!e.used) continue;
    if (tab < 0 ? !fmSosEntryVisible(e) : e.ch != (uint8_t)tab) continue;
    idxOut[n++] = i;
  }
  // Newest first. n is at most FM_SOS_TABLE, so insertion sort is plenty.
  for (uint8_t i = 1; i < n; i++) {
    for (uint8_t j = i; j > 0 && (int32_t)(g_tab[idxOut[j]].lastAt - g_tab[idxOut[j - 1]].lastAt) > 0;
         j--) {
      const uint8_t t = idxOut[j];
      idxOut[j] = idxOut[j - 1];
      idxOut[j - 1] = t;
    }
  }
  return n;
}

bool fmSosTabDot(uint8_t ch) {
  if (fmSosSelected(ch)) return false;
  for (uint8_t i = 0; i < FM_SOS_TABLE; i++) {
    if (g_tab[i].used && g_tab[i].ch == ch && !g_tab[i].seen) return true;
  }
  return false;
}

uint8_t fmSosUnseenVisible() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < FM_SOS_TABLE; i++) {
    if (g_tab[i].used && !g_tab[i].seen && fmSosEntryVisible(g_tab[i])) n++;
  }
  return n;
}

uint8_t fmSosVisibleCount() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < FM_SOS_TABLE; i++) {
    if (g_tab[i].used && fmSosEntryVisible(g_tab[i])) n++;
  }
  return n;
}

// ---------------------------------------------------------------- selection
bool fmSosSelected(uint8_t ch) {
  if (ch == FM_SOS_GENERAL) return true;
  return ch < FM_SOS_CH_COUNT && (g_mask & (1u << ch)) != 0;
}

bool fmSosToggle(uint8_t ch) {
  if (ch == FM_SOS_GENERAL || ch >= FM_SOS_CH_COUNT) return false;
  g_mask ^= (uint8_t)(1u << ch);
  saveMask();
  return true;
}

uint8_t fmSosSelectionMask() { return g_mask; }

bool fmSosEntryVisible(const FmSosEntry &e) { return fmSosSelected(e.ch) || e.escalated; }
