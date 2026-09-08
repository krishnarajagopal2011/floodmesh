/**
 * FloodMesh - I2S voice path: INMP441 microphone in, MAX98357A amplifier out.
 *
 * The rest of the firmware talks to this module in one currency only: 8 kHz,
 * 16-bit, mono PCM. That is what Codec2 1200 bps wants, and it is what
 * fmMicRead() hands back and fmSpeakerWrite() expects. Everything below is
 * about the gap between that clean number and what the two chips will
 * actually do.
 *
 * Why the microphone does NOT run at 8 kHz
 * ----------------------------------------
 * The obvious build - configure I2S RX for 8 kHz and read 8 kHz samples - does
 * not work reliably. The INMP441 is a clocked-slave PDM-to-I2S part with no
 * internal PLL: its decimator is driven directly by the bit clock we supply,
 * so its usable sample-rate range is really a usable BIT-clock range. At 8 kHz
 * with the customary 64 bit clocks per frame the SCK is 512 kHz, which sits at
 * the very bottom edge of the part's specification. Devices in that region are
 * not merely noisy but erratic: some units run, some produce a badly-scaled or
 * silent stream, and the same unit can behave differently across temperature.
 * It is the wrong place to put a life-safety voice channel.
 *
 * So the capture side runs at FM_MIC_I2S_HZ (16 kHz, ~1.024 MHz SCK, comfortably
 * inside spec) and this module decimates by two in software. fmMicRead() is
 * still an 8 kHz source; the 16 kHz link is entirely internal.
 *
 * Why there is a filter and not just "drop every other sample"
 * -----------------------------------------------------------
 * Decimating by 2 without a low-pass folds everything from 4-8 kHz back down
 * over the 0-4 kHz band we keep. That is not a subtle degradation: sibilance
 * and fricatives (/s/, /f/, /sh/), which carry most of their energy above
 * 4 kHz, would reappear mirrored on top of the vowels. Codec2 at 1200 bps
 * fits a harmonic speech model to whatever it is given; feed it a spectrum
 * with a mirrored copy of the top octave laid over the formants and the model
 * has nothing coherent to fit, which is what "gravel" sounds like.
 *
 * The anti-alias filter: 27-tap half-band FIR
 * -------------------------------------------
 * Symmetric (linear phase), 27 taps, Q15 integer coefficients. It is a HALF-
 * BAND filter, which means it is -6 dB at exactly Fs/4 = 4 kHz and every even
 * tap either side of the centre is exactly zero - 15 of the 27 taps are
 * non-zero, and the centre tap is exactly 0.5. That structure is not decorative:
 * it is what makes the same coefficient set usable, unchanged, as the
 * anti-imaging filter for the 2x interpolator on the playback side.
 *
 * Provenance - these numbers were generated, not copied from anywhere:
 *
 *   h_ideal[m] = 0.5              for m == 0            (m = n - 13)
 *              = 0                for m even, m != 0
 *              = sin(pi*m/2)/(pi*m) for m odd
 *   windowed by Hamming w[n] = 0.54 - 0.46*cos(2*pi*n/26), n = 0..26
 *   the ODD taps only are then normalised to sum to 0.5, so the centre tap
 *   stays exactly 0.5 and the whole filter sums to exactly 1.0 (unity DC gain)
 *   scaled by 32768 and rounded; the 2 LSB of rounding error is absorbed
 *   symmetrically into the largest tap pair so the result stays symmetric and
 *   sums to exactly 32768.
 *
 * The .cpp carries the resulting integers and static_asserts all three
 * properties (sum, centre tap, accumulator headroom), so a future reader who
 * wants a longer filter can re-run the recipe above and the asserts will catch
 * a bad transcription.
 *
 * Measured response of the quantised filter (16 kHz input rate):
 *
 *      0 - 3000 Hz   ripple within +/-0.06 dB
 *         3400 Hz    -0.8 dB     (top of the Codec2 speech band)
 *         4000 Hz    -6.0 dB     (the half-band symmetry point)
 *         4600 Hz   -21.4 dB
 *   5000 - 8000 Hz   -43.3 dB or better
 *
 * The band that matters is 5-8 kHz, because that is what folds down into
 * 0-3 kHz where the speech lives; 4-5 kHz folds into 4-3 kHz, the least
 * critical corner of the band. 43 dB of rejection there puts any aliased
 * fricative energy far below Codec2's own quantisation noise.
 *
 * Why the SPEAKER also runs at 16 kHz
 * -----------------------------------
 * By default (FM_SPK_UPSAMPLE) the output I2S runs at 16 kHz too and this
 * module interpolates the 8 kHz stream by two on the way out, reusing the same
 * half-band kernel as the anti-imaging filter. The reasoning is the same shape
 * as the microphone's: 8 kHz x 32 bit clocks is a 256 kHz BCLK, and clocking a
 * class-D amplifier that slowly is asking for the same class of trouble at the
 * bottom of its range. This is a precaution rather than a measured failure -
 * the MAX98357A does claim 8 kHz support - so the escape hatch is one flag:
 * build with -D FM_SPK_UPSAMPLE=0 to drive the amplifier at 8 kHz directly and
 * skip the interpolator entirely.
 *
 * Note the interpolator's gain: zero-stuffing halves the amplitude, so the
 * filter output is doubled to compensate. A doubled half-band output can
 * momentarily exceed full scale on transients, which is exactly why every
 * sample leaves through a saturating clamp rather than a cast.
 *
 * 24 bits in a 32-bit slot
 * ------------------------
 * The INMP441 puts its 24-bit sample LEFT-JUSTIFIED in a 32-bit slot, so I2S RX
 * is configured for 32-bit words and the int16 is taken from the TOP of the
 * word (>> 16), never by truncating the bottom. Truncating would keep the 16
 * least significant bits - which for a left-justified sample is the noise
 * floor and 8 bits of hard-wired zero, i.e. silence with a rattle in it.
 *
 * L/R on the microphone is tied to GND, so its data appears in the LEFT slot
 * and RX is configured mono-left. The legacy driver's LEFT/RIGHT sense has a
 * long history of being inverted on some ESP32 variants, so it is a tunable:
 * if the microphone reads as silence or a stuck DC value, build with
 * -D FM_MIC_SWAP_CHANNEL=1 before suspecting the wiring.
 *
 * One peripheral each, never both at once
 * ---------------------------------------
 * Capture uses I2S_NUM_0 and playback I2S_NUM_1 because the two directions
 * need different sample rates, word widths and channel layouts, and sharing a
 * port would mean tearing the configuration down between every record and
 * play. They are still mutually exclusive at runtime - fmMicStart() refuses
 * while the speaker is running and vice versa. This is deliberate: it is a
 * half-duplex push-to-talk radio, monitoring your own transmission through the
 * cavity speaker would just feed the microphone, and the driver's DMA buffers
 * are a real cost on a part with 320 KB of DRAM and no PSRAM.
 *
 * The driver is installed on start and uninstalled on stop rather than left
 * resident, so an idle FloodMesh is not holding ~4 KB of DMA buffers per
 * direction for nothing.
 *
 * Amplifier gating
 * ----------------
 * The MAX98357A idles around 2 mA, which on a battery-powered device that
 * spends almost all its life listening is worth more than it sounds. When
 * floodmesh_pins.h defines PIN_AMP_SD the amplifier is held in shutdown except
 * during playback. When it does not, the pin is presumably strapped high on
 * the board, playback still works, and fmAudioBegin() says once at boot that
 * the idle current is being paid.
 *
 * On the usual MAX98357A breakout, SD_MODE is not a plain enable: the voltage
 * on it also selects the channel. Driven hard HIGH by a GPIO it means "on, and
 * output (L+R)/2". That is why playback writes the same mono sample into BOTH
 * I2S slots - it makes the amplifier's output independent of which slot the
 * board decided to listen to, at the cost of nothing but DMA bytes.
 *
 * Everything here is fixed-size and static. Nothing allocates after
 * fmAudioBegin(), and the I2S driver's own DMA buffers are the only heap this
 * module is responsible for.
 *
 * All time arithmetic is unsigned subtraction (now - then), correct across the
 * 49.7-day millis() rollover.
 */
#pragma once
#include <Arduino.h>

#include "floodmesh_pins.h"

// ---------------------------------------------------------------- tunables
#ifndef FM_AUDIO_HZ
#define FM_AUDIO_HZ 8000           // the only rate this module's API speaks
#endif
#ifndef FM_MIC_I2S_HZ
#define FM_MIC_I2S_HZ 16000        // actual INMP441 clock; see the header notes
#endif
#ifndef FM_SPK_UPSAMPLE
#define FM_SPK_UPSAMPLE 1          // 1 = amp runs at 2x with the interpolator,
#endif                             // 0 = amp runs at FM_AUDIO_HZ directly

#ifndef FM_MIC_I2S_PORT
#define FM_MIC_I2S_PORT 0          // I2S_NUM_0; cast in the .cpp so this header
#endif                             // does not drag driver/i2s.h into every TU
#ifndef FM_SPK_I2S_PORT
#define FM_SPK_I2S_PORT 1          // I2S_NUM_1, independent of the microphone
#endif

#ifndef FM_MIC_SWAP_CHANNEL
#define FM_MIC_SWAP_CHANNEL 0      // 1 = take the RIGHT slot instead. Try this
#endif                             // before rewiring a silent INMP441.

// 4 x 256 frames is 64 ms of slack at 16 kHz: enough that a display refresh or
// a LoRa interrupt cannot punch a hole in a recording, small enough that the
// pair costs well under 10 KB of DRAM and only while actually recording.
#ifndef FM_MIC_DMA_BUFS
#define FM_MIC_DMA_BUFS 4
#endif
#ifndef FM_MIC_DMA_LEN
#define FM_MIC_DMA_LEN 256
#endif
#ifndef FM_SPK_DMA_BUFS
#define FM_SPK_DMA_BUFS 4
#endif
#ifndef FM_SPK_DMA_LEN
#define FM_SPK_DMA_LEN 256
#endif

// The INMP441 has no PLL: its decimator settles over the first few thousand
// bit clocks after SCK appears, and until it does the stream is a thump. Those
// samples are discarded inside fmMicRead rather than by blocking fmMicStart,
// so the main loop keeps polling buttons through the settling window.
#ifndef FM_MIC_PRIME_MS
#define FM_MIC_PRIME_MS 30
#endif

#ifndef FM_MIC_READ_TIMEOUT_MS
#define FM_MIC_READ_TIMEOUT_MS 100 // fmMicRead gives up rather than hanging the
#endif                             // main loop if the mic clock dies
#ifndef FM_SPK_WRITE_TIMEOUT_MS
#define FM_SPK_WRITE_TIMEOUT_MS 200
#endif

#ifndef FM_AUDIO_VOL_DEFAULT
#define FM_AUDIO_VOL_DEFAULT 60    // percent. The cavity speaker is an 8 ohm
#endif                             // 1 W part in a small box; 100% is a buzz,
                                   // not a message.

// Unity by default so the capture path is bit-transparent out of the box. The
// INMP441 is a quiet microphone (-26 dBFS at 94 dB SPL), so speech at arm's
// length lands low in the range; raise this in Q8 (512 = 2x) if the recorded
// level measures thin, and re-check for clipping when you do.
#ifndef FM_MIC_GAIN_Q8
#define FM_MIC_GAIN_Q8 256
#endif

// Settling around SD_MODE moving. On the way down this is added to a drain
// delay computed from the DMA geometry, because cutting the amplifier with a
// non-zero sample still on the line is a click at both ends.
#ifndef FM_AMP_SETTLE_MS
#define FM_AMP_SETTLE_MS 10
#endif

// ---------------------------------------------------------------- API
/**
 * Idempotent. Parks the amplifier in shutdown, clears the filter state, sets
 * the default volume, and proves the RX configuration is accepted by the
 * driver so a bad pin map fails at boot rather than at the first PTT press.
 * Returns false if that probe fails; nothing else here can.
 */
bool fmAudioBegin();

/** Install and start I2S RX. False if already capturing or the speaker is on. */
bool fmMicStart();
void fmMicStop();

/**
 * Up to maxSamples of 8 kHz mono PCM. Blocks until the buffer is full or
 * FM_MIC_READ_TIMEOUT_MS has passed, whichever comes first, and returns how
 * many samples were actually produced. A short count means the timeout expired
 * - or, on the first reads after fmMicStart, that the microphone's start-up
 * settling window is still being discarded; it never means end of stream.
 * Safe to call with the microphone stopped; returns 0.
 */
size_t fmMicRead(int16_t *pcm, size_t maxSamples);

/**
 * Install and start I2S TX, then bring the amplifier out of shutdown onto
 * already-silent DMA. Blocks for FM_AMP_SETTLE_MS while the MAX98357A wakes.
 */
bool fmSpeakerStart();

/**
 * Silence the DMA, let the block in flight finish, drop the amplifier back
 * into shutdown and release the peripheral. Blocks for one DMA buffer period
 * plus FM_AMP_SETTLE_MS - about 30 ms - which is the price of not clicking.
 */
void fmSpeakerStop();

/**
 * Queue `samples` of 8 kHz mono PCM. Returns how many were accepted; a short
 * count means FM_SPK_WRITE_TIMEOUT_MS expired with the DMA still full, and the
 * caller should re-offer the remainder. Note that after such a timeout the
 * anti-imaging filter state has already advanced over part of the unaccepted
 * tail, so a re-offer can produce a single-frame discontinuity - inaudible
 * next to the underrun that caused it, but worth knowing before chasing it.
 */
size_t fmSpeakerWrite(const int16_t *pcm, size_t samples);

/**
 * Directly gate the MAX98357A. fmSpeakerStart/Stop already do this; the hook
 * is exposed so a caller can hold the amplifier up across a gapless run of
 * clips instead of clicking between each one. A no-op when the board has no
 * PIN_AMP_SD.
 */
void fmAmpEnable(bool on);

/** Digital output attenuation, 0..100 percent. Values above 100 clamp to 100. */
void fmAudioSetVolume(uint8_t percent);
