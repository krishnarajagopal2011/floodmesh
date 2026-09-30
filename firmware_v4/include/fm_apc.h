/**
 * FloodMesh V4 - transmit power: the antenna cap and adaptive power control.
 *
 * docs/architecture.md §1.1 (G.S.R. 853(E) Table II: 500 mW e.r.p., APC
 * required), §14.4 (antenna gain) and §16 (the design). Plan:
 * firmware_v4/docs/next-build-4.4.md.
 *
 * Antenna cap
 * -----------
 * The legal limit is on RADIATED power, so it depends on the antenna fitted.
 * Each unit stores its antenna gain (dBi) and cable loss (dB) in NVS
 * (`ant <gain> [loss]`), and the maximum transmit power is
 *     txMax = min(FM_LORA_TX_DBM, floor(29.15 - gain + loss))
 * 29.15 dBm is 500 mW e.r.p. (27 dBm) plus the 2.15 dBi of a half-wave dipole,
 * which e.r.p. is measured against. The standard whip gives 27, so the chip's
 * +22 dBm is the limit; a 12 dBi Yagi with 1 dB of cable gives 18.
 *
 * Adaptive power control
 * ----------------------
 * A broadcast mesh has no single link to tune, so the unit tunes to its
 * neighbourhood, and only for texts:
 * - SOS, ACK and heartbeat frames (sent or relayed) always go at txMax. Every
 *   such frame received is therefore a sample of a neighbour link heard at full
 *   power: its SNR (the last hop's, whoever relayed it) is kept for 65 min.
 * - Texts go at txMax minus a reduction from the WEAKEST sample:
 *       margin = lowest SNR - demodulation floor at FM_LORA_SF - FM_APC_MARGIN_DB
 *   rounded down to 2 dB steps, never below 0, and never taking the power below
 *   FM_APC_MIN_DBM. Power only drops when every neighbour heard lately is
 *   strong, which suits a flood mesh.
 * - Full power when there are fewer than FM_APC_MIN_SAMPLES samples, and for
 *   FM_APC_BOOST_MS after a text we sent was not heard being forwarded.
 * - The reduction grows by at most one step a minute, and falls at once when a
 *   weaker sample arrives.
 *
 * LoRa SNR stops rising at roughly +10 dB, so very strong links look weaker
 * than they are and the reduction tops out around 10-15 dB. That errs on the
 * safe side.
 *
 * `apc off` (saved) sends everything at txMax: use it for range tests, or the
 * PING texts go out at reduced power and the results mean nothing.
 */
#pragma once

#include <Arduino.h>

#ifndef FM_APC
#define FM_APC 1                    // 0 = build without power control (every frame at txMax)
#endif
#ifndef FM_APC_MARGIN_DB
#define FM_APC_MARGIN_DB 10         // safety margin above the demodulation floor
#endif
#ifndef FM_APC_MIN_DBM
#define FM_APC_MIN_DBM 8            // texts never go below this
#endif
#ifndef FM_APC_STEP_DB
#define FM_APC_STEP_DB 2
#endif
#ifndef FM_APC_SAMPLES
#define FM_APC_SAMPLES 32           // ring of recent full-power link samples
#endif
#ifndef FM_APC_MIN_SAMPLES
#define FM_APC_MIN_SAMPLES 2        // fewer than this: full power
#endif
#ifndef FM_APC_WINDOW_MS
#define FM_APC_WINDOW_MS (65UL * 60UL * 1000UL)   // the forwarding rule's window, §13.3
#endif
#ifndef FM_APC_PERIOD_MS
#define FM_APC_PERIOD_MS 60000UL    // recompute once a minute
#endif
#ifndef FM_APC_ECHO_WAIT_MS
#define FM_APC_ECHO_WAIT_MS 30000UL // a sent text not heard forwarded within this...
#endif
#ifndef FM_APC_BOOST_MS
#define FM_APC_BOOST_MS (15UL * 60UL * 1000UL)    // ...puts texts back at full power this long
#endif

/** Load the antenna setting and the APC switch from NVS. Call after fmRadioBegin(). */
void fmApcBegin();

/** Once per loop pass: the minute recompute and the unanswered-text check. */
void fmApcLoop(uint32_t now);

/** Power in dBm for the next transmission of a frame of this FM_TYPE_*. */
int8_t fmApcPowerFor(uint8_t frameType, uint32_t now);

/** A frame was received and authenticated: feeds the samples if it went at full power. */
void fmApcOnReceive(uint8_t frameType, float snr, uint32_t now);

/** We sent a text with this msgId (after the radio accepted it). */
void fmApcOnTextSent(uint8_t msgId, uint32_t now);

/** One of our own frames came back, i.e. a neighbour forwarded it. */
void fmApcOnEcho(uint8_t frameType, uint8_t msgId);

/** Maximum transmit power after the antenna cap. */
int8_t fmApcTxMaxDbm();

/** Current reduction for texts in dB (0 when APC is off or at full power). */
uint8_t fmApcReductionDb(uint32_t now);

bool fmApcEnabled();
bool fmApcSetEnabled(bool on);   // saved in NVS; false if NVS failed

/** Antenna gain (dBi) and cable loss (dB) as stored. */
float fmApcAntennaGainDbi();
float fmApcCableLossDb();
/** Store a new antenna; false if out of range (gain -5..20 dBi, loss 0..20 dB) or NVS failed. */
bool fmApcSetAntenna(float gainDbi, float lossDb);
/** Back to the standard whip (2.2 dBi, no loss). */
void fmApcResetAntenna();

/** Print the `ant` / `apc` status lines. */
void fmApcPrintAntenna();
void fmApcPrintStatus(uint32_t now);
