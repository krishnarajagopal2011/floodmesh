/**
 * FloodMesh - Codec2 1200 bps wrapper implementation.
 * See fm_codec.h for the geometry contract and the heap/stack/time budgets.
 *
 * All time arithmetic uses unsigned subtraction (now - then), which is correct
 * across the micros() rollover as well as the millis() one.
 */
#include "fm_codec.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern "C" {
#include <codec2.h>
}

// int16_t and short are the same type on xtensa-esp32s3, but the encode path
// below reinterprets one as the other to satisfy codec2's signature, so make the
// assumption fail loudly rather than silently on any future port.
static_assert(sizeof(int16_t) == sizeof(short),
              "codec2 speaks short[]; int16_t must be the same width");

namespace {

struct CODEC2 *g_c2 = NULL;

// One-shot stack verdicts, per direction. They MUST be one-shot: the high water
// mark reports the least stack ever free, so after the first 15 KB codec call it
// legitimately reads ~5 KB and a repeated test would refuse every frame from the
// second one onward. The first call in each direction is the only moment the
// measurement means "headroom still available".
bool g_encChecked = false;
bool g_encOk      = false;
bool g_decChecked = false;
bool g_decOk      = false;

uint32_t g_heapCost = 0;   // measured across the first codec2_create()

/** Free stack of the CALLING task, in bytes (ESP-IDF returns bytes, not words). */
uint32_t taskStackFree() {
  return (uint32_t)uxTaskGetStackHighWaterMark(NULL);
}

/**
 * Refuse rather than overflow. A codec call needs ~15.8 kB of stack; the Arduino
 * loop task ships with 8 kB. Returning false costs the operator a voice note,
 * overflowing costs them the whole device mid-transmission.
 */
bool stackGuard(bool &checked, bool &verdict, const char *dir) {
  if (checked) return verdict;
  checked = true;
#if FM_CODEC_MIN_STACK_BYTES > 0
  const uint32_t freeBytes = taskStackFree();
  verdict = (freeBytes >= (uint32_t)FM_CODEC_MIN_STACK_BYTES);
  if (!verdict) {
    Serial.printf("[ERR ] codec2 %s refused: task '%s' has %u B stack free, "
                  "needs %d B. Voice off, alarms unaffected. Fix with "
                  "-D ARDUINO_LOOP_STACK_SIZE=24576 or a dedicated task.\n",
                  dir, pcTaskGetName(NULL), (unsigned)freeBytes,
                  (int)FM_CODEC_MIN_STACK_BYTES);
  }
#else
  (void)dir;
  verdict = true;
#endif
  return verdict;
}

/**
 * Confirm the linked library still uses the frame geometry every byte of
 * fm_packet.h was derived from. Prints expected vs actual on failure so the
 * cause (a mode disabled at compile time, or a library upgrade) is obvious from
 * the boot log alone.
 */
bool geometryMatches(struct CODEC2 *c2) {
  const int samples = codec2_samples_per_frame(c2);
  const int bits    = codec2_bits_per_frame(c2);
  const int bytes   = codec2_bytes_per_frame(c2);

  if (samples == FM_CODEC_SAMPLES_PER_FRAME && bits == FM_CODEC_BITS_PER_FRAME &&
      bytes == FM_CODEC_BYTES_PER_FRAME) {
    return true;
  }

  Serial.printf("[ERR ] codec2 mode 1200 geometry mismatch: got %d samples / "
                "%d bits / %d bytes per frame, need %d / %d / %d.\n",
                samples, bits, bytes, FM_CODEC_SAMPLES_PER_FRAME,
                FM_CODEC_BITS_PER_FRAME, FM_CODEC_BYTES_PER_FRAME);
  Serial.println("[ERR ] fm_packet.h's 1500 B / 8-fragment layout is compiled "
                 "in and cannot follow. Voice disabled; alarms unaffected.");
  return false;
}

#if FM_CODEC_SELFTEST
/**
 * Time a real encode and a real decode on this silicon, because the whole
 * capture design depends on one frame costing well under its own 40 ms of audio
 * and no datasheet will tell you that number.
 *
 * The test tone is voiced rather than silence: the pitch estimator and the VQ
 * search are the expensive parts, and silence does not exercise them honestly.
 */
void selfTest(struct CODEC2 *c2) {
  const uint32_t stackFree = taskStackFree();
  if (stackFree < (uint32_t)FM_CODEC_MIN_STACK_BYTES) {
    Serial.printf("[WARN] codec2 self-test skipped: only %u B of stack free on "
                  "task '%s'. Encoding will be refused for the same reason.\n",
                  (unsigned)stackFree, pcTaskGetName(NULL));
    return;
  }

  int16_t pcm[FM_CODEC_SAMPLES_PER_FRAME];
  uint8_t bits[FM_CODEC_BYTES_PER_FRAME];
  uint32_t encUs = 0;
  uint32_t decUs = 0;

  for (int f = 0; f < FM_CODEC_SELFTEST_FRAMES; f++) {
    for (int i = 0; i < FM_CODEC_SAMPLES_PER_FRAME; i++) {
      const int n = f * FM_CODEC_SAMPLES_PER_FRAME + i;
      pcm[i] = (int16_t)(8000.0f * sinf(2.0f * (float)PI * 300.0f * (float)n /
                                        (float)FM_CODEC_SAMPLE_RATE_HZ));
    }

    uint32_t t0 = micros();
    codec2_encode(c2, bits, pcm);
    encUs += micros() - t0;

    t0 = micros();
    codec2_decode(c2, pcm, bits);
    decUs += micros() - t0;
  }

  const uint32_t enc = encUs / FM_CODEC_SELFTEST_FRAMES;
  const uint32_t dec = decUs / FM_CODEC_SELFTEST_FRAMES;
  Serial.printf("[INFO] codec2 1200: encode %u us, decode %u us per %d ms "
                "frame (%u%% / %u%% of real time).\n",
                (unsigned)enc, (unsigned)dec, FM_C2_FRAME_MS,
                (unsigned)((enc * 100u) / (FM_C2_FRAME_MS * 1000u)),
                (unsigned)((dec * 100u) / (FM_C2_FRAME_MS * 1000u)));
  if (enc >= (uint32_t)FM_C2_FRAME_MS * 1000u) {
    Serial.println("[WARN] encode is slower than real time - capture cannot "
                   "keep up. Check the CPU is at 240 MHz.");
  }
}
#endif  // FM_CODEC_SELFTEST

}  // namespace

// ---------------------------------------------------------------- API
bool fmCodecBegin() {
  if (g_c2 != NULL) return true;

  const uint32_t heapBefore = ESP.getFreeHeap();
  g_c2 = codec2_create(CODEC2_MODE_1200);
  if (g_c2 == NULL) {
    Serial.printf("[ERR ] codec2_create(CODEC2_MODE_1200) failed. Either mode "
                  "1200 was compiled out, or ~31 KB would not fit in the %u B "
                  "free heap. Voice disabled; alarms unaffected.\n",
                  (unsigned)heapBefore);
    return false;
  }
  g_heapCost = heapBefore - ESP.getFreeHeap();

  if (!geometryMatches(g_c2)) {
    codec2_destroy(g_c2);
    g_c2 = NULL;
    return false;
  }

#if FM_CODEC_SELFTEST
  selfTest(g_c2);

  // The self-test leaves the pitch tracker, the Wo/energy VQ predictor and the
  // 320-sample analysis history primed with a 300 Hz tone. That would colour the
  // first frame or two of the first real recording - which in this device is
  // someone's opening word - so start the shipped codec from a clean state.
  codec2_destroy(g_c2);
  g_c2 = codec2_create(CODEC2_MODE_1200);
  if (g_c2 == NULL) {
    Serial.println("[ERR ] codec2 re-create after self-test failed. Voice "
                   "disabled; alarms unaffected.");
    return false;
  }
#endif

  // Re-measure the stack on first use: begin() may well run on a different task
  // from the one that ends up encoding.
  g_encChecked = g_decChecked = false;
  g_encOk = g_decOk = false;
  Serial.printf("[INFO] codec2 1200 ready: %d samples / %d bits per frame, "
                "%u B heap, %u B free.\n",
                FM_CODEC_SAMPLES_PER_FRAME, FM_CODEC_BITS_PER_FRAME,
                (unsigned)g_heapCost, (unsigned)ESP.getFreeHeap());
  return true;
}

void fmCodecEnd() {
  if (g_c2 == NULL) return;
  codec2_destroy(g_c2);
  g_c2 = NULL;
  g_encChecked = g_decChecked = false;
  g_encOk = g_decOk = false;
}

int fmCodecSamplesPerFrame() {
  return g_c2 != NULL ? codec2_samples_per_frame(g_c2)
                      : FM_CODEC_SAMPLES_PER_FRAME;
}

int fmCodecBytesPerFrame() {
  return g_c2 != NULL ? codec2_bytes_per_frame(g_c2) : FM_CODEC_BYTES_PER_FRAME;
}

bool fmCodecEncodeFrame(const int16_t *pcm, uint8_t *out) {
  if (g_c2 == NULL || pcm == NULL || out == NULL) return false;
  if (!stackGuard(g_encChecked, g_encOk, "encode")) return false;

  // codec2_encode() declares its input as short[] rather than const short[], but
  // the mode 1200 path only ever copies from it: codec2_encode_1200() passes the
  // buffer to analyse_one_frame(), whose sole use is filling the codec's own
  // c2->Sn history. Verified against codec2.c in sh123/esp32_codec2 1.0.7. The
  // cast is here, once, so no caller has to hand over a mutable capture buffer.
  short *speech = reinterpret_cast<short *>(const_cast<int16_t *>(pcm));
  codec2_encode(g_c2, out, speech);
  return true;
}

bool fmCodecDecodeFrame(const uint8_t *in, int16_t *pcm) {
  if (g_c2 == NULL || in == NULL || pcm == NULL) return false;
  if (!stackGuard(g_decChecked, g_decOk, "decode")) return false;

  codec2_decode(g_c2, reinterpret_cast<short *>(pcm), in);
  return true;
}
