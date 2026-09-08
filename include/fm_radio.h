/**
 * FloodMesh - SX1262 driver layer (RadioLib 7.7.1, Heltec WiFi LoRa 32 V3).
 *
 * Everything above this module deals in whole frames: hand it a buffer, get a
 * buffer back. This file owns the parts of the SX1262 that are easy to get
 * wrong, and the reasons they are easy to get wrong are written down below so
 * nobody has to rediscover them by watching a node go deaf in a flood.
 *
 * ============================================================================
 * 125 kHz IS A LEGAL CONSTRAINT, NOT A PERFORMANCE CHOICE. DO NOT "OPTIMISE".
 * ============================================================================
 * India's licence-exempt 865-867 MHz allocation (G.S.R. 853(E)) caps the
 * occupied channel bandwidth at 200 kHz. The SX1262 has no 200 kHz setting -
 * its ladder steps 125 -> 250 -> 500 kHz - so 250 kHz would put this device
 * 50 kHz over the limit and make the whole build unlawful to operate. 125 kHz
 * is the widest legal rung on the ladder. Raising FM_LORA_BW_KHZ to 250 to
 * "make voice faster" is not a tuning decision, it is a compliance failure.
 * Airtime is bought elsewhere (SF, payload size), never here.
 *
 * ---------------------------------------------------------------------------
 * Things in RadioLib 7.7.1 that will bite you (all verified against the source
 * in .platformio/lib/RadioLib/src, not from memory):
 *
 *  1. scanChannel() BLOCKS FOREVER. SX126x.cpp spins on the DIO1 line with
 *     only a yield() and has no timeout at all. One dead DIO1 wire and the
 *     node hangs until the watchdog resets it. This module never calls it.
 *     Every CAD here is startChannelScan() + a bounded poll of
 *     getChannelScanResult().
 *
 *  2. CAD result codes are not what you would guess. Busy is
 *     RADIOLIB_LORA_DETECTED (-702) and free is RADIOLIB_CHANNEL_FREE (-15).
 *     RADIOLIB_PREAMBLE_DETECTED (-14) exists in TypeDef.h but the SX126x
 *     driver NEVER returns it - testing for it is dead code that silently
 *     turns "busy" into "free". Anything else (typically ERR_UNKNOWN) means
 *     the scan has not finished yet.
 *
 *  3. getChannelScanResult() reads the IRQ register but does not clear it.
 *     If you do not clear CAD_DONE|CAD_DETECTED yourself, DIO1 stays high,
 *     the next CAD produces no rising edge, and every later transmit either
 *     stalls or fires blind. This module clears them explicitly after every
 *     scan, including the timeout path.
 *
 *  4. There is no setCadParams() on SX126x. CAD is configured only through the
 *     ChannelScanConfig_t union passed to startChannelScan (use cfg.cad.*).
 *     setCad() exists but is protected AND starts a scan as a side effect.
 *
 *  5. cfg.cad.symNum IS AN ENUM, NOT A COUNT. The register values are
 *     1 symbol = 0x00, 2 = 0x01, 4 = 0x02, 8 = 0x03, 16 = 0x04. Writing the
 *     literal 2 therefore asks for FOUR symbols, roughly doubling CAD time
 *     and the collision window. (Meshtastic ships this exact bug.) To make
 *     that impossible here, the tunable below is FM_CAD_SYMBOLS - a real
 *     symbol COUNT - and fm_radio.cpp translates it to the enum in one place
 *     with a compile-time check that the count is a legal power of two.
 *
 *  6. setDio1Action / setPacketReceivedAction / setPacketSentAction /
 *     setChannelScanAction are literally the same function. They all overwrite
 *     the single DIO1 handler, so the last one attached wins and the others
 *     silently stop working. This module attaches ONE dispatcher for the life
 *     of the program and demultiplexes TX/RX/CAD by reading getIrqFlags().
 *
 *  7. There is no transmit-abort command on the SX1262. standby() IS the
 *     abort: dropping to STDBY_RC mid-packet is explicitly legal (datasheet
 *     Fig. 9-1) and SPI stays alive during TX (Table 8-3), so the command
 *     lands. fmRadioAbortTx() is that, plus the IRQ and state cleanup.
 *
 *  8. Heltec V3 drives the SX1262 from a 1.8 V TCXO on DIO3. Leaving RadioLib's
 *     1.6 V default gives intermittent -706/-707 SPI errors that look like a
 *     broken board. begin() already issues setDio2AsRfSwitch(true), so do not
 *     call it again.
 *
 * ---------------------------------------------------------------------------
 * Listen-before-talk and the backoff shape
 *
 * Every transmit is gated on a CAD. On a busy channel we wait a randomised
 * interval and rescan, up to FM_CAD_MAX_TRIES times; if the channel is busy
 * every single time we return FM_RADIO_ERR_BUSY rather than transmitting
 * blind. Talking over a neighbour does not just lose our packet, it destroys
 * theirs too, and in a flood the packet we would step on is as likely to be an
 * EVACUATE as ours.
 *
 * The backoff is truncated binary exponential (the Ethernet/802.11 shape),
 * chosen because the failure it has to fix is synchronisation: several nodes
 * that all deferred to the SAME busy channel will all rescan the moment it
 * clears, and a fixed delay makes them collide forever. Randomising over a
 * window that DOUBLES after each failure de-synchronises them in a couple of
 * rounds without punishing the common case of one unlucky scan.
 *
 *   wait = rand(0 .. CW-1) * FM_CAD_SLOT_MS,  CW: 4, 8, 16, 32, 32, 32, 32
 *
 * The slot is one 16-symbol preamble at SF7/BW125 (20.7 ms, rounded to 21).
 * That is the natural quantum: two nodes one slot apart are guaranteed to see
 * each other's preamble on the next CAD instead of overlapping. The window is
 * capped at 32 slots so the worst case stays bounded - with the defaults, 8
 * attempts cost at most 21*(3+7+15+31+31+31+31) = 3129 ms of backoff and
 * about 1.6 s on average. That ceiling is deliberate: an unbounded exponential
 * would be the "correct" congestion answer and the wrong life-safety one.
 *
 * A CAD that does not finish within FM_CAD_TIMEOUT_MS is reported as FREE, not
 * busy. This is the one place the module deliberately fails open. If DIO1 is
 * miswired or the CAD engine wedges, "assume busy" means the node never
 * transmits again and dies silently; "assume free" means it transmits without
 * listening, which is exactly how every non-CAD LoRa node on earth behaves.
 * A degraded radio that still shouts beats a polite brick.
 *
 * ---------------------------------------------------------------------------
 * Concurrency and who must call what
 *
 * The DIO1 handler is an IRAM_ATTR ISR that sets one volatile flag and returns.
 * It touches no SPI, no Serial and no allocator - all of that happens later in
 * task context. The radio itself is NOT thread-safe and this module adds no
 * locking: call every fmRadio* function from the same task (the main loop).
 *
 * A transmit is asynchronous. fmRadioTransmit() returns as soon as the packet
 * is handed to the radio; completion is serviced inside fmRadioPoll() and
 * fmRadioIsTransmitting(), so ONE OF THOSE TWO MUST BE CALLED REGULARLY or the
 * transmit never finishes, receive is never re-armed, and the node goes deaf.
 * They are the only two "query" functions here that mutate state, and that is
 * on purpose: it keeps the caller from needing a separate service() call it
 * would inevitably forget.
 *
 * fmRadioTransmit() itself can block for the length of the backoff (see above)
 * because the assigned signature has no way to report "try again later". The
 * waits use delay(), which yields to FreeRTOS, so the RTOS and watchdog keep
 * running - but the caller's own loop() is stalled. Budget for it.
 */
#pragma once
#include <Arduino.h>

#include "floodmesh_pins.h"
#include "fm_packet.h"

// ---------------------------------------------------------------- radio config
// Overridable from platformio.ini. FM_LORA_BW_KHZ is the exception: see the
// regulatory note at the top of this file before touching it.
#ifndef FM_LORA_FREQ_MHZ
#define FM_LORA_FREQ_MHZ 866.5f    // mid-band in the 865-867 MHz Indian allocation
#endif
#ifndef FM_LORA_BW_KHZ
#define FM_LORA_BW_KHZ 125.0f      // LEGAL CEILING - G.S.R. 853(E). Do not raise.
#endif
#ifndef FM_LORA_SF
#define FM_LORA_SF 7               // fastest SF; voice needs the airtime back
#endif
#ifndef FM_LORA_CR
#define FM_LORA_CR 5               // 4/5 - denominator, per RadioLib's convention
#endif
#ifndef FM_LORA_PREAMBLE
#define FM_LORA_PREAMBLE 16        // long enough that a CAD on a neighbour lands
#endif
#ifndef FM_LORA_SYNC_WORD
#define FM_LORA_SYNC_WORD 0x12     // private network; 0x34 is the public/LoRaWAN one
#endif
#ifndef FM_LORA_TX_DBM
#define FM_LORA_TX_DBM 20          // 100 mW, inside the 1 W ERP allowance
#endif
#ifndef FM_LORA_CURRENT_MA
#define FM_LORA_CURRENT_MA 140.0f  // PA overcurrent trip, not a power setting
#endif
#ifndef FM_LORA_TCXO_V
#define FM_LORA_TCXO_V 1.8f        // Heltec V3 TCXO on DIO3; 1.6 gives -706/-707
#endif
#ifndef FM_LORA_USE_LDO
#define FM_LORA_USE_LDO 0          // 0 = DC-DC. The V3 has the buck inductor.
#endif
#ifndef FM_LORA_CRC_BYTES
#define FM_LORA_CRC_BYTES 2        // hardware CRC on; 0 disables it
#endif
#ifndef FM_LORA_RX_BOOST
#define FM_LORA_RX_BOOST 1         // ~2 dB sensitivity for ~2 mA. Worth it here.
#endif
#ifndef FM_RADIO_SPI_HZ
#define FM_RADIO_SPI_HZ 2000000    // RadioLib's default; the bus is LoRa-only
#endif

/** Largest frame this module will send or hand back. */
#ifndef FM_RADIO_MAX_LEN
#define FM_RADIO_MAX_LEN FM_FRAME_LEN_MAX
#endif

// ---------------------------------------------------------------- LBT tunables
#ifndef FM_CAD_SYMBOLS
#define FM_CAD_SYMBOLS 2           // COUNT, not the register enum. 1/2/4/8/16 only.
#endif                             // See note 5 above - this is checked at compile time.
#ifndef FM_CAD_MAX_TRIES
#define FM_CAD_MAX_TRIES 8         // scans before we give up and report BUSY
#endif
#ifndef FM_CAD_SLOT_MS
#define FM_CAD_SLOT_MS 21          // one 16-symbol preamble at SF7/BW125 (20.7 ms)
#endif
#ifndef FM_CAD_CW_MIN
#define FM_CAD_CW_MIN 4            // first contention window, in slots
#endif
#ifndef FM_CAD_CW_MAX
#define FM_CAD_CW_MAX 32           // window ceiling; bounds the worst-case wait
#endif

/**
 * How long a single CAD may take before we give up on it and call the channel
 * free. Scales with SF so that raising FM_LORA_SF does not silently turn every
 * scan into a timeout: a CAD costs roughly (symbols + ~4) symbol periods, and
 * one symbol period is 2^SF / BW_kHz milliseconds. The +10 ms covers the SPI
 * command sequence and one FreeRTOS tick of polling granularity.
 */
#ifndef FM_CAD_TIMEOUT_MS
#define FM_CAD_TIMEOUT_MS                                                      \
  (((uint32_t)(1UL << FM_LORA_SF) * (uint32_t)(FM_CAD_SYMBOLS + 4)) /          \
       (uint32_t)FM_LORA_BW_KHZ +                                              \
   10UL)
#endif

// ---------------------------------------------------------------- status codes
// Success and every hardware failure are RadioLib status codes passed through
// unchanged, so a caller can print them with RadioLib's own tables. The two
// FloodMesh-specific codes sit at -2001/-2002, clear of RadioLib's range
// (which currently bottoms out at -1401).
#define FM_RADIO_OK             0
#define FM_RADIO_ERR_BUSY       (-702)   // == RADIOLIB_LORA_DETECTED: all CAD tries busy
#define FM_RADIO_ERR_TOO_LONG   (-4)     // == RADIOLIB_ERR_PACKET_TOO_LONG
#define FM_RADIO_ERR_TX_ACTIVE  (-2001)  // a transmit is already in flight
#define FM_RADIO_ERR_NOT_READY  (-2002)  // fmRadioBegin() failed or was never called

// ---------------------------------------------------------------- API
/**
 * Bring up SPI and the SX1262 with the configuration above and attach the DIO1
 * dispatcher. Leaves the radio in standby - call fmRadioStartReceive() to
 * listen. Logs the failing RadioLib code to Serial and returns false on error;
 * every other entry point then returns FM_RADIO_ERR_NOT_READY / false rather
 * than talking to an unconfigured chip.
 */
bool fmRadioBegin();

/**
 * Run exactly one CAD and report whether the channel is occupied.
 *
 * Blocks for at most timeoutMs (delay(1) between polls, so FreeRTOS still
 * runs). A scan that does not complete in time is reported as FREE - see the
 * fail-open rationale above. Returns true without scanning while one of our own
 * packets is on the air, since that carrier is exactly what a CAD would find.
 * Clears the CAD IRQ bits on every path, and
 * restores continuous receive afterwards if fmRadioStartReceive() had been
 * called, so a node polling channel occupancy for the UI never goes deaf.
 */
bool fmRadioChannelBusy(uint32_t timeoutMs);

/**
 * Listen-before-talk transmit. Returns FM_RADIO_OK once the packet has been
 * handed to the radio - NOT when it has been sent; poll fmRadioIsTransmitting()
 * for that. Returns FM_RADIO_ERR_BUSY if every CAD attempt found the channel
 * occupied, FM_RADIO_ERR_TX_ACTIVE if a transmit is still in flight,
 * FM_RADIO_ERR_TOO_LONG for len == 0 or len > FM_RADIO_MAX_LEN, or the raw
 * RadioLib error if the chip refused the command.
 *
 * May block for up to the full backoff (~3.1 s worst case with the defaults).
 * The radio is put back into receive between backoff waits: deferring to a
 * neighbour and then not listening to what they said would be the worst of
 * both worlds.
 */
int16_t fmRadioTransmit(const uint8_t *buf, size_t len);

/**
 * Abandon an in-flight transmission. standby() is the abort - see note 7. Safe
 * to call mid-packet and safe to call when nothing is being transmitted. Clears
 * the IRQ state and returns to receive if receive was wanted. The half-sent
 * packet is lost, which is the point: this exists so a life-safety alarm can
 * pre-empt a voice fragment.
 */
void fmRadioAbortTx();

/**
 * Enter continuous receive and stay there. Idempotent. If a transmit is in
 * flight the request is remembered and applied automatically when the
 * transmission completes; the return value is true in that case too, since the
 * radio will be listening shortly and the caller has nothing to retry.
 */
bool fmRadioStartReceive();

/**
 * Service the radio and, if a complete CRC-valid frame arrived, copy it out.
 *
 * Returns true only when *outLen bytes were written to buf. buf must be at
 * least FM_RADIO_MAX_LEN bytes; a frame longer than cap is dropped rather than
 * truncated, because a short frame handed to fm_packet's parser is a bug
 * generator, and its IRQ is cleared anyway so receive does not wedge. Frames
 * failing the hardware CRC are dropped silently.
 *
 * outLen, rssi and snr may all be NULL. This function also completes any
 * finished transmission - see the note about who must call what, above.
 */
bool fmRadioPoll(uint8_t *buf, size_t cap, size_t *outLen, float *rssi, float *snr);

/** SNR in dB of the last frame fmRadioPoll() returned true for. 0 before then. */
float fmRadioLastSnr();

/**
 * True while a packet is on the air. Also services transmit completion, so it
 * is safe (and intended) to use as the condition of a wait loop. A transmit
 * that never reports done is aborted after 5x its expected time-on-air so the
 * flag can never latch on forever.
 */
bool fmRadioIsTransmitting();
