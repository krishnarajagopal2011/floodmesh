/**
 * FloodMesh V4 - transmit power: the antenna cap and adaptive power control.
 * See fm_apc.h for the rules and docs/architecture.md §16 for the reasoning.
 *
 * All time arithmetic uses unsigned subtraction (now - then), which is correct
 * across the millis() rollover.
 */
#include "fm_apc.h"

#include <Preferences.h>
#include <math.h>

#include "fm_ids.h"
#include "fm_packet.h"
#include "fm_radio.h"

#define FM_NVS_KEY_ANTGAIN "antgain"   // dBi x10, int16; absent = standard whip
#define FM_NVS_KEY_ANTLOSS "antloss"   // dB x10, int16
#define FM_NVS_KEY_APC     "apc"       // 0 = off; absent = on

namespace {

const int16_t kWhipGain10 = 22;   // the standard half-wave whip, §14.1
const uint8_t kSentTrack = 4;     // texts awaiting an echo

struct Sample {
  bool     used;
  int16_t  snr10;   // dB x10
  uint32_t at;
};

struct SentText {
  bool     used;
  uint8_t  msgId;
  uint32_t at;
};

Sample   g_samples[FM_APC_SAMPLES];
uint8_t  g_nextSample = 0;
SentText g_sent[kSentTrack];

int16_t  g_gain10 = kWhipGain10;
int16_t  g_loss10 = 0;
int8_t   g_txMax = FM_LORA_TX_DBM;
bool     g_enabled = FM_APC ? true : false;
uint8_t  g_reduce = 0;            // current reduction for texts, dB
bool     g_boosting = false;
uint32_t g_boostAt = 0;
uint32_t g_lastRecalc = 0;

/** SX1262 demodulation floor (SNR, dB) at the network's spreading factor. */
float demodFloorDb() {
  switch (FM_LORA_SF) {
    case 5:  return -2.5f;
    case 6:  return -5.0f;
    case 7:  return -7.5f;
    case 8:  return -10.0f;
    case 9:  return -12.5f;
    case 10: return -15.0f;
    case 11: return -17.5f;
    default: return -20.0f;
  }
}

bool isFullPower(uint8_t type) {
  return type == FM_TYPE_ALARM || type == FM_TYPE_ACK || type == FM_TYPE_HEARTBEAT;
}

void computeTxMax() {
  const float cap = floorf(29.15f - g_gain10 / 10.0f + g_loss10 / 10.0f + 0.001f);
  int v = (int)FM_LORA_TX_DBM;
  if (cap < v) v = (int)cap;
  if (v < -9) v = -9;   // SX1262 minimum
  g_txMax = (int8_t)v;
}

/** Samples inside the window, and the lowest SNR among them. */
uint8_t liveSamples(uint32_t now, float *lowest) {
  uint8_t n = 0;
  float lo = 99.0f;
  for (uint8_t i = 0; i < FM_APC_SAMPLES; i++) {
    const Sample &s = g_samples[i];
    if (!s.used || (now - s.at) >= FM_APC_WINDOW_MS) continue;
    n++;
    const float v = s.snr10 / 10.0f;
    if (v < lo) lo = v;
  }
  if (lowest) *lowest = lo;
  return n;
}

bool boostActive(uint32_t now) {
  if (g_boosting && (now - g_boostAt) >= FM_APC_BOOST_MS) g_boosting = false;
  return g_boosting;
}

/** The reduction the samples justify right now, before the one-step-a-minute rule. */
uint8_t targetReduction(uint32_t now) {
  float lowest = 0.0f;
  if (liveSamples(now, &lowest) < FM_APC_MIN_SAMPLES) return 0;
  const float margin = lowest - demodFloorDb() - (float)FM_APC_MARGIN_DB;
  if (margin <= 0.0f) return 0;
  int t = ((int)margin / FM_APC_STEP_DB) * FM_APC_STEP_DB;
  const int room = (int)g_txMax - (int)FM_APC_MIN_DBM;
  if (room <= 0) return 0;
  if (t > room) t = (room / FM_APC_STEP_DB) * FM_APC_STEP_DB;
  return t > 0 ? (uint8_t)t : 0;
}

}  // namespace

// ---------------------------------------------------------------- API
void fmApcBegin() {
  memset(g_samples, 0, sizeof(g_samples));
  memset(g_sent, 0, sizeof(g_sent));
  Preferences p;
  if (p.begin(FM_NVS_NAMESPACE, true)) {
    g_gain10 = p.getShort(FM_NVS_KEY_ANTGAIN, kWhipGain10);
    g_loss10 = p.getShort(FM_NVS_KEY_ANTLOSS, 0);
    g_enabled = FM_APC ? (p.getUChar(FM_NVS_KEY_APC, 1) != 0) : false;
    p.end();
  }
  computeTxMax();
  g_reduce = 0;
  g_lastRecalc = millis();
  fmApcPrintAntenna();
  Serial.printf("[APC] power control %s\n", g_enabled ? "ON (texts only)" : "OFF");
}

void fmApcLoop(uint32_t now) {
  // A text nobody forwarded: back to full power for a while.
  for (uint8_t i = 0; i < kSentTrack; i++) {
    SentText &s = g_sent[i];
    if (!s.used || (now - s.at) < FM_APC_ECHO_WAIT_MS) continue;
    s.used = false;
    if (g_enabled && !g_boosting) {
      Serial.printf("[APC] text msg %u not heard forwarded - texts at full power for %lu min\n",
                    (unsigned)s.msgId, (unsigned long)(FM_APC_BOOST_MS / 60000UL));
    }
    g_boosting = true;
    g_boostAt = now;
    g_reduce = 0;
  }

  if ((now - g_lastRecalc) < FM_APC_PERIOD_MS) return;
  g_lastRecalc = now;
  if (boostActive(now)) {   // full power until the boost ends; re-earn the reduction after
    g_reduce = 0;
    return;
  }
  const uint8_t t = targetReduction(now);
  const uint8_t before = g_reduce;
  if (t > g_reduce) {
    g_reduce = (uint8_t)((g_reduce + FM_APC_STEP_DB) > t ? t : (g_reduce + FM_APC_STEP_DB));
  } else {
    g_reduce = t;
  }
  if (g_enabled && g_reduce != before) {
    Serial.printf("[APC] text power %d dBm (reduction %u dB)\n",
                  (int)g_txMax - (boostActive(now) ? 0 : (int)g_reduce), (unsigned)g_reduce);
  }
}

int8_t fmApcPowerFor(uint8_t frameType, uint32_t now) {
  if (!g_enabled || isFullPower(frameType)) return g_txMax;
  return (int8_t)((int)g_txMax - (int)fmApcReductionDb(now));
}

void fmApcOnReceive(uint8_t frameType, float snr, uint32_t now) {
  if (!isFullPower(frameType)) return;
  Sample &s = g_samples[g_nextSample];
  s.used = true;
  s.snr10 = (int16_t)lroundf(snr * 10.0f);
  s.at = now;
  g_nextSample = (uint8_t)((g_nextSample + 1) % FM_APC_SAMPLES);
  // A weaker neighbour takes effect at once; raising the reduction waits for
  // the minute recompute.
  const uint8_t t = targetReduction(now);
  if (t < g_reduce) {
    g_reduce = t;
    if (g_enabled) {
      Serial.printf("[APC] weaker neighbour (SNR %.1f) - text power %d dBm\n", snr,
                    (int)g_txMax - (int)g_reduce);
    }
  }
}

void fmApcOnTextSent(uint8_t msgId, uint32_t now) {
  uint8_t slot = 0;
  for (uint8_t i = 0; i < kSentTrack; i++) {
    if (!g_sent[i].used) { slot = i; break; }
    if ((now - g_sent[i].at) > (now - g_sent[slot].at)) slot = i;   // else the oldest
  }
  g_sent[slot].used = true;
  g_sent[slot].msgId = msgId;
  g_sent[slot].at = now;
}

void fmApcOnEcho(uint8_t frameType, uint8_t msgId) {
  if (frameType != FM_TYPE_TEXT) return;
  for (uint8_t i = 0; i < kSentTrack; i++) {
    if (g_sent[i].used && g_sent[i].msgId == msgId) g_sent[i].used = false;
  }
}

int8_t fmApcTxMaxDbm() { return g_txMax; }

uint8_t fmApcReductionDb(uint32_t now) {
  if (!g_enabled || boostActive(now)) return 0;
  if (liveSamples(now, nullptr) < FM_APC_MIN_SAMPLES) return 0;
  return g_reduce;
}

bool fmApcEnabled() { return g_enabled; }

bool fmApcSetEnabled(bool on) {
  if (!FM_APC && on) return false;   // built without it
  Preferences p;
  if (!p.begin(FM_NVS_NAMESPACE, false)) return false;
  p.putUChar(FM_NVS_KEY_APC, on ? 1 : 0);
  p.end();
  g_enabled = on;
  return true;
}

float fmApcAntennaGainDbi() { return g_gain10 / 10.0f; }
float fmApcCableLossDb() { return g_loss10 / 10.0f; }

bool fmApcSetAntenna(float gainDbi, float lossDb) {
  if (!(gainDbi >= -5.0f && gainDbi <= 20.0f) || !(lossDb >= 0.0f && lossDb <= 20.0f)) {
    return false;
  }
  const int16_t g = (int16_t)lroundf(gainDbi * 10.0f);
  const int16_t l = (int16_t)lroundf(lossDb * 10.0f);
  Preferences p;
  if (!p.begin(FM_NVS_NAMESPACE, false)) return false;
  p.putShort(FM_NVS_KEY_ANTGAIN, g);
  p.putShort(FM_NVS_KEY_ANTLOSS, l);
  p.end();
  g_gain10 = g;
  g_loss10 = l;
  computeTxMax();
  g_reduce = 0;   // re-earn any reduction against the new maximum
  return true;
}

void fmApcResetAntenna() {
  Preferences p;
  if (p.begin(FM_NVS_NAMESPACE, false)) {
    p.remove(FM_NVS_KEY_ANTGAIN);
    p.remove(FM_NVS_KEY_ANTLOSS);
    p.end();
  }
  g_gain10 = kWhipGain10;
  g_loss10 = 0;
  computeTxMax();
  g_reduce = 0;
}

void fmApcPrintAntenna() {
  const float erpDbm = g_txMax + g_gain10 / 10.0f - g_loss10 / 10.0f - 2.15f;
  Serial.printf("[ANT] antenna %.1f dBi, cable loss %.1f dB -> max %d dBm "
                "(%.0f mW e.r.p., limit 500)\n",
                g_gain10 / 10.0f, g_loss10 / 10.0f, (int)g_txMax, powf(10.0f, erpDbm / 10.0f));
}

void fmApcPrintStatus(uint32_t now) {
  float lowest = 0.0f;
  const uint8_t n = liveSamples(now, &lowest);
  Serial.printf("[APC] %s, max %d dBm, %u samples in %lu min", g_enabled ? "ON" : "OFF",
                (int)g_txMax, (unsigned)n, (unsigned long)(FM_APC_WINDOW_MS / 60000UL));
  if (n) Serial.printf(", lowest SNR %.1f dB", lowest);
  Serial.printf(", floor SF%u %.1f dB, margin %u dB\n", (unsigned)FM_LORA_SF, demodFloorDb(),
                (unsigned)FM_APC_MARGIN_DB);
  Serial.printf("[APC] SOS/ACK/heartbeat %d dBm, texts %d dBm (reduction %u dB%s%s)\n",
                (int)g_txMax, (int)fmApcPowerFor(FM_TYPE_TEXT, now),
                (unsigned)fmApcReductionDb(now),
                boostActive(now) ? ", full power: a text was not forwarded" : "",
                (g_enabled && n < FM_APC_MIN_SAMPLES) ? ", full power: too few samples" : "");
}
