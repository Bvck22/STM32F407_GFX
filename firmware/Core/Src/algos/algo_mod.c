/*
 * algo_mod.c - Modulation: chorus, vibrato, flanger, phaser, tremolo/pan,
 * rotary, ring modulator. Delay lines (chorus, vibrato, flanger) live in the
 * arena; the rotary's short Doppler lines fit in the state area.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "dsp_util.h"
#include "fx_algo.h"

/* ---- Shared helpers ------------------------------------------------------ */

#define PHASE_QUARTER 0x40000000u
#define PHASE_HALF    0x80000000u

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

/* tan(x) for x < ~0.4 rad (f < ~3 kHz at 48 kHz), series to x^5. */
static inline float tan_small(float x)
{
  const float x2 = x * x;
  return x * (1.0f + x2 * (0.33333333f + x2 * 0.13333333f));
}

static inline float mix_lerp(float dry, float wet, float m)
{
  return dry + m * (wet - dry);
}

/* ---- Chorus -------------------------------------------------------------- */

#define CHO_LEN       1024u      /* floats, power of two */
#define CHO_BASE_L_MS 10.0f
#define CHO_BASE_R_MS 13.0f
#define CHO_DEPTH_MS  5.0f

typedef struct
{
  pcache_t pc;
  float *buf;
  uint32_t w;
  uint32_t ph, inc;
  dsp_smooth_t depth;   /* samples */
  dsp_smooth_t width;
  dsp_smooth_t mix;
} cho_state_t;

_Static_assert(sizeof(cho_state_t) <= FX_STATE_BYTES, "cho_state_t too large");

static void cho_init(void *state, void *mem, uint32_t mem_len)
{
  (void)mem_len;
  cho_state_t *s = state;
  memset(s, 0, sizeof(*s));
  s->buf = mem;
  memset(mem, 0, CHO_LEN * sizeof(float));
}

static void cho_update(void *state, const float *p)
{
  cho_state_t *s = state;
  const bool first = !s->pc.valid;
  if (!pcache_changed(&s->pc, p, 4))
  {
    return;
  }
  s->inc = dsp_phase_inc(p[0]);
  sm_set(&s->depth, p[1] * dsp_ms_to_samples(CHO_DEPTH_MS), first);
  sm_set(&s->width, p[2], first);
  sm_set(&s->mix, p[3], first);
}

static void cho_process(void *state, float *l, float *r, uint32_t n)
{
  cho_state_t *s = state;
  const float base_l = dsp_ms_to_samples(CHO_BASE_L_MS);
  const float base_r = dsp_ms_to_samples(CHO_BASE_R_MS);
  for (uint32_t i = 0; i < n; i++)
  {
    const float depth = dsp_smooth_next(&s->depth);
    const float width = dsp_smooth_next(&s->width);
    const float mix = dsp_smooth_next(&s->mix);

    /* Width spreads both the LFO phase of the two taps and their panning. */
    const uint32_t off = (uint32_t)(width * (float)PHASE_HALF);
    const float tap_l = dsp_read_frac_f(s->buf, CHO_LEN - 1u, s->w,
                                        base_l + depth * dsp_lfo_sin(s->ph));
    const float tap_r = dsp_read_frac_f(s->buf, CHO_LEN - 1u, s->w,
                                        base_r + depth * dsp_lfo_sin(s->ph + off));
    s->ph += s->inc;
    s->buf[s->w] = 0.5f * (l[i] + r[i]);
    s->w = (s->w + 1u) & (CHO_LEN - 1u);

    const float pan = 0.5f + 0.5f * width;
    l[i] = mix_lerp(l[i], tap_r + pan * (tap_l - tap_r), mix);
    r[i] = mix_lerp(r[i], tap_l + pan * (tap_r - tap_l), mix);
  }
}

const fx_algo_t g_fx_algo_CHORUS = {
  .id = FX_ALGO_CHORUS,
  .mem_bytes = CHO_LEN * sizeof(float),
  .cost = 260,
  .init = cho_init,
  .update = cho_update,
  .process = cho_process,
};

/* ---- Vibrato ------------------------------------------------------------- */

#define VIB_LEN        1024u     /* floats per channel, power of two */
#define VIB_MAX_PITCH  0.03f     /* peak pitch deviation at depth 1 (~50 cents) */
#define VIB_R_PHASE    0x20000000u

typedef struct
{
  pcache_t pc;
  float *buf[2];
  uint32_t w;
  uint32_t ph, inc;
  dsp_smooth_t amp;    /* delay swing, samples */
} vib_state_t;

_Static_assert(sizeof(vib_state_t) <= FX_STATE_BYTES, "vib_state_t too large");

static void vib_init(void *state, void *mem, uint32_t mem_len)
{
  (void)mem_len;
  vib_state_t *s = state;
  memset(s, 0, sizeof(*s));
  s->buf[0] = mem;
  s->buf[1] = s->buf[0] + VIB_LEN;
  memset(mem, 0, 2u * VIB_LEN * sizeof(float));
}

static void vib_update(void *state, const float *p)
{
  vib_state_t *s = state;
  const bool first = !s->pc.valid;
  if (!pcache_changed(&s->pc, p, 2))
  {
    return;
  }
  s->inc = dsp_phase_inc(p[0]);
  /* Pitch deviation = 2*pi*rate*swing/fs, so the swing is scaled by 1/rate to
   * keep the deviation in cents independent of the rate. */
  sm_set(&s->amp, p[1] * VIB_MAX_PITCH * DSP_FS / (DSP_TWO_PI * dsp_clampf(p[0], 0.1f, 20.0f)), first);
}

static void vib_process(void *state, float *l, float *r, uint32_t n)
{
  vib_state_t *s = state;
  for (uint32_t i = 0; i < n; i++)
  {
    const float amp = dsp_smooth_next(&s->amp);
    const float d_l = 2.0f + amp * (1.0f + dsp_lfo_sin(s->ph));
    const float d_r = 2.0f + amp * (1.0f + dsp_lfo_sin(s->ph + VIB_R_PHASE));
    s->ph += s->inc;

    const float y_l = dsp_read_frac_f(s->buf[0], VIB_LEN - 1u, s->w, d_l);
    const float y_r = dsp_read_frac_f(s->buf[1], VIB_LEN - 1u, s->w, d_r);
    s->buf[0][s->w] = l[i];
    s->buf[1][s->w] = r[i];
    s->w = (s->w + 1u) & (VIB_LEN - 1u);
    l[i] = y_l;
    r[i] = y_r;
  }
}

const fx_algo_t g_fx_algo_VIBRATO = {
  .id = FX_ALGO_VIBRATO,
  .mem_bytes = 2u * VIB_LEN * sizeof(float),
  .cost = 170,
  .init = vib_init,
  .update = vib_update,
  .process = vib_process,
};

/* ---- Flanger ------------------------------------------------------------- */

#define FLA_LEN        512u      /* floats per channel, power of two */
#define FLA_MIN_MS     0.8f      /* centre delay at manual = 0 */
#define FLA_SPAN_MS    4.2f      /* centre delay range */
#define FLA_SWEEP      0.9f      /* delay swing as fraction of the centre at depth 1 */
#define FLA_R_PHASE    PHASE_QUARTER

typedef struct
{
  pcache_t pc;
  float *buf[2];
  uint32_t w;
  uint32_t ph, inc;
  dsp_smooth_t centre;   /* samples */
  dsp_smooth_t depth;
  dsp_smooth_t fb;
  dsp_smooth_t mix;
} fla_state_t;

_Static_assert(sizeof(fla_state_t) <= FX_STATE_BYTES, "fla_state_t too large");

static void fla_init(void *state, void *mem, uint32_t mem_len)
{
  (void)mem_len;
  fla_state_t *s = state;
  memset(s, 0, sizeof(*s));
  s->buf[0] = mem;
  s->buf[1] = s->buf[0] + FLA_LEN;
  memset(mem, 0, 2u * FLA_LEN * sizeof(float));
}

static void fla_update(void *state, const float *p)
{
  fla_state_t *s = state;
  const bool first = !s->pc.valid;
  if (!pcache_changed(&s->pc, p, 5))
  {
    return;
  }
  s->inc = dsp_phase_inc(p[0]);
  sm_set(&s->depth, p[1] * FLA_SWEEP, first);
  sm_set(&s->centre, dsp_ms_to_samples(FLA_MIN_MS + p[2] * FLA_SPAN_MS), first);
  sm_set(&s->fb, dsp_clampf(p[3], -0.95f, 0.95f), first);
  sm_set(&s->mix, p[4], first);
}

static inline float fla_run(float *buf, uint32_t w, float x, float delay, float fb)
{
  const float y = dsp_read_frac_f(buf, FLA_LEN - 1u, w, delay);
  /* The soft limit keeps strong resonances from running away. */
  buf[w] = x + dsp_tanh(fb * y);
  return y;
}

static void fla_process(void *state, float *l, float *r, uint32_t n)
{
  fla_state_t *s = state;
  for (uint32_t i = 0; i < n; i++)
  {
    const float centre = dsp_smooth_next(&s->centre);
    const float swing = centre * dsp_smooth_next(&s->depth);
    const float fb = dsp_smooth_next(&s->fb);
    const float mix = dsp_smooth_next(&s->mix);
    const float d_l = centre + swing * dsp_lfo_sin(s->ph);
    const float d_r = centre + swing * dsp_lfo_sin(s->ph + FLA_R_PHASE);
    s->ph += s->inc;

    const float y_l = fla_run(s->buf[0], s->w, l[i], d_l, fb);
    const float y_r = fla_run(s->buf[1], s->w, r[i], d_r, fb);
    s->w = (s->w + 1u) & (FLA_LEN - 1u);
    l[i] = mix_lerp(l[i], y_l, mix);
    r[i] = mix_lerp(r[i], y_r, mix);
  }
}

const fx_algo_t g_fx_algo_FLANGER = {
  .id = FX_ALGO_FLANGER,
  .mem_bytes = 2u * FLA_LEN * sizeof(float),
  .cost = 330,
  .init = fla_init,
  .update = fla_update,
  .process = fla_process,
};

/* ---- Phaser -------------------------------------------------------------- */

#define PH_MAX_STAGES 8u
#define PH_CTL_MASK   15u        /* allpass coefficient retargeted every 16 frames */
#define PH_LO_HZ      200.0f
#define PH_OCTAVES    4.3219281f /* log2(4 kHz / 200 Hz) */
#define PH_R_PHASE    PHASE_QUARTER

typedef struct
{
  float z[PH_MAX_STAGES];
  float fb_prev;
  float a, da;           /* current allpass coefficient and its per-frame step */
} ph_ch_t;

typedef struct
{
  pcache_t pc;
  ph_ch_t ch[2];
  uint32_t stages;
  uint32_t ph, inc;
  uint32_t tick;
  float depth;
  dsp_smooth_t fb;
  dsp_smooth_t mix;
} pha_state_t;

_Static_assert(sizeof(pha_state_t) <= FX_STATE_BYTES, "pha_state_t too large");

static void pha_init(void *state, void *mem, uint32_t mem_len)
{
  (void)mem;
  (void)mem_len;
  pha_state_t *s = state;
  memset(s, 0, sizeof(*s));
  s->stages = 4u;
}

static void pha_update(void *state, const float *p)
{
  pha_state_t *s = state;
  const bool first = !s->pc.valid;
  if (!pcache_changed(&s->pc, p, 5))
  {
    return;
  }
  s->inc = dsp_phase_inc(p[0]);
  s->depth = p[1];
  sm_set(&s->fb, dsp_clampf(p[2], 0.0f, 0.9f), first);
  s->stages = 2u * (uint32_t)(dsp_clampf(p[3], 2.0f, 8.0f) * 0.5f + 0.5f);
  sm_set(&s->mix, p[4], first);
}

/* First-order allpass (a + z^-1) / (1 + a z^-1) with a = (1 - t) / (1 + t). */
static void pha_retarget(const pha_state_t *s, ph_ch_t *c, uint32_t phase)
{
  const float pos = 0.5f + 0.5f * s->depth * dsp_lfo_sin(phase);
  const float t = tan_small(DSP_PI * PH_LO_HZ * fast_exp2(pos * PH_OCTAVES) / DSP_FS);
  c->da = ((1.0f - t) / (1.0f + t) - c->a) * (1.0f / (float)(PH_CTL_MASK + 1u));
}

static inline float pha_run(const pha_state_t *s, ph_ch_t *c, float x, float fb, float mix)
{
  float v = x + dsp_tanh(fb * c->fb_prev);
  for (uint32_t k = 0; k < s->stages; k++)
  {
    const float y = c->a * v + c->z[k];
    c->z[k] = v - c->a * y + DSP_ANTI_DENORM;
    v = y;
  }
  c->fb_prev = v;
  c->a += c->da;
  return mix_lerp(x, v, mix);
}

static void pha_process(void *state, float *l, float *r, uint32_t n)
{
  pha_state_t *s = state;
  for (uint32_t i = 0; i < n; i++)
  {
    if ((s->tick++ & PH_CTL_MASK) == 0u)
    {
      pha_retarget(s, &s->ch[0], s->ph);
      pha_retarget(s, &s->ch[1], s->ph + PH_R_PHASE);
    }
    s->ph += s->inc;
    const float fb = dsp_smooth_next(&s->fb);
    const float mix = dsp_smooth_next(&s->mix);
    l[i] = pha_run(s, &s->ch[0], l[i], fb, mix);
    r[i] = pha_run(s, &s->ch[1], r[i], fb, mix);
  }
}

const fx_algo_t g_fx_algo_PHASER = {
  .id = FX_ALGO_PHASER,
  .cost = 420,
  .init = pha_init,
  .update = pha_update,
  .process = pha_process,
};

/* ---- Tremolo / Pan ------------------------------------------------------- */

#define TRE_GAIN_K 0.01f      /* ~2 ms: rounds square edges, hides shape switches */

typedef struct
{
  pcache_t pc;
  uint32_t ph, inc;
  uint32_t shape;
  float gl, gr;
  dsp_smooth_t depth;
  dsp_smooth_t pan;      /* 0 = tremolo, 1 = pan */
} tre_state_t;

_Static_assert(sizeof(tre_state_t) <= FX_STATE_BYTES, "tre_state_t too large");

static void tre_init(void *state, void *mem, uint32_t mem_len)
{
  (void)mem;
  (void)mem_len;
  tre_state_t *s = state;
  memset(s, 0, sizeof(*s));
  s->gl = 1.0f;
  s->gr = 1.0f;
}

static void tre_update(void *state, const float *p)
{
  tre_state_t *s = state;
  const bool first = !s->pc.valid;
  if (!pcache_changed(&s->pc, p, 4))
  {
    return;
  }
  s->inc = dsp_phase_inc(p[0]);
  s->shape = (uint32_t)dsp_clampf(p[2] + 0.5f, 0.0f, 2.0f);
  sm_set(&s->depth, p[1], first);
  sm_set(&s->pan, p[3] >= 0.5f ? 1.0f : 0.0f, first);
}

static inline float tre_wave(const tre_state_t *s)
{
  switch (s->shape)
  {
    case 0u:
      return dsp_lfo_sin(s->ph);
    case 1u:
      return dsp_lfo_tri(s->ph);
    default:
      return (s->ph & PHASE_HALF) ? -1.0f : 1.0f;
  }
}

static void tre_process(void *state, float *l, float *r, uint32_t n)
{
  tre_state_t *s = state;
  for (uint32_t i = 0; i < n; i++)
  {
    const float u = tre_wave(s);
    s->ph += s->inc;
    const float depth = 0.5f * dsp_smooth_next(&s->depth);
    const float ur = u * (1.0f - 2.0f * dsp_smooth_next(&s->pan));
    s->gl += TRE_GAIN_K * ((1.0f - depth * (1.0f - u)) - s->gl);
    s->gr += TRE_GAIN_K * ((1.0f - depth * (1.0f - ur)) - s->gr);
    l[i] *= s->gl;
    r[i] *= s->gr;
  }
}

const fx_algo_t g_fx_algo_TREMOLO = {
  .id = FX_ALGO_TREMOLO,
  .cost = 110,
  .init = tre_init,
  .update = tre_update,
  .process = tre_process,
};

/* ---- Rotary -------------------------------------------------------------- */

#define ROT_LEN        128u      /* floats per line, power of two */
#define ROT_XOVER_HZ   800.0f
#define ROT_HORN_SLOW  0.8f
#define ROT_HORN_FAST  6.7f
#define ROT_DRUM_SLOW  0.7f
#define ROT_DRUM_FAST  5.7f
#define ROT_HORN_SWING 24.0f     /* samples, ~0.17 m radius */
#define ROT_DRUM_SWING 34.0f     /* samples, ~0.25 m radius */
#define ROT_HORN_AM    0.7f      /* level drop when the horn faces away */
#define ROT_DRUM_AM    0.4f
#define ROT_R_PHASE    PHASE_QUARTER   /* right mic 90 degrees from the left */

typedef struct
{
  float freq, target, k;       /* Hz; k = per-block approach to the target */
  uint32_t ph;
} rot_rotor_t;

typedef struct
{
  pcache_t pc;
  float horn[ROT_LEN];
  float drum[ROT_LEN];
  uint32_t w;
  float lp;
  float xover_k;
  rot_rotor_t horn_rotor, drum_rotor;
  uint32_t horn_inc, drum_inc;
  dsp_smooth_t drive;
  dsp_smooth_t drive_inv;
  dsp_smooth_t mix;
} rot_state_t;

_Static_assert(sizeof(rot_state_t) <= FX_STATE_BYTES, "rot_state_t too large");

static void rot_init(void *state, void *mem, uint32_t mem_len)
{
  (void)mem;
  (void)mem_len;
  memset(state, 0, sizeof(rot_state_t));
}

static void rot_update(void *state, const float *p)
{
  rot_state_t *s = state;
  const bool first = !s->pc.valid;
  if (!pcache_changed(&s->pc, p, 4))
  {
    return;
  }
  const bool fast = p[0] >= 0.5f;
  const float tau = 0.2f + 2.8f * p[1];
  s->xover_k = dsp_onepole_k(ROT_XOVER_HZ);
  s->horn_rotor.target = fast ? ROT_HORN_FAST : ROT_HORN_SLOW;
  s->drum_rotor.target = fast ? ROT_DRUM_FAST : ROT_DRUM_SLOW;
  /* The heavy drum lags the horn. */
  s->horn_rotor.k = 1.0f - expf(-(float)FX_BLOCK_FRAMES / (tau * DSP_FS));
  s->drum_rotor.k = 1.0f - expf(-(float)FX_BLOCK_FRAMES / (2.0f * tau * DSP_FS));
  if (first)
  {
    s->horn_rotor.freq = s->horn_rotor.target;
    s->drum_rotor.freq = s->drum_rotor.target;
  }
  const float g = 1.0f + 4.0f * p[2];
  sm_set(&s->drive, g, first);
  sm_set(&s->drive_inv, 1.0f / g, first);
  sm_set(&s->mix, p[3], first);
}

static void rot_spin(rot_rotor_t *r, uint32_t *inc)
{
  r->freq += r->k * (r->target - r->freq);
  *inc = dsp_phase_inc(r->freq);
}

/* One mic: Doppler delay (shortest when the source faces the mic) and level. */
static inline float rot_mic(const float *line, uint32_t w, uint32_t phase, float swing, float am)
{
  const float c = dsp_lfo_sin(phase + PHASE_QUARTER);   /* cos */
  const float y = dsp_read_frac_f(line, ROT_LEN - 1u, w, swing * (1.0f - c) + 2.0f);
  return y * (1.0f - 0.5f * am * (1.0f - c));
}

static void rot_process(void *state, float *l, float *r, uint32_t n)
{
  rot_state_t *s = state;
  rot_spin(&s->horn_rotor, &s->horn_inc);
  rot_spin(&s->drum_rotor, &s->drum_inc);

  for (uint32_t i = 0; i < n; i++)
  {
    const float g = dsp_smooth_next(&s->drive);
    const float mix = dsp_smooth_next(&s->mix);
    const float x = dsp_tanh(g * 0.5f * (l[i] + r[i])) * dsp_smooth_next(&s->drive_inv);

    s->lp += s->xover_k * (x - s->lp) + DSP_ANTI_DENORM;
    s->horn[s->w] = x - s->lp;
    s->drum[s->w] = s->lp;

    const uint32_t hp = s->horn_rotor.ph;
    const uint32_t dp = s->drum_rotor.ph;
    const float wl = rot_mic(s->horn, s->w, hp, ROT_HORN_SWING, ROT_HORN_AM) +
                     rot_mic(s->drum, s->w, dp, ROT_DRUM_SWING, ROT_DRUM_AM);
    const float wr = rot_mic(s->horn, s->w, hp + ROT_R_PHASE, ROT_HORN_SWING, ROT_HORN_AM) +
                     rot_mic(s->drum, s->w, dp + ROT_R_PHASE, ROT_DRUM_SWING, ROT_DRUM_AM);
    s->horn_rotor.ph += s->horn_inc;
    s->drum_rotor.ph += s->drum_inc;
    s->w = (s->w + 1u) & (ROT_LEN - 1u);

    l[i] = mix_lerp(l[i], wl, mix);
    r[i] = mix_lerp(r[i], wr, mix);
  }
}

const fx_algo_t g_fx_algo_ROTARY = {
  .id = FX_ALGO_ROTARY,
  .cost = 480,
  .init = rot_init,
  .update = rot_update,
  .process = rot_process,
};

/* ---- Ring modulator ------------------------------------------------------ */

#define RING_LFO_SWING 0.8f      /* carrier frequency swing at lfo_depth 1 */

typedef struct
{
  pcache_t pc;
  uint32_t ph;
  uint32_t lfo_ph, lfo_inc;
  dsp_smooth_t carrier_inc;   /* phase increment per sample, as float */
  dsp_smooth_t lfo_depth;
  dsp_smooth_t mix;
} ring_state_t;

_Static_assert(sizeof(ring_state_t) <= FX_STATE_BYTES, "ring_state_t too large");

static void ring_init(void *state, void *mem, uint32_t mem_len)
{
  (void)mem;
  (void)mem_len;
  memset(state, 0, sizeof(ring_state_t));
}

static void ring_update(void *state, const float *p)
{
  ring_state_t *s = state;
  const bool first = !s->pc.valid;
  if (!pcache_changed(&s->pc, p, 4))
  {
    return;
  }
  s->lfo_inc = dsp_phase_inc(p[1]);
  sm_set(&s->carrier_inc, p[0] * DSP_PHASE_PER_HZ, first);
  sm_set(&s->lfo_depth, p[2] * RING_LFO_SWING, first);
  sm_set(&s->mix, p[3], first);
}

static void ring_process(void *state, float *l, float *r, uint32_t n)
{
  ring_state_t *s = state;
  for (uint32_t i = 0; i < n; i++)
  {
    const float depth = dsp_smooth_next(&s->lfo_depth);
    const float lfo = dsp_lfo_sin(s->lfo_ph);
    s->lfo_ph += s->lfo_inc;
    s->ph += (uint32_t)(dsp_smooth_next(&s->carrier_inc) * (1.0f + depth * lfo));

    const float c = dsp_lfo_sin(s->ph);
    const float mix = dsp_smooth_next(&s->mix);
    l[i] = mix_lerp(l[i], l[i] * c, mix);
    r[i] = mix_lerp(r[i], r[i] * c, mix);
  }
}

const fx_algo_t g_fx_algo_RINGMOD = {
  .id = FX_ALGO_RINGMOD,
  .cost = 130,
  .init = ring_init,
  .update = ring_update,
  .process = ring_process,
};
