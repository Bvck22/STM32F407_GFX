/*
 * algo_reverb.c - ROOM, HALL, PLATE, SPRING, SHIMMER.
 *
 * ROOM, HALL and SHIMMER share one 8-line feedback delay network: Walsh-
 * Hadamard feedback matrix (orthogonal, so only the per-line gains and the
 * damping filters lose energy), series input allpasses, slowly modulated lines.
 * PLATE is a Dattorro figure-8 tank, SPRING two dispersive allpass chains inside
 * short feedback loops.
 *
 * All delay memory is int16 with 1/8192 resolution (+-4.0 full scale) and a
 * saturating store. The store rounds stochastically (one uniform dither value
 * per sample): plain truncation loses up to one LSB per pass and would kill the
 * tail early, plain rounding would freeze it. The residue is a noise floor below
 * -80 dBFS. Filters and allpass arithmetic are float.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __ARM_FEATURE_SAT
#include <arm_acle.h>
#endif

#include "dsp_util.h"
#include "fx_algo.h"

/* ---- Delay line primitives (int16 ring, any length) ---------------------- */

#define RV_STORE      8192.0f
#define RV_PD_MAX     4800u                 /* 100 ms */
#define RV_PD_LEN     (RV_PD_MAX + 2u)
#define RV_PD_STEP    7u                    /* predelay moves one sample per 8 samples */
#define RV_MOD_PAD    12u                   /* spare samples on a modulated line */
#define RV_LN_RT60    6.9077553f            /* ln(1000): -60 dB */

typedef struct
{
  int16_t *buf;
  uint32_t len;
  uint32_t w;       /* next write slot, holds the oldest sample */
} dl_t;

typedef struct
{
  uint8_t *p;
} rv_arena_t;

static inline int32_t rv_sat16(int32_t v)
{
#ifdef __ARM_FEATURE_SAT
  return __ssat(v, 16);
#else
  return v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
#endif
}

/* q = 32768 + dither in [0, 1): the float-to-int step becomes an unbiased
 * floor(x + dither) (the offset keeps the value positive, where the conversion
 * truncates downward). */
static inline int16_t rv_st(float x, float q)
{
  return (int16_t)rv_sat16((int32_t)(x * RV_STORE + q) - 32768);
}

static inline float rv_dither(uint32_t *rng)
{
  *rng = *rng * 1664525u + 1013904223u;
  return (float)(*rng >> 8) * (1.0f / 16777216.0f) + 32768.0f;
}

static inline float rv_ld(int16_t v)
{
  return (float)v * (1.0f / RV_STORE);
}

static void dl_alloc(dl_t *d, rv_arena_t *a, uint32_t len)
{
  d->buf = (int16_t *)a->p;
  d->len = len;
  d->w = 0;
  memset(d->buf, 0, len * sizeof(int16_t));
  a->p += len * sizeof(int16_t);
}

static inline void dl_step(dl_t *d)
{
  d->w = (d->w + 1u == d->len) ? 0u : d->w + 1u;
}

static inline float dl_oldest(const dl_t *d)
{
  return rv_ld(d->buf[d->w]);
}

static inline void dl_push(dl_t *d, float x, float q)
{
  d->buf[d->w] = rv_st(x, q);
  dl_step(d);
}

/* i samples behind the newest entry (0 = newest), i < len. */
static inline float dl_tap(const dl_t *d, uint32_t i)
{
  uint32_t j = d->w + d->len - 1u - i;
  if (j >= d->len)
  {
    j -= d->len;
  }
  return rv_ld(d->buf[j]);
}

/* Linear interpolation, delay + 1 < len. */
static inline float dl_tap_f(const dl_t *d, float delay)
{
  const uint32_t i = (uint32_t)delay;
  const float fr = delay - (float)i;
  const float a = dl_tap(d, i);
  const float b = dl_tap(d, i + 1u);
  return a + fr * (b - a);
}

/* Schroeder allpass whose delay is the line length. */
static inline float dl_allpass(dl_t *d, float g, float x, float q)
{
  const float z = dl_oldest(d);
  const float v = x + g * z;
  dl_push(d, v, q);
  return z - g * v;
}

/* Allpass reading at a fractional tap (total delay = tap + 1). */
static inline float dl_allpass_mod(dl_t *d, float g, float tap, float x, float q)
{
  const float z = dl_tap_f(d, tap);
  const float v = x + g * z;
  dl_push(d, v, q);
  return z - g * v;
}

/* ---- Common state: parameter cache, mix smoother, predelay ---------------- */

typedef struct
{
  float last[FX_ALGO_MAX_PARAMS];
  bool primed;
  bool fresh;               /* set by rv_changed() on the first update only */
  uint32_t rng;
  float mix;                /* ramps linearly to mix_tgt over each block */
  float mix_tgt;
  uint32_t pd_cur;          /* samples */
  uint32_t pd_tgt;
  uint32_t pd_tick;
} rv_base_t;

static bool rv_changed(rv_base_t *b, const float *p, uint32_t n)
{
  bool diff = !b->primed;
  for (uint32_t i = 0; i < n; i++)
  {
    if (p[i] != b->last[i])
    {
      b->last[i] = p[i];
      diff = true;
    }
  }
  b->fresh = !b->primed;
  b->primed = true;
  return diff;
}

static void rv_set_mix(rv_base_t *b, float mix)
{
  b->mix_tgt = mix;
  if (b->fresh)
  {
    b->mix = mix;
  }
}

static void rv_set_pd(rv_base_t *b, float ms)
{
  b->pd_tgt = (uint32_t)dsp_clampf(dsp_ms_to_samples(ms), 0.0f, (float)RV_PD_MAX);
  if (b->fresh)
  {
    b->pd_cur = b->pd_tgt;
  }
}

/* Integer predelay that walks to its target at 1/8 sample per sample, so a moving
 * knob bends pitch by 12 % instead of clicking. */
static inline float rv_predelay(rv_base_t *b, dl_t *pd, float x, float q)
{
  dl_push(pd, x, q);
  if (b->pd_cur != b->pd_tgt && (++b->pd_tick & RV_PD_STEP) == 0u)
  {
    b->pd_cur += (b->pd_cur < b->pd_tgt) ? 1u : 0xFFFFFFFFu;
  }
  return dl_tap(pd, b->pd_cur);
}

#define RV_X_COMMA(n) n,
#define RV_X_PLUS(n)  + (n)
#define RV_SUM(list)  (0u list(RV_X_PLUS))

/* ---- FDN (ROOM, HALL, SHIMMER) -------------------------------------------- */

/*
 * The 8 lines are interleaved in one bank: slot w holds the newest sample of
 * every line, so a single write index serves all of them and a line's delay is
 * just a read offset. All lines therefore share one ring period, which sets the
 * memory (8 * period * 2 bytes) and caps the longest line.
 */
#define FDN_N         8u
#define FDN_MAX_DIFF  4u
#define FDN_NORM      0.35355339f           /* 1/sqrt(8) */
#define FDN_DIFF_G    0.65f
#define FDN_WET_TRIM  1.5f                  /* wet at about -6 dB re input at mix 1 */
#define PITCH_WIN     2048.0f
#define PITCH_LEN     2050u
#define PITCH_FIFTH   1.4983071f            /* 2^(7/12) */

/* First n_mod lines are modulated (delay +- depth, so period >= len + depth + 2). */
#define ROOM_PERIOD   832u
#define ROOM_LINES(X) X(331) X(521) X(389) X(661) X(457) X(599) X(739) X(827)
#define ROOM_DIFF(X)  X(107) X(173)
#define HALL_PERIOD   1740u
#define HALL_LINES(X) X(997) X(1213) X(1451) X(1693) X(877) X(1087) X(1327) X(1583)
#define HALL_DIFF(X)  X(142) X(107) X(379)

typedef struct
{
  uint16_t line[FDN_N];
  uint16_t diff[FDN_MAX_DIFF];
  uint16_t period;
  uint8_t n_diff;
  uint8_t n_mod;
  bool predelay;
  bool shimmer;
  float mod_depth;          /* samples */
  float mod_hz;
  float rt_min;
  float rt_max;
  uint32_t sum;             /* total line length */
} fdn_cfg_t;

static const fdn_cfg_t k_room = {
  .line = { ROOM_LINES(RV_X_COMMA) }, .diff = { ROOM_DIFF(RV_X_COMMA) }, .period = ROOM_PERIOD,
  .n_diff = 2, .n_mod = 2, .predelay = true, .shimmer = false,
  .mod_depth = 3.0f, .mod_hz = 0.7f, .rt_min = 0.2f, .rt_max = 1.5f,
  .sum = RV_SUM(ROOM_LINES)
};

static const fdn_cfg_t k_hall = {
  .line = { HALL_LINES(RV_X_COMMA) }, .diff = { HALL_DIFF(RV_X_COMMA) }, .period = HALL_PERIOD,
  .n_diff = 3, .n_mod = 4, .predelay = true, .shimmer = false,
  .mod_depth = 8.0f, .mod_hz = 0.35f, .rt_min = 1.0f, .rt_max = 8.0f,
  .sum = RV_SUM(HALL_LINES)
};

static const fdn_cfg_t k_shimmer = {
  .line = { HALL_LINES(RV_X_COMMA) }, .diff = { HALL_DIFF(RV_X_COMMA) }, .period = HALL_PERIOD,
  .n_diff = 3, .n_mod = 4, .predelay = false, .shimmer = true,
  .mod_depth = 6.0f, .mod_hz = 0.5f, .rt_min = 1.0f, .rt_max = 8.0f,
  .sum = RV_SUM(HALL_LINES)
};

#define ROOM_MEM    (2u * (FDN_N * ROOM_PERIOD + RV_SUM(ROOM_DIFF) + RV_PD_LEN))
#define HALL_MEM    (2u * (FDN_N * HALL_PERIOD + RV_SUM(HALL_DIFF) + RV_PD_LEN))
#define SHIMMER_MEM (2u * (FDN_N * HALL_PERIOD + RV_SUM(HALL_DIFF) + PITCH_LEN))

_Static_assert(ROOM_MEM <= 40000u && HALL_MEM <= 40000u && SHIMMER_MEM <= 40000u,
               "reverb arena over 40 KB");

typedef struct
{
  rv_base_t b;
  int16_t *bank;            /* period * FDN_N, interleaved */
  uint32_t w;
  dl_t diff[FDN_MAX_DIFF];
  dl_t pd;
  dl_t pitch;
  float lp[FDN_N];
  float gain[FDN_N];        /* feedback gain per line, includes FDN_NORM */
  float kd;
  float wet_a;              /* own channel, includes level compensation */
  float wet_b;              /* opposite channel (width) */
  uint32_t lfo_ph;
  uint32_t lfo_inc;
  float shim_amt;
  float shim_dph;
  float shim_ph;
  float shim_out;
} fdn_t;

_Static_assert(sizeof(fdn_t) <= FX_STATE_BYTES, "fdn state too big");

/* Walsh row 3 signs, scaled to unit total injection energy. */
static const float k_inject[FDN_N] = {
  FDN_NORM, -FDN_NORM, -FDN_NORM, FDN_NORM, FDN_NORM, -FDN_NORM, -FDN_NORM, FDN_NORM
};

#define RV_BF(a, b) do { const float t_ = (a); (a) = t_ + (b); (b) = t_ - (b); } while (0)

static inline void fdn_wht8(float *h)
{
  RV_BF(h[0], h[1]); RV_BF(h[2], h[3]); RV_BF(h[4], h[5]); RV_BF(h[6], h[7]);
  RV_BF(h[0], h[2]); RV_BF(h[1], h[3]); RV_BF(h[4], h[6]); RV_BF(h[5], h[7]);
  RV_BF(h[0], h[4]); RV_BF(h[1], h[5]); RV_BF(h[2], h[6]); RV_BF(h[3], h[7]);
}

/* Line j, d samples old (1 <= d <= period). */
static inline float fdn_read(const fdn_t *f, uint32_t period, uint32_t w, uint32_t d, uint32_t j)
{
  const uint32_t slot = (w >= d) ? w - d : w + period - d;
  return rv_ld(f->bank[slot * FDN_N + j]);
}

static void fdn_init(void *state, void *mem, uint32_t mem_len, const fdn_cfg_t *c)
{
  (void)mem_len;   /* arena is sized by mem_bytes */
  fdn_t *f = state;
  memset(f, 0, sizeof(*f));
  rv_arena_t a = { mem };
  const uint32_t bank_len = c->period * FDN_N;
  f->bank = (int16_t *)a.p;
  memset(f->bank, 0, bank_len * sizeof(int16_t));
  a.p += bank_len * sizeof(int16_t);
  for (uint32_t k = 0; k < c->n_diff; k++)
  {
    dl_alloc(&f->diff[k], &a, c->diff[k]);
  }
  if (c->predelay)
  {
    dl_alloc(&f->pd, &a, RV_PD_LEN);
  }
  if (c->shimmer)
  {
    dl_alloc(&f->pitch, &a, PITCH_LEN);
  }
  f->lfo_inc = dsp_phase_inc(c->mod_hz);
  f->shim_ph = 0.5f;
}

static void fdn_tune(fdn_t *f, const fdn_cfg_t *c, float decay, float damp, float width)
{
  const float rt = c->rt_min * powf(c->rt_max / c->rt_min, decay);
  const float per_sample = -RV_LN_RT60 / (rt * DSP_FS);
  for (uint32_t j = 0; j < FDN_N; j++)
  {
    f->gain[j] = FDN_NORM * expf(per_sample * (float)c->line[j]);
  }
  f->kd = dsp_onepole_k(20000.0f * powf(0.125f, damp));
  /* Steady-state tank power is about 8 * RT60/13.8 * fs / sum(len) times the
   * input power; divide it out so wet loudness does not follow decay. */
  const float comp = FDN_WET_TRIM * FDN_NORM *
                     sqrtf((float)c->sum * (RV_LN_RT60 * 2.0f) / (8.0f * DSP_FS * rt));
  f->wet_a = comp * 0.5f * (1.0f + width);
  f->wet_b = comp * 0.5f * (1.0f - width);
}

static inline __attribute__((always_inline))
void fdn_process(fdn_t *f, const fdn_cfg_t *c, float *l, float *r, uint32_t n)
{
  const uint32_t period = c->period;
  uint32_t w = f->w;
  float m = f->b.mix;
  const float m_step = (f->b.mix_tgt - m) / (float)n;
  for (uint32_t i = 0; i < n; i++)
  {
    m += m_step;
    const float q = rv_dither(&f->b.rng);
    float x = 0.5f * (l[i] + r[i]);
    if (c->predelay)
    {
      x = rv_predelay(&f->b, &f->pd, x, q);
    }
    for (uint32_t k = 0; k < c->n_diff; k++)
    {
      x = dl_allpass(&f->diff[k], FDN_DIFF_G, x, q);
    }
    if (c->shimmer)
    {
      x += f->shim_amt * dsp_tanh(f->shim_out);
    }

    float h[FDN_N];
    /* Two triangle LFOs in quadrature, the other two lines run inverted. */
    const float lfo_a = dsp_lfo_tri(f->lfo_ph);
    const float lfo_b = dsp_lfo_tri(f->lfo_ph + 0x40000000u);
    const float lfo[4] = { lfo_a, lfo_b, -lfo_a, -lfo_b };
    f->lfo_ph += f->lfo_inc;
#pragma GCC unroll 8
    for (uint32_t j = 0; j < FDN_N; j++)
    {
      float y;
      if (j < c->n_mod)
      {
        const float d = (float)c->line[j] + c->mod_depth * lfo[j];
        const uint32_t di = (uint32_t)d;
        const float a = fdn_read(f, period, w, di, j);
        const float b = fdn_read(f, period, w, di + 1u, j);
        y = a + (d - (float)di) * (b - a);
      }
      else
      {
        y = fdn_read(f, period, w, c->line[j], j);
      }
      f->lp[j] += f->kd * (y - f->lp[j]) + DSP_ANTI_DENORM;
      h[j] = f->lp[j];
    }
    fdn_wht8(h);

    const float wl = f->wet_a * h[5] + f->wet_b * h[6];
    const float wr = f->wet_a * h[6] + f->wet_b * h[5];
    if (c->shimmer && f->shim_amt != 0.0f)
    {
      dl_push(&f->pitch, (0.5f * FDN_NORM) * (h[5] + h[6]), q);
      f->shim_ph -= f->shim_dph;
      if (f->shim_ph < 0.0f)
      {
        f->shim_ph += 1.0f;
      }
      const float ph2 = f->shim_ph >= 0.5f ? f->shim_ph - 0.5f : f->shim_ph + 0.5f;
      const float w1 = 1.0f - fabsf(2.0f * f->shim_ph - 1.0f);
      const float a = dl_tap_f(&f->pitch, f->shim_ph * PITCH_WIN);
      const float b = dl_tap_f(&f->pitch, ph2 * PITCH_WIN);
      f->shim_out = b + w1 * (a - b);
    }

    int16_t *slot = f->bank + w * FDN_N;
#pragma GCC unroll 8
    for (uint32_t j = 0; j < FDN_N; j++)
    {
      slot[j] = rv_st(f->gain[j] * h[j] + k_inject[j] * x, q);
    }
    w = (w + 1u == period) ? 0u : w + 1u;

    l[i] += m * (wl - l[i]);
    r[i] += m * (wr - r[i]);
  }
  f->w = w;
  f->b.mix = f->b.mix_tgt;
}

static void room_init(void *s, void *mem, uint32_t len)
{
  fdn_init(s, mem, len, &k_room);
}

static void room_update(void *s, const float *p)
{
  fdn_t *f = s;
  if (!rv_changed(&f->b, p, 4))
  {
    return;
  }
  rv_set_mix(&f->b, p[3]);
  rv_set_pd(&f->b, p[2]);
  fdn_tune(f, &k_room, p[0], p[1], 1.0f);
}

static void room_process(void *s, float *l, float *r, uint32_t n)
{
  fdn_process(s, &k_room, l, r, n);
}

static void hall_init(void *s, void *mem, uint32_t len)
{
  fdn_init(s, mem, len, &k_hall);
}

static void hall_update(void *s, const float *p)
{
  fdn_t *f = s;
  if (!rv_changed(&f->b, p, 5))
  {
    return;
  }
  rv_set_mix(&f->b, p[4]);
  rv_set_pd(&f->b, p[2]);
  fdn_tune(f, &k_hall, p[0], p[1], p[3]);
}

static void hall_process(void *s, float *l, float *r, uint32_t n)
{
  fdn_process(s, &k_hall, l, r, n);
}

static void shimmer_init(void *s, void *mem, uint32_t len)
{
  fdn_init(s, mem, len, &k_shimmer);
}

static void shimmer_update(void *s, const float *p)
{
  fdn_t *f = s;
  if (!rv_changed(&f->b, p, 5))
  {
    return;
  }
  rv_set_mix(&f->b, p[4]);
  fdn_tune(f, &k_shimmer, p[0], p[3], 1.0f);
  /* Loop gain stays below 1 even before the soft clip: the tap and injection
   * vectors have norm <= 0.71 and 1, the tank is passive. */
  f->shim_amt = 0.8f * p[1];
  f->shim_dph = ((p[2] >= 0.5f ? PITCH_FIFTH : 2.0f) - 1.0f) * (1.0f / PITCH_WIN);
}

static void shimmer_process(void *s, float *l, float *r, uint32_t n)
{
  fdn_process(s, &k_shimmer, l, r, n);
}

/* ---- PLATE: Dattorro figure-8 tank, lengths ~0.65 x the paper's, 3 input
 * diffusers instead of 4 and 5 output taps per channel instead of 7 (CPU) ---- */

#define PLATE_DIFF(X)  X(93) X(71) X(247)
#define PLATE_AP1(X)   X(439) X(593)
#define PLATE_D1(X)    X(2897) X(2741)
#define PLATE_AP2(X)   X(1171) X(1721)
#define PLATE_D2(X)    X(2417) X(2053)
#define PLATE_TANK     (RV_SUM(PLATE_AP1) + RV_SUM(PLATE_D1) + RV_SUM(PLATE_AP2) + RV_SUM(PLATE_D2))
#define PLATE_MEM      (2u * (RV_PD_LEN + RV_SUM(PLATE_DIFF) + PLATE_TANK + 2u * RV_MOD_PAD))
#define PLATE_MOD_DEPTH 8.0f
#define PLATE_NDIFF    3u
#define PLATE_WET_TRIM  0.35f

_Static_assert(PLATE_MEM <= 40000u, "plate arena over 40 KB");

static const uint16_t k_plate_diff[PLATE_NDIFF] = { PLATE_DIFF(RV_X_COMMA) };
static const float k_plate_diff_g[PLATE_NDIFF] = { 0.75f, 0.75f, 0.625f };
static const uint16_t k_plate_ap1[2] = { PLATE_AP1(RV_X_COMMA) };
static const uint16_t k_plate_d1[2] = { PLATE_D1(RV_X_COMMA) };
static const uint16_t k_plate_ap2[2] = { PLATE_AP2(RV_X_COMMA) };
static const uint16_t k_plate_d2[2] = { PLATE_D2(RV_X_COMMA) };

typedef struct
{
  rv_base_t b;
  dl_t pd;
  dl_t diff[PLATE_NDIFF];
  dl_t ap1[2];
  dl_t d1[2];
  dl_t ap2[2];
  dl_t d2[2];
  float lp[2];
  float fb[2];              /* last output of each half's final delay */
  float gdec;
  float g2;
  float kd;
  float wet;
  uint32_t lfo_ph[2];
  uint32_t lfo_inc[2];
} plate_t;

_Static_assert(sizeof(plate_t) <= FX_STATE_BYTES, "plate state too big");

static void plate_init(void *state, void *mem, uint32_t mem_len)
{
  (void)mem_len;
  plate_t *f = state;
  memset(f, 0, sizeof(*f));
  rv_arena_t a = { mem };
  dl_alloc(&f->pd, &a, RV_PD_LEN);
  for (uint32_t k = 0; k < PLATE_NDIFF; k++)
  {
    dl_alloc(&f->diff[k], &a, k_plate_diff[k]);
  }
  for (uint32_t s = 0; s < 2; s++)
  {
    dl_alloc(&f->ap1[s], &a, k_plate_ap1[s] + RV_MOD_PAD);
    dl_alloc(&f->d1[s], &a, k_plate_d1[s]);
    dl_alloc(&f->ap2[s], &a, k_plate_ap2[s]);
    dl_alloc(&f->d2[s], &a, k_plate_d2[s]);
  }
  f->lfo_ph[1] = 0x80000000u;
  f->lfo_inc[0] = dsp_phase_inc(0.9f);
  f->lfo_inc[1] = dsp_phase_inc(1.1f);
}

static void plate_update(void *state, const float *p)
{
  plate_t *f = state;
  if (!rv_changed(&f->b, p, 4))
  {
    return;
  }
  rv_set_mix(&f->b, p[3]);
  rv_set_pd(&f->b, p[2]);
  const float rt = 0.4f * powf(15.0f, p[0]);
  /* Two decay gains per trip around the 8, so g^2 per tank length. */
  f->gdec = expf(-0.5f * RV_LN_RT60 * (float)PLATE_TANK / (rt * DSP_FS));
  f->g2 = dsp_clampf(f->gdec + 0.15f, 0.25f, 0.5f);
  f->kd = dsp_onepole_k(20000.0f * powf(0.15f, p[1]));
  f->wet = PLATE_WET_TRIM * sqrtf((float)PLATE_TANK * (RV_LN_RT60 * 2.0f) / (5.0f * DSP_FS * rt));
}

static void plate_process(void *state, float *l, float *r, uint32_t n)
{
  plate_t *f = state;
  float m = f->b.mix;
  const float m_step = (f->b.mix_tgt - m) / (float)n;
  for (uint32_t i = 0; i < n; i++)
  {
    m += m_step;
    const float q = rv_dither(&f->b.rng);
    float x = rv_predelay(&f->b, &f->pd, 0.5f * (l[i] + r[i]), q);
    for (uint32_t k = 0; k < PLATE_NDIFF; k++)
    {
      x = dl_allpass(&f->diff[k], k_plate_diff_g[k], x, q);
    }

    const float cross[2] = { f->fb[1], f->fb[0] };
    for (uint32_t s = 0; s < 2; s++)
    {
      const float tap = (float)(k_plate_ap1[s] - 1u) + PLATE_MOD_DEPTH * dsp_lfo_tri(f->lfo_ph[s]);
      f->lfo_ph[s] += f->lfo_inc[s];
      const float a = dl_allpass_mod(&f->ap1[s], -0.7f, tap, x + cross[s], q);
      const float z = dl_oldest(&f->d1[s]);
      dl_push(&f->d1[s], a, q);
      f->lp[s] += f->kd * (z - f->lp[s]) + DSP_ANTI_DENORM;
      const float c = dl_allpass(&f->ap2[s], f->g2, f->gdec * f->lp[s], q);
      f->fb[s] = dl_oldest(&f->d2[s]);
      dl_push(&f->d2[s], c, q);
    }

    const float wl = dl_tap(&f->d1[1], 173) + dl_tap(&f->d1[1], 1933) - dl_tap(&f->ap2[1], 1243)
                   + dl_tap(&f->d2[1], 1297) - dl_tap(&f->d1[0], 1294);
    const float wr = dl_tap(&f->d1[0], 229) + dl_tap(&f->d1[0], 2358) - dl_tap(&f->ap2[0], 798)
                   + dl_tap(&f->d2[0], 1737) - dl_tap(&f->d1[1], 1372);
    l[i] += m * (f->wet * wl - l[i]);
    r[i] += m * (f->wet * wr - r[i]);
  }
}

/* ---- SPRING: dispersive allpass chain inside a feedback loop, two springs ---- */

#define SPRING_STAGES 10u
#define SPRING_WET_TRIM 0.8f
#define SPRING_DELAY(X) X(1409) X(1811)
#define SPRING_M(X)     X(11) X(13)
#define SPRING_MEM      (2u * RV_SUM(SPRING_DELAY) + 4u * SPRING_STAGES * RV_SUM(SPRING_M))

static const uint16_t k_spring_delay[2] = { SPRING_DELAY(RV_X_COMMA) };
static const uint16_t k_spring_m[2] = { SPRING_M(RV_X_COMMA) };

typedef struct
{
  rv_base_t b;
  dl_t dl[2];
  float *chain[2];          /* SPRING_STAGES rings of k_spring_m[s] floats */
  uint32_t pos[2];
  float lo[2];              /* highpass: input minus this lowpass */
  float lp[2];
  float klo;
  float klp;
  float g[2];
  float wet;
  float a_cur;
  float a_tgt;
} spring_t;

_Static_assert(sizeof(spring_t) <= FX_STATE_BYTES, "spring state too big");

static void spring_init(void *state, void *mem, uint32_t mem_len)
{
  (void)mem_len;
  spring_t *f = state;
  memset(f, 0, sizeof(*f));
  rv_arena_t a = { mem };
  /* Floats first: the arena is 4-byte aligned and the int16 lines are even. */
  for (uint32_t s = 0; s < 2; s++)
  {
    const uint32_t bytes = SPRING_STAGES * k_spring_m[s] * sizeof(float);
    f->chain[s] = (float *)a.p;
    memset(a.p, 0, bytes);
    a.p += bytes;
  }
  for (uint32_t s = 0; s < 2; s++)
  {
    dl_alloc(&f->dl[s], &a, k_spring_delay[s]);
  }
}

static void spring_update(void *state, const float *p)
{
  spring_t *f = state;
  if (!rv_changed(&f->b, p, 4))
  {
    return;
  }
  rv_set_mix(&f->b, p[3]);
  const float rt = 0.8f * powf(8.0f, p[0]);
  for (uint32_t s = 0; s < 2; s++)
  {
    const float loop = (float)(k_spring_delay[s] + SPRING_STAGES * k_spring_m[s]);
    f->g[s] = expf(-RV_LN_RT60 * loop / (rt * DSP_FS));
  }
  f->klp = dsp_onepole_k(1200.0f * powf(5.0f, p[1]));
  f->klo = dsp_onepole_k(120.0f);
  f->a_tgt = 0.25f + 0.55f * p[2];
  if (f->b.fresh)
  {
    f->a_cur = f->a_tgt;
  }
  f->wet = SPRING_WET_TRIM * sqrtf(1.0f - f->g[0] * f->g[0]);
}

static void spring_process(void *state, float *l, float *r, uint32_t n)
{
  spring_t *f = state;
  const float a_step = (f->a_tgt - f->a_cur) / (float)n;
  float a = f->a_cur;
  float m = f->b.mix;
  const float m_step = (f->b.mix_tgt - m) / (float)n;
  for (uint32_t i = 0; i < n; i++)
  {
    m += m_step;
    const float q = rv_dither(&f->b.rng);
    const float x = 0.5f * (l[i] + r[i]);
    float out[2];
    a += a_step;
    for (uint32_t s = 0; s < 2; s++)
    {
      const float z = dl_oldest(&f->dl[s]);
      f->lo[s] += f->klo * (z - f->lo[s]);
      f->lp[s] += f->klp * ((z - f->lo[s]) - f->lp[s]) + DSP_ANTI_DENORM;
      out[s] = f->lp[s];

      const uint32_t len = k_spring_m[s];
      float *ring = f->chain[s] + f->pos[s];
      float u = x + f->g[s] * f->lp[s];
      for (uint32_t k = 0; k < SPRING_STAGES; k++)
      {
        const float w = ring[k * len];
        const float v = u + a * w + DSP_ANTI_DENORM;
        ring[k * len] = v;
        u = w - a * v;
      }
      f->pos[s] = (f->pos[s] + 1u == len) ? 0u : f->pos[s] + 1u;
      dl_push(&f->dl[s], u, q);
    }
    const float wl = f->wet * (out[0] + 0.4f * out[1]);
    const float wr = f->wet * (out[1] + 0.4f * out[0]);
    l[i] += m * (wl - l[i]);
    r[i] += m * (wr - r[i]);
  }
  f->a_cur = f->a_tgt;
  f->b.mix = f->b.mix_tgt;
}

/* ---- Descriptors --------------------------------------------------------------- */

const fx_algo_t g_fx_algo_ROOM = {
  .id = FX_ALGO_ROOM, .mem_bytes = ROOM_MEM, .min_mem_bytes = 0, .cost = 640, .tails = 1,
  .init = room_init, .update = room_update, .process = room_process
};

const fx_algo_t g_fx_algo_HALL = {
  .id = FX_ALGO_HALL, .mem_bytes = HALL_MEM, .min_mem_bytes = 0, .cost = 690, .tails = 1,
  .init = hall_init, .update = hall_update, .process = hall_process
};

const fx_algo_t g_fx_algo_PLATE = {
  .id = FX_ALGO_PLATE, .mem_bytes = PLATE_MEM, .min_mem_bytes = 0, .cost = 670, .tails = 1,
  .init = plate_init, .update = plate_update, .process = plate_process
};

const fx_algo_t g_fx_algo_SPRING = {
  .id = FX_ALGO_SPRING, .mem_bytes = SPRING_MEM, .min_mem_bytes = 0, .cost = 330, .tails = 1,
  .init = spring_init, .update = spring_update, .process = spring_process
};

const fx_algo_t g_fx_algo_SHIMMER = {
  .id = FX_ALGO_SHIMMER, .mem_bytes = SHIMMER_MEM, .min_mem_bytes = 0, .cost = 780, .tails = 1,
  .init = shimmer_init, .update = shimmer_update, .process = shimmer_process
};
