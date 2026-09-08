/**
 * FloodMesh - I2S voice path implementation.
 * See fm_audio.h for why the microphone runs at 16 kHz, where the half-band
 * coefficients came from, and why the amplifier is gated.
 *
 * arduino-esp32 2.0.17 ships the LEGACY I2S driver only - driver/i2s.h with
 * i2s_config_t / i2s_driver_install. The i2s_std.h channel API that most
 * current examples use arrived in core 3.x and is NOT present here; do not
 * "modernise" this file without first checking that the header exists.
 *
 * All time arithmetic uses unsigned subtraction (now - then), which is correct
 * across the 49.7-day millis() rollover.
 */
#include "fm_audio.h"

#include <driver/i2s.h>
#include <string.h>

namespace {

const i2s_port_t kMicPort = (i2s_port_t)FM_MIC_I2S_PORT;
const i2s_port_t kSpkPort = (i2s_port_t)FM_SPK_I2S_PORT;

// ------------------------------------------------------- half-band FIR kernel
// 27 taps, Q15, symmetric, unity DC gain. Regenerate with the recipe in
// fm_audio.h; the static_asserts below check every property that recipe
// promises, so a mistranscribed digit fails the build rather than the ear.
const size_t kHbTaps   = 27;
const size_t kHbCentre = 13;

constexpr int16_t kHb[kHbTaps] = {
      64,     0,  -125,     0,   322,     0,  -719,     0,  1461,
       0, -3063,     0, 10252, 16384, 10252,     0, -3063,     0,
    1461,     0,  -719,     0,   322,     0,  -125,     0,    64,
};

// Recursive constexpr scans, in the same C++11-friendly style as the pin map.
constexpr int32_t hbSum(size_t i = 0) {
  return i >= kHbTaps ? 0 : (int32_t)kHb[i] + hbSum(i + 1);
}
constexpr int32_t hbAbsSum(size_t i = 0) {
  return i >= kHbTaps ? 0
       : (kHb[i] < 0 ? -(int32_t)kHb[i] : (int32_t)kHb[i]) + hbAbsSum(i + 1);
}
constexpr bool hbSymmetric(size_t i = 0) {
  return i >= kHbCentre                        ? true
       : (kHb[i] != kHb[kHbTaps - 1 - i])      ? false
                                               : hbSymmetric(i + 1);
}
/** Every even offset from the centre, except the centre itself, must be zero. */
constexpr bool hbZeros(size_t i = 0) {
  return i >= kHbTaps ? true
       : (i != kHbCentre &&
          ((i > kHbCentre ? i - kHbCentre : kHbCentre - i) % 2) == 0 &&
          kHb[i] != 0) ? false
       : hbZeros(i + 1);
}

static_assert(hbSum() == 32768,
              "half-band FIR must sum to exactly 32768: anything else is a DC "
              "gain error that scales every recorded sample.");
static_assert(kHb[kHbCentre] == 16384,
              "half-band centre tap must be exactly 0.5, or the interpolator's "
              "pass-through phase stops being unity gain.");
static_assert(hbSymmetric(), "FIR must stay symmetric or it is no longer linear phase.");
static_assert(hbZeros(), "half-band structure broken: an even-offset tap is non-zero.");
static_assert(sizeof(kHb) / sizeof(kHb[0]) == kHbTaps, "kHbTaps disagrees with the table.");
// Worst case is every sample at full scale aligned with the coefficient signs.
static_assert((int64_t)hbAbsSum() * 32767LL <= 2147483647LL,
              "FIR accumulator can overflow int32_t - shorten the filter or "
              "widen the accumulator.");

/**
 * Delay line stored twice, so the 27-sample window is always contiguous and
 * the inner product needs no modulo. pos is the next slot to overwrite, which
 * after any number of pushes is also the OLDEST sample of the window.
 */
struct Fir {
  int16_t z[kHbTaps * 2];
  uint8_t pos;
};

void firReset(Fir &f) {
  memset(f.z, 0, sizeof(f.z));
  f.pos = 0;
}

void firPush(Fir &f, int16_t s) {
  f.z[f.pos] = s;
  f.z[f.pos + kHbTaps] = s;
  f.pos = (uint8_t)((f.pos + 1u == kHbTaps) ? 0u : f.pos + 1u);
}

int32_t firDot(const Fir &f) {
  int32_t acc = 0;
  const int16_t *w = &f.z[f.pos];
  // The kernel is symmetric, so window orientation is irrelevant here.
  for (size_t k = 0; k < kHbTaps; k++) {
    acc += (int32_t)kHb[k] * (int32_t)w[k];
  }
  return acc;
}

// ------------------------------------------------------------ sample plumbing
/** A wrapped sample is a loud click, so every path out of int32 lands here. */
inline int16_t sat16(int32_t v) {
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return (int16_t)v;
}

/**
 * Q15 accumulator to sample, round-to-nearest then saturate. shift is 15 for
 * unity gain and 14 for the doubling the zero-stuffed interpolator needs -
 * folding the x2 into the shift keeps the accumulator's proven headroom.
 */
inline int16_t firOut(int32_t acc, uint8_t shift) {
  return sat16((acc + (1 << (shift - 1))) >> shift);
}

inline int16_t gainQ8(int16_t s, uint16_t g) {
  return sat16(((int32_t)s * (int32_t)g + 128) >> 8);
}

// ------------------------------------------------------------ module state
bool     g_begun  = false;
bool     g_micOn  = false;
bool     g_spkOn  = false;
uint16_t g_volQ8  = 0;

Fir      g_micFir;
uint8_t  g_micPhase   = 0;   // 1 = one input sample banked toward the next output
uint32_t g_micDiscard = 0;   // INMP441 start-up samples still to be thrown away

#if FM_SPK_UPSAMPLE
Fir g_spkFir;
#endif

const uint32_t kMicPrimeSamples =
    ((uint32_t)FM_MIC_PRIME_MS * (uint32_t)FM_MIC_I2S_HZ) / 1000u;

// Staging buffers. Fixed size, file scope: nothing here allocates at runtime.
// 512 B each, and the two are never in use at the same time.
const size_t kMicRawMax  = 128;                    // 32-bit slots per i2s_read
const size_t kSpkInChunk = 64;                     // 8 kHz samples per pass
const size_t kSpkRatio   = FM_SPK_UPSAMPLE ? 2 : 1;

int32_t g_micRaw[kMicRawMax];
int16_t g_spkStage[kSpkInChunk * kSpkRatio * 2];   // interleaved L,R

const uint32_t kSpkRateHz = (uint32_t)FM_AUDIO_HZ * (uint32_t)kSpkRatio;
// One DMA buffer period, rounded up. Zeroing the DMA buffers replaces queued
// audio with silence, but the block already handed to the peripheral still has
// to clock out before SD_MODE may safely fall.
const uint32_t kSpkDrainMs =
    ((uint32_t)FM_SPK_DMA_LEN * 1000u + kSpkRateHz - 1u) / kSpkRateHz;

// ------------------------------------------------------------ driver setup
/** Common fields; the caller fills in direction, rate, width and layout. */
i2s_config_t baseConfig() {
  i2s_config_t cfg = {};
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  // APLL is left off deliberately. It buys sub-ppm rate accuracy that speech
  // cannot use, and on the S3 it is one more shared clock resource to fight
  // over; the integer divider's few hundred ppm of error is inaudible.
  cfg.use_apll             = false;
  cfg.tx_desc_auto_clear   = false;
  cfg.fixed_mclk           = 0;
  cfg.mclk_multiple        = I2S_MCLK_MULTIPLE_DEFAULT;
  cfg.bits_per_chan        = I2S_BITS_PER_CHAN_DEFAULT;
  return cfg;
}

bool micInstall() {
  i2s_config_t cfg = baseConfig();
  cfg.mode            = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate     = FM_MIC_I2S_HZ;
  // 32-bit slots because the INMP441's 24 bits arrive left-justified in them.
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format  = FM_MIC_SWAP_CHANNEL ? I2S_CHANNEL_FMT_ONLY_RIGHT
                                            : I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.dma_buf_count   = FM_MIC_DMA_BUFS;
  cfg.dma_buf_len     = FM_MIC_DMA_LEN;

  esp_err_t e = i2s_driver_install(kMicPort, &cfg, 0, NULL);
  if (e != ESP_OK) {
    Serial.printf("[ERR ] i2s_driver_install(mic): %s\n", esp_err_to_name(e));
    return false;
  }

  i2s_pin_config_t pins = {};
  pins.mck_io_num   = I2S_PIN_NO_CHANGE;
  pins.bck_io_num   = PIN_MIC_SCK;
  pins.ws_io_num    = PIN_MIC_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num  = PIN_MIC_SD;

  e = i2s_set_pin(kMicPort, &pins);
  if (e != ESP_OK) {
    Serial.printf("[ERR ] i2s_set_pin(mic): %s\n", esp_err_to_name(e));
    i2s_driver_uninstall(kMicPort);
    return false;
  }
  return true;
}

bool spkInstall() {
  i2s_config_t cfg = baseConfig();
  cfg.mode            = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate     = kSpkRateHz;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  // Real stereo frames carrying the same mono sample twice: SD_MODE driven
  // high selects the amplifier's (L+R)/2 mix, so duplicating makes the output
  // level independent of which slot the breakout listens to.
  cfg.channel_format  = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.dma_buf_count   = FM_SPK_DMA_BUFS;
  cfg.dma_buf_len     = FM_SPK_DMA_LEN;
  // On underrun replay silence, not the stale buffer - a repeated 16 ms block
  // is a rasp, and an underrun is exactly when the operator is listening hard.
  cfg.tx_desc_auto_clear = true;

  esp_err_t e = i2s_driver_install(kSpkPort, &cfg, 0, NULL);
  if (e != ESP_OK) {
    Serial.printf("[ERR ] i2s_driver_install(spk): %s\n", esp_err_to_name(e));
    return false;
  }

  i2s_pin_config_t pins = {};
  pins.mck_io_num   = I2S_PIN_NO_CHANGE;
  pins.bck_io_num   = PIN_AMP_BCLK;
  pins.ws_io_num    = PIN_AMP_LRC;
  pins.data_out_num = PIN_AMP_DIN;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;

  e = i2s_set_pin(kSpkPort, &pins);
  if (e != ESP_OK) {
    Serial.printf("[ERR ] i2s_set_pin(spk): %s\n", esp_err_to_name(e));
    i2s_driver_uninstall(kSpkPort);
    return false;
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------- API
bool fmAudioBegin() {
#ifdef PIN_AMP_SD
  pinMode(PIN_AMP_SD, OUTPUT);
  digitalWrite(PIN_AMP_SD, LOW);   // shutdown is the resting state
#else
  Serial.println("[WARN] no PIN_AMP_SD on this board: the MAX98357A cannot be "
                 "gated and draws ~2 mA whenever the rail is up.");
#endif

  firReset(g_micFir);
#if FM_SPK_UPSAMPLE
  firReset(g_spkFir);
#endif
  g_micPhase   = 0;
  g_micDiscard = 0;
  fmAudioSetVolume(FM_AUDIO_VOL_DEFAULT);

  if (g_begun) return true;

  // Prove the capture configuration once, here, so a mistyped pin or an
  // impossible clock fails at boot instead of on the first PTT press - when it
  // would cost a message rather than a log line.
  if (!micInstall()) {
    Serial.println("[ERR ] I2S capture configuration rejected; voice is unavailable.");
    return false;
  }
  i2s_driver_uninstall(kMicPort);

  g_begun = true;
  return true;
}

bool fmMicStart() {
  if (!g_begun) {
    Serial.println("[WARN] fmMicStart before fmAudioBegin.");
    return false;
  }
  if (g_spkOn) {
    Serial.println("[WARN] fmMicStart refused: the speaker is running.");
    return false;
  }
  if (g_micOn) {
    Serial.println("[WARN] fmMicStart called twice; already capturing.");
    return false;
  }
  if (!micInstall()) return false;

  firReset(g_micFir);
  g_micPhase = 0;
  // The INMP441's decimator needs its clock for a while before its output
  // means anything. Those samples are dropped WITHOUT going through the FIR:
  // priming the filter with start-up garbage would smear it across the first
  // real samples, whereas letting the filter ramp from zero costs a 1.6 ms
  // fade-in nobody can hear.
  g_micDiscard = kMicPrimeSamples;

  i2s_zero_dma_buffer(kMicPort);
  esp_err_t e = i2s_start(kMicPort);
  if (e != ESP_OK) {
    Serial.printf("[ERR ] i2s_start(mic): %s\n", esp_err_to_name(e));
    i2s_driver_uninstall(kMicPort);
    return false;
  }
  g_micOn = true;
  return true;
}

void fmMicStop() {
  if (!g_micOn) return;
  i2s_stop(kMicPort);
  i2s_driver_uninstall(kMicPort);   // give the DMA buffers back while idle
  g_micOn = false;
}

size_t fmMicRead(int16_t *pcm, size_t maxSamples) {
  if (!g_micOn || pcm == NULL || maxSamples == 0) return 0;

  const uint32_t t0 = millis();
  size_t out = 0;

  while (out < maxSamples) {
    const uint32_t elapsed = millis() - t0;
    if (elapsed >= FM_MIC_READ_TIMEOUT_MS) break;

    // Two input samples per output, less the one already banked mid-pair, plus
    // whatever start-up settling is still to be discarded. Clamped before the
    // doubling so a huge maxSamples cannot overflow the arithmetic.
    size_t need = maxSamples - out;
    if (need > kMicRawMax) need = kMicRawMax;
    size_t want = need * 2u - (g_micPhase ? 1u : 0u) + (size_t)g_micDiscard;
    if (want > kMicRawMax) want = kMicRawMax;

    size_t got = 0;
    const esp_err_t e = i2s_read(kMicPort, g_micRaw, want * sizeof(int32_t), &got,
                                 pdMS_TO_TICKS(FM_MIC_READ_TIMEOUT_MS - elapsed));
    if (e != ESP_OK) break;

    // The request is a whole number of 32-bit slots and the driver copies whole
    // DMA frames, so got is slot-aligned; the division cannot silently lose a
    // partial sample and desynchronise the stream.
    const size_t n = got / sizeof(int32_t);
    if (n == 0) break;   // the timeout expired with nothing to show for it

#if FM_MIC_RAW_DEBUG
    // Dump the untouched 32-bit slots once per capture. All-zero words mean the
    // SD line is dead - wiring, power, or the wrong pin. Non-zero words that
    // still decode to silence would instead mean the shift is wrong, which is a
    // completely different fault, and no amount of staring at the int16 output
    // can tell the two apart.
    {
      static uint8_t dumps = 0;
      if (dumps < 3) {
        dumps++;
        Serial.printf("[MIC RAW] %08lX %08lX %08lX %08lX %08lX %08lX\n",
                      (unsigned long)g_micRaw[0], (unsigned long)g_micRaw[1],
                      (unsigned long)g_micRaw[2], (unsigned long)g_micRaw[3],
                      (unsigned long)g_micRaw[4], (unsigned long)g_micRaw[5]);
      }
    }
#endif

    for (size_t i = 0; i < n; i++) {
      if (g_micDiscard) {
        g_micDiscard--;
        continue;
      }
      // 24 bits left-justified in a 32-bit slot: the int16 is the TOP half of
      // the word. Truncating to the bottom half would keep 8 hard-wired zeros
      // and the noise floor.
      firPush(g_micFir, (int16_t)(g_micRaw[i] >> 16));
      g_micPhase ^= 1u;
      if (g_micPhase) continue;              // first half of the pair
      if (out >= maxSamples) break;          // want is sized so this cannot
      pcm[out++] = gainQ8(firOut(firDot(g_micFir), 15), FM_MIC_GAIN_Q8);
    }                                        // fire, but pcm[] is never trusted
  }                                          // to a computed index unchecked
  return out;
}

bool fmSpeakerStart() {
  if (!g_begun) {
    Serial.println("[WARN] fmSpeakerStart before fmAudioBegin.");
    return false;
  }
  if (g_micOn) {
    Serial.println("[WARN] fmSpeakerStart refused: the microphone is running.");
    return false;
  }
  if (g_spkOn) {
    Serial.println("[WARN] fmSpeakerStart called twice; already playing.");
    return false;
  }
  if (!spkInstall()) return false;

#if FM_SPK_UPSAMPLE
  firReset(g_spkFir);
#endif
  i2s_zero_dma_buffer(kSpkPort);
  esp_err_t e = i2s_start(kSpkPort);
  if (e != ESP_OK) {
    Serial.printf("[ERR ] i2s_start(spk): %s\n", esp_err_to_name(e));
    i2s_driver_uninstall(kSpkPort);
    return false;
  }
  g_spkOn = true;

  // SD_MODE rises only once the DMA is running on zeroed buffers, so the first
  // thing the amplifier amplifies is silence rather than a floating line.
  fmAmpEnable(true);
  delay(FM_AMP_SETTLE_MS);
  return true;
}

void fmSpeakerStop() {
  if (!g_spkOn) return;

  // Cutting SD_MODE while a non-zero sample is on the output leaves a step the
  // amplifier reproduces as a click. Replace the queued audio with silence and
  // let the block already in flight clock out first.
  i2s_zero_dma_buffer(kSpkPort);
  delay(kSpkDrainMs + FM_AMP_SETTLE_MS);
  fmAmpEnable(false);

  i2s_stop(kSpkPort);
  i2s_driver_uninstall(kSpkPort);
  g_spkOn = false;
}

size_t fmSpeakerWrite(const int16_t *pcm, size_t samples) {
  if (!g_spkOn || pcm == NULL || samples == 0) return 0;

  const uint32_t t0 = millis();
  size_t done = 0;

  while (done < samples) {
    const uint32_t elapsed = millis() - t0;
    if (elapsed >= FM_SPK_WRITE_TIMEOUT_MS) break;

    size_t n = samples - done;
    if (n > kSpkInChunk) n = kSpkInChunk;

    size_t frames = 0;
    for (size_t i = 0; i < n; i++) {
      // Volume first: it is half the multiplies of scaling after interpolation,
      // and it proportionally reduces how hard the interpolator can overshoot.
      const int16_t s = gainQ8(pcm[done + i], g_volQ8);
#if FM_SPK_UPSAMPLE
      // Zero-stuff to 2x and filter. Zero-stuffing halves the amplitude, so the
      // output shift is 14 rather than 15; the accumulator itself never sees
      // the doubling and keeps the headroom the static_assert proved.
      firPush(g_spkFir, s);
      const int16_t a = firOut(firDot(g_spkFir), 14);
      firPush(g_spkFir, 0);
      const int16_t b = firOut(firDot(g_spkFir), 14);
      g_spkStage[frames * 2 + 0] = a;
      g_spkStage[frames * 2 + 1] = a;
      frames++;
      g_spkStage[frames * 2 + 0] = b;
      g_spkStage[frames * 2 + 1] = b;
      frames++;
#else
      g_spkStage[frames * 2 + 0] = s;
      g_spkStage[frames * 2 + 1] = s;
      frames++;
#endif
    }

    const size_t bytes = frames * 2u * sizeof(int16_t);
    size_t sent = 0;
    while (sent < bytes) {
      const uint32_t e2 = millis() - t0;
      if (e2 >= FM_SPK_WRITE_TIMEOUT_MS) break;
      size_t w = 0;
      const esp_err_t e = i2s_write(kSpkPort, (const uint8_t *)g_spkStage + sent,
                                    bytes - sent, &w,
                                    pdMS_TO_TICKS(FM_SPK_WRITE_TIMEOUT_MS - e2));
      if (e != ESP_OK || w == 0) break;
      sent += w;
    }

    // Only input samples whose frames all reached the DMA count as consumed.
    // On a short write the filter state has already run past the rest - see the
    // note on fmSpeakerWrite in fm_audio.h.
    done += (sent / (2u * sizeof(int16_t))) / kSpkRatio;
    if (sent < bytes) break;
  }
  return done;
}

void fmAmpEnable(bool on) {
#ifdef PIN_AMP_SD
  digitalWrite(PIN_AMP_SD, on ? HIGH : LOW);
#else
  (void)on;   // nothing to gate; the warning was issued once at begin
#endif
}

void fmAudioSetVolume(uint8_t percent) {
  // Clamped at unity on purpose: a digital gain above 1.0 cannot make the
  // 8 ohm cavity speaker louder, only clipped. Headroom is the point.
  if (percent > 100) percent = 100;
  g_volQ8 = (uint16_t)(((uint32_t)percent * 256u + 50u) / 100u);
}
