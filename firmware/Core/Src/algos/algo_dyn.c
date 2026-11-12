/*
 * algo_dyn.c - Drive and dynamics: overdrive, fuzz, bit crusher, compressor,
 * auto-wah, EQ. All state is small (state area), no arena memory.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "dsp_util.h"
#include "fx_algo.h"

/* ---- Shared helpers ------------------------------------------------------ */

/* Last parameter values, so update() can return early when nothing changed. */
typedef struct
{
  float v[FX_ALGO_MAX_PARAMS];
  bool valid;
} pcache_t;

static bool pcache_changed(pcache_t *c, const float *p, uint32_t n)
{
  if (c->valid && memcmp(c->v, p, n * sizeof(float)) == 0)
  {
    return false;
  }
  memcpy(c->v, p, n * sizeof(float));
  c->valid = true;
  return true;
}

/* On the first update the smoothers start at the target instead of gliding from 0. */
static void sm_set(dsp_smooth_t *s, float v, bool first)
{
  s->tgt = v;
  if (first)
  {
    s->cur = v;
  }
}

static float log_sweep(float lo, float hi, float t)
{
  return lo * powf(hi / lo, t);
}

static float time_coef(float ms)
{
  return 1.0f - expf(-1000.0f / (ms * DSP_FS));
}

/* log2 for x > 0, abs error ~1e-3 (cubic fit of the mantissa). */
static inline float fast_log2(float x)
{
  uint32_t u;
  memcpy(&u, &x, sizeof(u));
  const float e = (float)((int32_t)(u >> 23) - 127);
  u = (u & 0x007FFFFFu) | 0x3F800000u;
  float m;
  memcpy(&m, &u, sizeof(m));
  const float t = m - 1.0f;
  return e + t * (1.4208645f + t * (-0.5772507f + t * 0.1563861f));
}

/* 2^x, rel error ~1e-3. */
static inline float fast_exp2(float x)
{
  x = dsp_clampf(x, -100.0f, 60.0f);
  int32_t i = (int32_t)x;
  if ((float)i > x)
  {
    i--;
  }
  const float f = x - (float)i;
  const float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * 0.0096181f)));
  const uint32_t u = (uint32_t)(i + 127) << 23;
  float s;
  memcpy(&s, &u, sizeof(s));
  return p * s;
}

/* tan(x) for x < ~0.4 rad (fc < ~3 kHz at 48 kHz), series to x^5. */
static inline float tan_small(float x)
{
  const float x2 = x * x;
  return x * (1.0f + x2 * (0.33333333f + x2 * 0.13333333f));
}

/* One-pole DC blocker, corner ~15 Hz. */
typedef struct
{
  float x1, y1;
} dcblock_t;

static inline float dcblock_run(dcblock_t *d, float x)
{
  const float y = x - d->x1 + 0.998f * d->y1 + DSP_ANTI_DENORM;
  d->x1 = x;
  d->y1 = y;
  return y;
}

/* ---- Overdrive ----------------------------------------------------------- */

#define OD_PREEMPH_HZ  700.0f
#define OD_PREEMPH_AMT 1.2f
#define OD_TONE_MIN_HZ 700.0f
#define OD_TONE_MAX_HZ 9000.0f

typedef struct
{
  float pre_lp;
  float tone_lp;
  dcblock_t dc;
} od_ch_t;

typedef struct
{
  pcache_t pc;
  od_ch_t ch[2];
  dsp_smooth_t gain;
  dsp_smooth_t out;
  float pre_k;
  float tone_k;
} od_state_t;

_Static_assert(sizeof(od_state_t) <= FX_STATE_BYTES, "od_state_t too large");

static void od_init(void *state, void *mem, uint32_t mem_len)
{
  (void)mem;
  (void)mem_len;
  memset(state, 0, sizeof(od_state_t));
}

static void od_update(void *state, const float *p)
{
  od_state_t *s = state;
  const bool first = !s->pc.valid;
  if (!pcache_changed(&s->pc, p, 3))
  {
    return;
  }
  const float g = powf(100.0f, p[0]);
  s->pre_k = dsp_onepole_k(OD_PREEMPH_HZ);
  s->tone_k = dsp_onepole_k(log_sweep(OD_TONE_MIN_HZ, OD_TONE_MAX_HZ, p[1]));
  sm_set(&s->gain, g, first);
  /* Level 0.5 is about unity at low gain; high gain is pulled down to
   * compensate for the added loudness of the clipped signal. */
  sm_set(&s->out, 2.0f * p[2] / powf(g, 0.3f), first);
}

static inline float od_run(const od_state_t *s, od_ch_t *c, float x, float g)
{
  c->pre_lp += s->pre_k * (x - c->pre_lp) + DSP_ANTI_DENORM;
  const float u = (x + OD_PREEMPH_AMT * (x - c->pre_lp)) * g;
  /* Negative half saturates earlier: even harmonics, same slope at zero. */
  const float y = u >= 0.0f ? dsp_tanh(u) : 0.8f * dsp_tanh(1.25f * u);
  const float d = dcblock_run(&c->dc, y);
  c->tone_lp += s->tone_k * (d - c->tone_lp) + DSP_ANTI_DENORM;
  return c->tone_lp;
}

static void od_process(void *state, float *l, float *r, uint32_t n)
{
  od_state_t *s = state;
  for (uint32_t i = 0; i < n; i++)
  {
    const float g = dsp_smooth_next(&s->gain);
    const float o = dsp_smooth_next(&s->out);
    l[i] = od_run(s, &s->ch[0], l[i], g) * o;
    r[i] = od_run(s, &s->ch[1], r[i], g) * o;
  }
}

const fx_algo_t g_fx_algo_OVERDRIVE = {
  .id = FX_ALGO_OVERDRIVE,
  .cost = 260,
  .init = od_init,
  .update = od_update,
  .process = od_process,
};

/* ---- Fuzz ---------------------------------------------------------------- */

#define FUZZ_TONE_MIN_HZ 500.0f
#define FUZZ_TONE_MAX_HZ 8000.0f

typedef struct
{
  float tone_lp;
  dcblock_t dc;
} fuzz_ch_t;

typedef struct
{
  pcache_t pc;
  fuzz_ch_t ch[2];
  dsp_smooth_t gain;
  dsp_smooth_t shift;
  dsp_smooth_t out;
  float tone_k;
} fuzz_state_t;

_Static_assert(sizeof(fuzz_state_t) <= FX_STATE_BYTES, "fuzz_state_t too large");

static void fuzz_init(void *state, void *mem, uint32_t mem_len)
{
  (void)mem;
  (void)mem_len;
  memset(state, 0, sizeof(fuzz_state_t));
}

static void fuzz_update(void *state, const float *p)
{
  fuzz_state_t *s = state;
  const bool first = !s->pc.valid;
  if (!pcache_changed(&s->pc, p, 4))
  {
    return;
  }
  s->tone_k = dsp_onepole_k(log_sweep(FUZZ_TONE_MIN_HZ, FUZZ_TONE_MAX_HZ, p[1]));
  sm_set(&s->gain, 10.0f * powf(30.0f, p[0]), first);
  sm_set(&s->shift, 1.5f * p[2], first);
  sm_set(&s->out, 1.2f * p[3], first);
}

/* Cubic soft clipper, flat beyond +-1. */
static inline float fuzz_clip(float v)
{
  const float y = dsp_clampf(v, -1.0f, 1.0f);
  return y * (1.5f - 0.5f * y * y);
}

static inline float fuzz_run(const fuzz_state_t *s, fuzz_ch_t *c, float x, float g, float sh,
                             float rest)
{
  const float y = fuzz_clip(x * g - sh) - rest;
  const float d = dcblock_run(&c->dc, y);
  c->tone_lp += s->tone_k * (d - c->tone_lp) + DSP_ANTI_DENORM;
  return c->tone_lp;
}

static void fuzz_process(void *state, float *l, float *r, uint32_t n)
{
  fuzz_state_t *s = state;
  for (uint32_t i = 0; i < n; i++)
  {
    const float g = dsp_smooth_next(&s->gain);
    const float sh = dsp_smooth_next(&s->shift);
    const float o = dsp_smooth_next(&s->out);
    /* Bias pushes the signal below the clipper's linear range: weak input
     * (note decay) is gated, the rest sputters. Subtracting f(-sh) keeps
     * silence at 0. */
    const float rest = fuzz_clip(-sh);
    l[i] = fuzz_run(s, &s->ch[0], l[i], g, sh, rest) * o;
    r[i] = fuzz_run(s, &s->ch[1], r[i], g, sh, rest) * o;
  }
}

const fx_algo_t g_fx_algo_FUZZ = {
  .id = FX_ALGO_FUZZ,
  .cost = 240,
  .init = fuzz_init,
  .update = fuzz_update,
  .process = fuzz_process,
};

/* ---- Bit crusher --------------------------------------------------------- */

typedef struct
{
  pcache_t pc;
  float held[2];
  uint32_t hold_len;
  uint32_t count;
  float levels;       /* quantizer steps per unit amplitude */
  float inv_levels;
  dsp_smooth_t mix;
} crush_state_t;

_Static_assert(sizeof(crush_state_t) <= FX_STATE_BYTES, "crush_state_t too large");

static void crush_init(void *state, void *mem, uint32_t mem_len)
{
  (void)mem;
  (void)mem_len;
  memset(state, 0, sizeof(crush_state_t));
}

static void crush_update(void *state, const float *p)
{
  crush_state_t *s = state;
  const bool first = !s->pc.valid;
  if (!pcache_changed(&s->pc, p, 3))
  {
    return;
  }
  const float bits = dsp_clampf(p[0], 2.0f, 16.0f);
  s->levels = exp2f(bits - 1.0f);
  s->inv_levels = 1.0f / s->levels;
  s->hold_len = (uint32_t)dsp_clampf(p[1] + 0.5f, 1.0f, 64.0f);
  sm_set(&s->mix, p[2], first);
}

/* Mid-tread quantizer: zero stays exactly zero. */
static inline float crush_quant(const crush_state_t *s, float x)
{
  const float v = dsp_clampf(x, -1.0f, 1.0f) * s->levels;
  const int32_t q = (int32_t)(v + (v >= 0.0f ? 0.5f : -0.5f));
  return (float)q * s->inv_levels;
}

static void crush_process(void *state, float *l, float *r, uint32_t n)
{
  crush_state_t *s = state;
  for (uint32_t i = 0; i < n; i++)
  {
    if (s->count == 0u)
    {
      s->held[0] = crush_quant(s, l[i]);
      s->held[1] = crush_quant(s, r[i]);
    }
    if (++s->count >= s->hold_len)
    {
      s->count = 0u;
    }
    const float m = dsp_smooth_next(&s->mix);
    l[i] += m * (s->held[0] - l[i]);
    r[i] += m * (s->held[1] - r[i]);
  }
}

const fx_algo_t g_fx_algo_BITCRUSH = {
  .id = FX_ALGO_BITCRUSH,
  .cost = 70,
  .init = crush_init,
  .update = crush_update,
  .process = crush_process,
};

/* ---- Compressor ---------------------------------------------------------- */

#define COMP_KNEE_DB     6.0f
#define COMP_DB_PER_LOG2 3.0103f      /* power ratio: 10 * log10(2) per octave */
#define COMP_LOG2_PER_DB 0.16609640f  /* amplitude gain: log2(10) / 20 */

typedef struct
{
  pcache_t pc;
  float thr_db;
  float slope;      /* 1/ratio - 1 */
  float ka, kr;
  float gr_db;      /* smoothed gain reduction, <= 0 */
  dsp_smooth_t makeup;
  dsp_smooth_t mix;
} comp_state_t;

_Static_assert(sizeof(comp_state_t) <= FX_STATE_BYTES, "comp_state_t too large");

static void comp_init(void *state, void *mem, uint32_t mem_len)
{
  (void)mem;
  (void)mem_len;
  memset(state, 0, sizeof(comp_state_t));
}

static void comp_update(void *state, const float *p)
{
  comp_state_t *s = state;
  const bool first = !s->pc.valid;
  if (!pcache_changed(&s->pc, p, 6))
  {
    return;
  }
  s->thr_db = p[0];
  s->slope = 1.0f / dsp_clampf(p[1], 1.0f, 20.0f) - 1.0f;
  s->ka = time_coef(dsp_clampf(p[2], 0.1f, 1000.0f));
  s->kr = time_coef(dsp_clampf(p[3], 1.0f, 5000.0f));
  sm_set(&s->makeup, dsp_db_to_lin(p[4]), first);
  sm_set(&s->mix, p[5], first);
}

static void comp_process(void *state, float *l, float *r, uint32_t n)
{
  comp_state_t *s = state;
  for (uint32_t i = 0; i < n; i++)
  {
    /* Linked detector: both channels get the same gain, the image stays put. */
    const float pl = l[i] * l[i];
    const float pr = r[i] * r[i];
    const float lvl_db = COMP_DB_PER_LOG2 * fast_log2((pl > pr ? pl : pr) + 1e-12f);
    const float over = lvl_db - s->thr_db;

    float target = 0.0f;
    if (2.0f * over > COMP_KNEE_DB)
    {
      target = s->slope * over;
    }
    else if (2.0f * over > -COMP_KNEE_DB)
    {
      const float k = over + 0.5f * COMP_KNEE_DB;
      target = s->slope * k * k * (1.0f / (2.0f * COMP_KNEE_DB));
    }

    s->gr_db += (target < s->gr_db ? s->ka : s->kr) * (target - s->gr_db);
    if (target == 0.0f && s->gr_db > -1e-4f)
    {
      s->gr_db = 0.0f;
    }

    const float wet = fast_exp2(s->gr_db * COMP_LOG2_PER_DB) * dsp_smooth_next(&s->makeup);
    const float m = dsp_smooth_next(&s->mix);
    const float gain = 1.0f + m * (wet - 1.0f);
    l[i] *= gain;
    r[i] *= gain;
  }
}

const fx_algo_t g_fx_algo_COMPRESSOR = {
  .id = FX_ALGO_COMPRESSOR,
  .cost = 200,
  .init = comp_init,
  .update = comp_update,
  .process = comp_process,
};

/* ---- Auto-wah ------------------------------------------------------------ */

#define WAH_CTL_MASK 31u     /* filter coefficients refresh every 32 frames */
#define WAH_LO_HZ    300.0f
#define WAH_HI_HZ    2500.0f
#define WAH_GAIN     1.5f

typedef struct
{
  float s1, s2;
} svf_t;

typedef struct
{
  pcache_t pc;
  svf_t ch[2];
  float env;
  float env_ka, env_kr;
  float sens_gain;
  float span_log2;      /* log2(hi / lo) of the current sweep */
  float k;              /* 1/Q */
  float g, a1;
  uint32_t lfo_ph, lfo_inc;
  uint32_t tick;
  bool lfo_mode;
  dsp_smooth_t mix;
} wah_state_t;

_Static_assert(sizeof(wah_state_t) <= FX_STATE_BYTES, "wah_state_t too large");

static void wah_init(void *state, void *mem, uint32_t mem_len)
{
  (void)mem;
  (void)mem_len;
  wah_state_t *s = state;
  memset(s, 0, sizeof(*s));
  s->g = 0.02f;
  s->a1 = 1.0f;
}

static void wah_update(void *state, const float *p)
{
  wah_state_t *s = state;
  const bool first = !s->pc.valid;
  if (!pcache_changed(&s->pc, p, 6))
  {
    return;
  }
  s->sens_gain = 16.0f * p[0];
  s->span_log2 = log2f(1.0f + p[1] * (WAH_HI_HZ / WAH_LO_HZ - 1.0f));
  s->k = 1.0f / dsp_clampf(p[2], 0.5f, 10.0f);
  s->lfo_mode = p[3] >= 0.5f;
  s->lfo_inc = dsp_phase_inc(p[4]);
  s->env_ka = time_coef(5.0f);
  s->env_kr = time_coef(120.0f);
  sm_set(&s->mix, p[5], first);
}

/* Zavalishin TPT state-variable filter, band-pass output with unity peak. */
static inline float svf_bp(const wah_state_t *s, svf_t *f, float x)
{
  const float hp = (x - (s->k + s->g) * f->s1 - f->s2) * s->a1;
  const float v1 = s->g * hp;
  const float bp = v1 + f->s1;
  f->s1 = bp + v1 + DSP_ANTI_DENORM;
  const float v2 = s->g * bp;
  f->s2 = v2 + f->s2 + v2;
  return bp * s->k;
}

static void wah_process(void *state, float *l, float *r, uint32_t n)
{
  wah_state_t *s = state;
  for (uint32_t i = 0; i < n; i++)
  {
    const float a = fabsf(l[i]);
    const float b = fabsf(r[i]);
    const float m = a > b ? a : b;
    s->env += (m > s->env ? s->env_ka : s->env_kr) * (m - s->env) + DSP_ANTI_DENORM;

    if ((s->tick++ & WAH_CTL_MASK) == 0u)
    {
      float pos;
      if (s->lfo_mode)
      {
        pos = 0.5f + 0.5f * dsp_lfo_sin(s->lfo_ph);
        s->lfo_ph += s->lfo_inc * (WAH_CTL_MASK + 1u);
      }
      else
      {
        pos = dsp_clampf(s->env * s->sens_gain, 0.0f, 1.0f);
      }
      const float fc = WAH_LO_HZ * fast_exp2(pos * s->span_log2);
      s->g = tan_small(DSP_PI * fc / DSP_FS);
      s->a1 = 1.0f / (1.0f + s->g * (s->g + s->k));
    }

    const float mix = dsp_smooth_next(&s->mix);
    const float wl = svf_bp(s, &s->ch[0], l[i]) * WAH_GAIN;
    const float wr = svf_bp(s, &s->ch[1], r[i]) * WAH_GAIN;
    l[i] += mix * (wl - l[i]);
    r[i] += mix * (wr - r[i]);
  }
}

const fx_algo_t g_fx_algo_AUTOWAH = {
  .id = FX_ALGO_AUTOWAH,
  .cost = 240,
  .init = wah_init,
  .update = wah_update,
  .process = wah_process,
};

/* ---- EQ ------------------------------------------------------------------ */

#define EQ_LOW_HZ  120.0f
#define EQ_HIGH_HZ 3200.0f
#define EQ_MID_Q   0.9f

typedef struct
{
  pcache_t pc;
  dsp_biquad_coef_t coef[3];
  dsp_biquad_state_t st[2][3];
  dsp_smooth_t level;
} eq_state_t;

_Static_assert(sizeof(eq_state_t) <= FX_STATE_BYTES, "eq_state_t too large");

static void eq_init(void *state, void *mem, uint32_t mem_len)
{
  (void)mem;
  (void)mem_len;
  memset(state, 0, sizeof(eq_state_t));
}

static void eq_update(void *state, const float *p)
{
  eq_state_t *s = state;
  const bool first = !s->pc.valid;
  if (!pcache_changed(&s->pc, p, 5))
  {
    return;
  }
  dsp_biquad_shelf(&s->coef[0], false, EQ_LOW_HZ, p[0]);
  dsp_biquad_peak(&s->coef[1], dsp_clampf(p[2], 50.0f, 10000.0f), EQ_MID_Q, p[1]);
  dsp_biquad_shelf(&s->coef[2], true, EQ_HIGH_HZ, p[3]);
  sm_set(&s->level, dsp_db_to_lin(p[4]), first);
}

static inline float eq_run(const eq_state_t *s, dsp_biquad_state_t *st, float x)
{
  x = dsp_biquad_run(&s->coef[0], &st[0], x);
  x = dsp_biquad_run(&s->coef[1], &st[1], x);
  return dsp_biquad_run(&s->coef[2], &st[2], x);
}

static void eq_process(void *state, float *l, float *r, uint32_t n)
{
  eq_state_t *s = state;
  for (uint32_t i = 0; i < n; i++)
  {
    const float lv = dsp_smooth_next(&s->level);
    l[i] = eq_run(s, s->st[0], l[i]) * lv;
    r[i] = eq_run(s, s->st[1], r[i]) * lv;
  }
}

const fx_algo_t g_fx_algo_EQ = {
  .id = FX_ALGO_EQ,
  .cost = 170,
  .init = eq_init,
  .update = eq_update,
  .process = eq_process,
};
