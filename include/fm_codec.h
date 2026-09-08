/**
 * FloodMesh - Codec2 1200 bps encoder/decoder wrapper.
 *
 * Codec2 mode 1200 is the single arithmetic assumption the whole Layer 2 voice
 * path rests on: 320 samples in (40 ms at 8 kHz), 48 bits out - exactly 6 bytes,
 * no padding. fm_packet.h turns that one fact into 250 frames = 1500 bytes =
 * 8 fragments with no rounding anywhere. If the library ever disagreed, every
 * one of those numbers would be silently wrong and the receiver would reassemble
 * garbage, so fmCodecBegin() asks the library its own opinion at boot and
 * refuses to run when it does not match. See "geometry mismatch" below.
 *
 * Why a wrapper at all, instead of calling codec2_*() directly
 * -----------------------------------------------------------
 *  - One owner for the ~31 KB of heap a codec state costs. On a 320 KB part with
 *    no PSRAM, a second accidental codec2_create() is 10% of RAM gone; making
 *    the state file-local means it cannot happen.
 *  - codec2_encode() takes a NON-const short*, but every call site here holds a
 *    const capture buffer. The cast that reconciles that is audited once, in
 *    fm_codec.cpp, against the library source - not sprinkled around.
 *  - The geometry gate has to live somewhere, once, before any audio moves.
 *
 * COST 1 - HEAP: 31,192 bytes, internal DRAM
 * ------------------------------------------
 * Counted from the library source for CODEC2_MODE_1200 at Fs=8000 (n_samp=80,
 * m_pitch=320), in 11 malloc blocks:
 *
 *   struct CODEC2                     4,400   (mostly W[512] + prev_model_dec)
 *   Pn, Sn_ (2 x 2*n_samp floats)     1,280
 *   w, Sn   (2 x m_pitch floats)      2,560
 *   fft_fwd_cfg   kiss_fft   512      4,360
 *   fftr_fwd_cfg  kiss_fftr  512      5,396
 *   fftr_inv_cfg  kiss_fftr  512      5,396
 *   nlp state + its kiss_fft 512      6,116
 *   bpf_buf (BPF_N + 4*n_samp)        1,684
 *
 * The ~1.1 MB of VQ codebooks are const and live in flash, not here: the library
 * defines __EMBEDDED__ in its defines.h, which is what makes lsp_codebook::cb a
 * const pointer. Do not "clean that up".
 *
 * One state serves both directions - codec2 is designed full-duplex - so there
 * is deliberately no separate encoder and decoder instance. Allocation happens
 * in fmCodecBegin() at setup() time and never again.
 *
 * COST 2 - STACK: ~15.8 KB. This is the one that will bite you.
 * ------------------------------------------------------------
 * Measured with xtensa-esp32s3-elf-gcc -Os -fstack-usage on the library source.
 * The deepest call chains:
 *
 *   encode: codec2_encode_1200 1,520 -> analyse_one_frame 4,176 -> nlp 4,160
 *           -> codec2_fft_inplace 4,128 -> kiss_fft chain ~480   = ~14.5 KB
 *   decode: codec2_decode_1200 9,776 -> synthesise_one_frame 1,344
 *           -> synthesise 4,176 -> kiss_fft chain ~480           = ~15.8 KB
 *
 * codec2_decode_1200 alone puts 9.7 KB on the stack (MODEL model[4] is 5.2 KB
 * and COMP Aw[512] another 4 KB). The Arduino loop task gets 8,192 bytes by
 * default, so calling either function from loop() as shipped overflows the stack
 * and reboots the device - and it reboots inside a distress call, which is the
 * worst possible time to find out.
 *
 * Fix it one of two ways:
 *   platformio.ini:  -D ARDUINO_LOOP_STACK_SIZE=24576
 *   or run the codec on its own task: xTaskCreate(..., 24576, ...)
 *
 * Because a stack overflow here is a crash rather than an error code, the first
 * call in each direction checks the calling task's headroom and REFUSES (returns
 * false) instead of smashing the stack. A refusal costs voice; a crash costs the
 * alarms too. FM_CODEC_MIN_STACK_BYTES tunes or disables that guard.
 *
 * COST 3 - TIME: roughly 8-15 ms per 40 ms frame (estimate; the device measures
 * it for real at boot - see FM_CODEC_SELFTEST). One frame is 8 x 512-point float
 * FFTs plus voicing estimation, two LPC/LSP conversions and a 3-stage VQ search,
 * so it should land near a quarter to a third of real time on a 240 MHz S3.
 *
 * The consequence is a design constraint, not a footnote: ENCODE WHILE
 * CAPTURING, one frame per 40 ms of audio. Encoding the whole clip after PTT
 * release would need 250 x that = 2-4 seconds of dead air before the first
 * fragment goes out.
 *
 * PCM is never buffered whole
 * ---------------------------
 * 10 s of 16-bit 8 kHz audio is 160,000 bytes - half the usable DRAM on this
 * part, and it does not exist alongside a radio stack and a display buffer. The
 * compressed clip is 1,500 bytes. That gap is precisely why capture feeds this
 * module 320 samples at a time and keeps only the 6-byte results.
 *
 * What happens on a geometry mismatch
 * -----------------------------------
 * If codec2_samples_per_frame() != 320 or codec2_bits_per_frame() != 48,
 * fmCodecBegin() prints both the expected and the actual values, frees the
 * state, and returns false. It does NOT adapt, and it does not let the caller
 * proceed with whatever the library offered instead: fm_packet.h's 1500-byte,
 * 8-fragment layout is compiled in, so a different frame size would produce
 * fragments that no receiver on the mesh - including an identical unit running
 * an identical build - could reassemble. Voice is then unavailable for the whole
 * session; Layer 1 alarms are entirely independent of this module and keep
 * working. That is the intended failure mode: degrade, never mislead.
 *
 * The only realistic trigger is a build that disables mode 1200 (the library
 * enables every mode by default; -D CODEC2_MODE_EN_DEFAULT=0 without
 * -D CODEC2_MODE_1200_EN=1 would do it) or a library upgrade that changes the
 * mode table. Both deserve a hard stop.
 */
#pragma once
#include <Arduino.h>

#include "fm_packet.h"

// ---------------------------------------------------------------- tunables
#ifndef FM_CODEC_MIN_STACK_BYTES
#define FM_CODEC_MIN_STACK_BYTES 20480  // measured worst case 15.8 kB + headroom.
#endif                                  // 0 disables the guard - only do that if
                                        // you have proven the stack another way.
#ifndef FM_CODEC_SELFTEST
#define FM_CODEC_SELFTEST 1        // time one encode + one decode at boot and
#endif                             // print the result. ~100 ms, once, and it is
                                   // the only honest source for COST 3 above.
#ifndef FM_CODEC_SELFTEST_FRAMES
#define FM_CODEC_SELFTEST_FRAMES 4 // averaged over this many frames
#endif

// ------------------------------------------------- geometry (facts, not knobs)
// These are properties of CODEC2_MODE_1200. They are #defines rather than calls
// so buffers can be sized at compile time; fmCodecBegin() proves at runtime that
// the linked library still agrees with them.
#define FM_CODEC_SAMPLE_RATE_HZ      8000
#define FM_CODEC_SAMPLES_PER_FRAME   320
#define FM_CODEC_BITS_PER_FRAME      48
#define FM_CODEC_BYTES_PER_FRAME     FM_C2_FRAME_BYTES
#define FM_CODEC_PCM_BYTES_PER_FRAME (FM_CODEC_SAMPLES_PER_FRAME * 2)

static_assert(FM_CODEC_BYTES_PER_FRAME == 6,
              "Codec2 1200 must be 6 B/frame or fm_packet.h's 1500 B is wrong");
static_assert((FM_CODEC_BITS_PER_FRAME + 7) / 8 == FM_CODEC_BYTES_PER_FRAME,
              "48 bits must pack into 6 whole bytes with nothing left over");
static_assert(FM_CODEC_SAMPLES_PER_FRAME * 1000 ==
                  FM_CODEC_SAMPLE_RATE_HZ * FM_C2_FRAME_MS,
              "320 samples at 8 kHz must be exactly FM_C2_FRAME_MS of audio");
static_assert(FM_CODEC_PCM_BYTES_PER_FRAME == 640, "int16 PCM frame is 640 B");

// ---------------------------------------------------------------- API
/**
 * Allocate the codec state and prove the library's geometry matches fm_packet.h.
 * Call once from setup(); this is the only allocation the module makes.
 *
 * Returns false - with the reason printed - if the state will not allocate, or
 * if the library reports anything other than 320 samples / 48 bits per frame.
 * Treat false as "voice is unavailable this session" and carry on: alarms do not
 * route through here. Calling it again when already open is a no-op that
 * returns true.
 */
bool fmCodecBegin();

/**
 * Free the codec state. Exists for completeness and for a clean teardown in
 * tests; in the shipped firmware the codec is held for the life of the device,
 * because re-allocating 31 KB against a fragmented heap can fail exactly when a
 * voice note is wanted.
 */
void fmCodecEnd();

/** 320. Reported by the library while open, the compile-time constant if not. */
int fmCodecSamplesPerFrame();

/** 6. Reported by the library while open, the compile-time constant if not. */
int fmCodecBytesPerFrame();

/**
 * Compress one frame: exactly FM_CODEC_SAMPLES_PER_FRAME int16 samples at
 * 8 kHz in, exactly FM_CODEC_BYTES_PER_FRAME bytes out. Both lengths are the
 * caller's promise - there is no length argument to check, so get the buffers
 * right at the call site.
 *
 * Returns false if the codec is not open, either pointer is null, or the calling
 * task does not have enough stack (see COST 2). It never partially writes out.
 */
bool fmCodecEncodeFrame(const int16_t *pcm, uint8_t *out);

/**
 * Decompress one frame: FM_CODEC_BYTES_PER_FRAME bytes in,
 * FM_CODEC_SAMPLES_PER_FRAME int16 samples out.
 *
 * Safe on arbitrary bytes off the air. Every field mode 1200 unpacks is exactly
 * as wide as its codebook index (9+9+9 bits into three 512-entry LSP codebooks,
 * 8 bits into a 256-entry Wo/energy codebook), so no 48-bit pattern can index
 * past the end of a table, and decode_WoE() clamps Wo into range afterwards. A
 * corrupt fragment therefore produces noise, never a fault - which is why a
 * damaged clip is still played rather than dropped.
 *
 * Returns false under the same conditions as fmCodecEncodeFrame().
 */
bool fmCodecDecodeFrame(const uint8_t *in, int16_t *pcm);
