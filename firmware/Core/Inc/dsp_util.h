/*
 * dsp_util.h - Shared inline DSP building blocks for the algorithms.
 * Per-sample code must avoid libm; sinf/powf/expf belong in update().
 */
#ifndef DSP_UTIL_H
#define DSP_UTIL_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "fx_algo.h"

#define DSP_FS        ((float)FX_SAMPLE_RATE)
#define DSP_PI        3.14159265f
#define DSP_TWO_PI    6.28318531f

/* Added inside recursive filters so silence settles on a normal float instead
 * of decaying into denormals (FPSCR.FZ is not relied upon). */
#define DSP_ANTI_DENORM 1e-18f

/* ~10 ms one-pole at 48 kHz: 1 - exp(-1 / (0.010 * 48000)). */
#define DSP_SMOOTH_K    0.00208f
#define DSP_SMOOTH_SNAP 1e-4f

/* Phase accumulator increment per Hz: 2^32 / fs. */
#define DSP_PHASE_PER_HZ (4294967296.0f / DSP_FS)

static inline float dsp_clampf(float v, float lo, float hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

static inline float dsp_ms_to_samples(float ms)
{
  return ms * (DSP_FS / 1000.0f);
}

static inline float dsp_db_to_lin(float db)
{
  return powf(10.0f, db * 0.05f);
}

/* One-pole coefficient for a cutoff in Hz (update() only). */
static inline float dsp_onepole_k(float hz)
{
  return 1.0f - expf(-DSP_TWO_PI * hz / DSP_FS);
}

/* Rational tanh approximation, exact +-1 beyond |x| = 3. */
static inline float dsp_tanh(float x)
{
  x = dsp_clampf(x, -3.0f, 3.0f);
  const float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* ---- G.711 mu-law on 16-bit samples (monotonic: order is preserved) ----- */

static inline uint8_t dsp_ulaw_encode(int16_t sample)
{
  int32_t s = sample;
  uint32_t sign = 0;
  if (s < 0)
  {
    s = -s;
    sign = 0x80;
  }
  s = (s > 32635 ? 32635 : s) + 0x84;
  const uint32_t exp = (uint32_t)(31 - __builtin_clz((uint32_t)s)) - 7u;
  const uint32_t mant = ((uint32_t)s >> (exp + 3u)) & 0x0Fu;
  return (uint8_t)~(sign | (exp << 4) | mant);
}

static inline int16_t dsp_ulaw_decode(uint8_t byte)
{
  const uint32_t u = (uint8_t)~byte;
  const int32_t t = (int32_t)((((u & 0x0Fu) << 3) + 0x84u) << ((u & 0x70u) >> 4)) - 0x84;
  return (int16_t)((u & 0x80u) ? -t : t);
}

/* ---- LFO: phase accumulator, full circle = 2^32 ------------------------- */

static inline uint32_t dsp_phase_inc(float hz)
{
  return (uint32_t)(hz * DSP_PHASE_PER_HZ);
}

/* Parabolic sine, error < 0.1 %. */
static inline float dsp_lfo_sin(uint32_t ph)
{
  const float x = (float)(int32_t)ph * (1.0f / 2147483648.0f);
  const float y = 4.0f * x * (1.0f - fabsf(x));
  return y + 0.225f * (y * fabsf(y) - y);
}

static inline float dsp_lfo_tri(uint32_t ph)
{
  const float x = (float)(int32_t)ph * (1.0f / 2147483648.0f);
  return 1.0f - 2.0f * fabsf(x);
}

/* ---- Parameter smoother -------------------------------------------------- */

typedef struct
{
  float cur;
  float tgt;
} dsp_smooth_t;

/* Snapping lets a target of 0 become exactly 0 so work can be skipped. */
static inline float dsp_smooth_next(dsp_smooth_t *s)
{
  const float d = s->tgt - s->cur;
  if (fabsf(d) < DSP_SMOOTH_SNAP)
  {
    s->cur = s->tgt;
  }
  else
  {
    s->cur += DSP_SMOOTH_K * d;
  }
  return s->cur;
}

static inline void dsp_smooth_snap(dsp_smooth_t *s, float v)
{
  s->cur = v;
  s->tgt = v;
}

/* ---- Biquad (transposed direct form II) ---------------------------------- */

typedef struct
{
  float b0, b1, b2, a1, a2;
} dsp_biquad_coef_t;

typedef struct
{
  float z1, z2;
} dsp_biquad_state_t;

static inline float dsp_biquad_run(const dsp_biquad_coef_t *c, dsp_biquad_state_t *s, float x)
{
  const float y = c->b0 * x + s->z1;
  s->z1 = c->b1 * x - c->a1 * y + s->z2 + DSP_ANTI_DENORM;
  s->z2 = c->b2 * x - c->a2 * y;
  return y;
}

static inline void dsp_biquad_norm(dsp_biquad_coef_t *c, float b0, float b1, float b2,
                                   float a0, float a1, float a2)
{
  const float inv = 1.0f / a0;
  c->b0 = b0 * inv;
  c->b1 = b1 * inv;
  c->b2 = b2 * inv;
  c->a1 = a1 * inv;
  c->a2 = a2 * inv;
}

/* RBJ shelf, S = 1. A high shelf is the low shelf with cos(w0) negated and the
 * odd-order coefficients flipped in sign. */
static inline void dsp_biquad_shelf(dsp_biquad_coef_t *c, bool high, float f0, float gain_db)
{
  const float A = powf(10.0f, gain_db * (1.0f / 40.0f));
  const float w0 = DSP_TWO_PI * f0 / DSP_FS;
  const float sgn = high ? -1.0f : 1.0f;
  const float cw = sgn * cosf(w0);
  const float t = 2.0f * sqrtf(A) * 0.70710678f * sinf(w0);
  const float k = (A - 1.0f) * cw;

  dsp_biquad_norm(c,
                  A * ((A + 1.0f) - k + t),
                  sgn * 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cw),
                  A * ((A + 1.0f) - k - t),
                  (A + 1.0f) + k + t,
                  sgn * -2.0f * ((A - 1.0f) + (A + 1.0f) * cw),
                  (A + 1.0f) + k - t);
}

static inline void dsp_biquad_peak(dsp_biquad_coef_t *c, float f0, float q, float gain_db)
{
  const float A = powf(10.0f, gain_db * (1.0f / 40.0f));
  const float w0 = DSP_TWO_PI * f0 / DSP_FS;
  const float cw = cosf(w0);
  const float alpha = sinf(w0) / (2.0f * q);

  dsp_biquad_norm(c, 1.0f + alpha * A, -2.0f * cw, 1.0f - alpha * A,
                  1.0f + alpha / A, -2.0f * cw, 1.0f - alpha / A);
}

/* ---- Fractional delay line helpers (power-of-two float or int16 buffers) -- */

/* Linear-interpolated read `delay` samples behind write index w (mask = len-1). */
static inline float dsp_read_frac_f(const float *buf, uint32_t mask, uint32_t w, float delay)
{
  const uint32_t di = (uint32_t)delay;
  const float fr = delay - (float)di;
  const float a = buf[(w - di) & mask];
  const float b = buf[(w - di - 1u) & mask];
  return a + fr * (b - a);
}

static inline int16_t dsp_f_to_s16(float x)
{
  x = dsp_clampf(x, -1.0f, 32767.0f / 32768.0f);
  return (int16_t)(x * 32768.0f);
}

static inline float dsp_s16_to_f(int16_t v)
{
  return (float)v * (1.0f / 32768.0f);
}

#endif /* DSP_UTIL_H */
