/*
 * algo_delay.c - Digital, Tape, Mod, Ping-Pong and Reverse delays.
 *
 * Delay lines are int16 at 48 kHz. Digital/Tape/Mod share one core (modulated
 * read tap, filtered + soft-limited feedback); they differ only in update().
 * The feedback limiter is dsp_tanh() on the write path, so loop gain stays
 * below fb < 1 for small signals and the line can never run away.
 */
#include <stdbool.h>
#include <string.h>

#include "dsp_util.h"
#include "fx_algo.h"

#define DLY_MAX_SAMPLES  36000u                   /* 750 ms */
#define PP_MAX_SAMPLES   18000u                   /* 375 ms per line */
#define DLY_MARGIN       64u                      /* interpolation headroom */
#define DLY_LEN          (DLY_MAX_SAMPLES + DLY_MARGIN)
#define PP_LEN           (PP_MAX_SAMPLES + DLY_MARGIN)

#define DLY_SLEW_K       0.001f
#define DLY_SLEW_MAX     0.5f                     /* samples/sample: at most one octave of glide */
#define DLY_MIN_DELAY    2.0f
#define DLY_MIN_TIME     64.0f                    /* samples, floor for requested time */
#define DLY_FB_MAX       0.95f

#define REV_FADE         480u                     /* 10 ms edge fade per window */
#define REV_MIN_WIN      (2u * REV_FADE)

#define DLY_MEM_LIMIT    (76u * 1024u)

/* ---- Delay line ---------------------------------------------------------- */

typedef struct
{
  int16_t *buf;
  uint32_t len;
  uint32_t wp;    /* next slot to write; wp - 1 is the newest sample */
} dline_t;

static void dline_init(dline_t *d, int16_t *buf, uint32_t len)
{
  memset(buf, 0, len * sizeof(int16_t));
  d->buf = buf;
  d->len = len;
  d->wp = 0;
}

/* Linear-interpolated read; delay in [2, len - 2] samples. */
static inline float dline_read(const dline_t *d, float delay)
{
  const uint32_t di = (uint32_t)delay;
  const float fr = delay - (float)di;
  const uint32_t i0 = d->wp >= di ? d->wp - di : d->wp + d->len - di;
  const uint32_t i1 = i0 ? i0 - 1u : d->len - 1u;
  const float a = (float)d->buf[i0];
  const float b = (float)d->buf[i1];
  return (a + fr * (b - a)) * (1.0f / 32768.0f);
}

static inline void dline_write(dline_t *d, float x)
{
  d->buf[d->wp] = dsp_f_to_s16(x);
  d->wp = (d->wp + 1u == d->len) ? 0u : d->wp + 1u;
}

static inline float slew_delay(float cur, float tgt)
{
  return cur + dsp_clampf(DLY_SLEW_K * (tgt - cur), -DLY_SLEW_MAX, DLY_SLEW_MAX);
}

static inline float mono_in(float l, float r)
{
  return 0.5f * (l + r);
}

static bool params_changed(float *cache, const float *p, uint32_t n)
{
  if (memcmp(cache, p, n * sizeof(float)) == 0)
  {
    return false;
  }
  memcpy(cache, p, n * sizeof(float));
  return true;
}

static float time_samples(float ms, float max_samples)
{
  return dsp_clampf(dsp_ms_to_samples(ms), DLY_MIN_TIME, max_samples);
}

/* ---- Digital / Tape / Mod core ------------------------------------------ */

typedef struct
{
  dline_t line;
  float cur, tgt;                 /* delay in samples */
  uint32_t ph1, inc1, ph2, inc2;  /* modulation LFOs */
  float depth1, depth2;           /* modulation depth in samples */
  float lp, lp_k;                 /* feedback low-pass */
  float hp, hp_k;                 /* feedback high-pass (k = 0 disables) */
  float sat, inv_sat;             /* feedback saturation drive (0 = off) */
  dsp_smooth_t fb, mix;
  float cache[FX_ALGO_MAX_PARAMS];
  bool primed;
} delay_t;

_Static_assert(sizeof(delay_t) <= FX_STATE_BYTES, "delay_t exceeds state area");
_Static_assert(DLY_LEN * sizeof(int16_t) <= DLY_MEM_LIMIT, "delay line too large");

static void delay_init(void *state, void *mem, uint32_t mem_len)
{
  (void)mem_len;
  delay_t *s = state;
  memset(s, 0, sizeof(*s));
  dline_init(&s->line, mem, DLY_LEN);
  memset(s->cache, 0xFF, sizeof(s->cache));
}

static void delay_commit(delay_t *s, float time_ms, float fb, float mix)
{
  s->tgt = time_samples(time_ms, (float)DLY_MAX_SAMPLES);
  s->fb.tgt = dsp_clampf(fb, 0.0f, DLY_FB_MAX);
  s->mix.tgt = dsp_clampf(mix, 0.0f, 1.0f);
  if (!s->primed)
  {
    s->cur = s->tgt;
    dsp_smooth_snap(&s->fb, s->fb.tgt);
    dsp_smooth_snap(&s->mix, s->mix.tgt);
    s->primed = true;
  }
}

static void delay_process(void *state, float *l, float *r, uint32_t n)
{
  delay_t *s = state;
  const float max_delay = (float)(s->line.len - 2u);

  for (uint32_t i = 0; i < n; i++)
  {
    const float x = mono_in(l[i], r[i]);

    s->cur = slew_delay(s->cur, s->tgt);
    const float d = s->cur + s->depth1 * dsp_lfo_sin(s->ph1) + s->depth2 * dsp_lfo_sin(s->ph2);
    s->ph1 += s->inc1;
    s->ph2 += s->inc2;
    const float wet = dline_read(&s->line, dsp_clampf(d, DLY_MIN_DELAY, max_delay));

    s->lp += s->lp_k * (wet - s->lp) + DSP_ANTI_DENORM;
    s->hp += s->hp_k * (s->lp - s->hp);
    float y = s->lp - s->hp;
    if (s->sat > 0.0f)
    {
      y = dsp_tanh(s->sat * y) * s->inv_sat;
    }
    dline_write(&s->line, dsp_tanh(x + dsp_smooth_next(&s->fb) * y));

    const float m = dsp_smooth_next(&s->mix);
    l[i] = l[i] * (1.0f - m) + wet * m;
    r[i] = r[i] * (1.0f - m) + wet * m;
  }
}

/* Digital: p = time, feedback, tone, mod, mix */
static void digital_update(void *state, const float *p)
{
  delay_t *s = state;
  if (!params_changed(s->cache, p, 5))
  {
    return;
  }
  s->lp_k = dsp_onepole_k(1500.0f * powf(10.0f, dsp_clampf(p[2], 0.0f, 1.0f)));
  s->depth1 = dsp_clampf(p[3], 0.0f, 1.0f) * dsp_ms_to_samples(2.0f);
  s->inc1 = dsp_phase_inc(1.3f);
  delay_commit(s, p[0], p[1], p[4]);
}

/* Tape: p = time, feedback, wow, saturate, age, mix */
static void tape_update(void *state, const float *p)
{
  delay_t *s = state;
  if (!params_changed(s->cache, p, 6))
  {
    return;
  }
  const float wow = dsp_clampf(p[2], 0.0f, 1.0f);
  const float sat = dsp_clampf(p[3], 0.0f, 1.0f);
  const float age = dsp_clampf(p[4], 0.0f, 1.0f);

  s->depth1 = wow * dsp_ms_to_samples(1.2f);
  s->inc1 = dsp_phase_inc(0.55f);
  s->depth2 = wow * dsp_ms_to_samples(0.1f);
  s->inc2 = dsp_phase_inc(6.8f);
  s->sat = sat > 0.0f ? 1.0f + 4.0f * sat : 0.0f;
  s->inv_sat = sat > 0.0f ? 1.0f / s->sat : 0.0f;
  s->lp_k = dsp_onepole_k(7000.0f * powf(0.15f, age));
  s->hp_k = dsp_onepole_k(30.0f + 220.0f * age);
  delay_commit(s, p[0], p[1], p[5]);
}

/* Mod: p = time, feedback, rate, depth, mix */
static void mod_update(void *state, const float *p)
{
  delay_t *s = state;
  if (!params_changed(s->cache, p, 5))
  {
    return;
  }
  s->lp_k = dsp_onepole_k(6000.0f);
  s->depth1 = dsp_clampf(p[3], 0.0f, 1.0f) * dsp_ms_to_samples(4.0f);
  s->inc1 = dsp_phase_inc(dsp_clampf(p[2], 0.1f, 5.0f));
  delay_commit(s, p[0], p[1], p[4]);
}

const fx_algo_t g_fx_algo_DIGITAL_DLY = {
  .id = FX_ALGO_DIGITAL_DLY,
  .mem_bytes = DLY_LEN * sizeof(int16_t),
  .cost = 200,
  .tails = 1,
  .init = delay_init,
  .update = digital_update,
  .process = delay_process,
};

const fx_algo_t g_fx_algo_TAPE_ECHO = {
  .id = FX_ALGO_TAPE_ECHO,
  .mem_bytes = DLY_LEN * sizeof(int16_t),
  .cost = 260,
  .tails = 1,
  .init = delay_init,
  .update = tape_update,
  .process = delay_process,
};

const fx_algo_t g_fx_algo_MOD_DLY = {
  .id = FX_ALGO_MOD_DLY,
  .mem_bytes = DLY_LEN * sizeof(int16_t),
  .cost = 220,
  .tails = 1,
  .init = delay_init,
  .update = mod_update,
  .process = delay_process,
};

/* ---- Ping-Pong ----------------------------------------------------------- */

typedef struct
{
  dline_t a, b;                   /* a is heard first (left), b second (right) */
  float cur, tgt;
  float lp_a, lp_b, lp_k;
  dsp_smooth_t fb, mix, spread;
  float cache[FX_ALGO_MAX_PARAMS];
  bool primed;
} pingpong_t;

_Static_assert(sizeof(pingpong_t) <= FX_STATE_BYTES, "pingpong_t exceeds state area");
_Static_assert(2u * PP_LEN * sizeof(int16_t) <= DLY_MEM_LIMIT, "ping-pong lines too large");

static void pingpong_init(void *state, void *mem, uint32_t mem_len)
{
  (void)mem_len;
  pingpong_t *s = state;
  int16_t *buf = mem;
  memset(s, 0, sizeof(*s));
  dline_init(&s->a, buf, PP_LEN);
  dline_init(&s->b, buf + PP_LEN, PP_LEN);
  memset(s->cache, 0xFF, sizeof(s->cache));
}

/* p = time, feedback, tone, spread, mix */
static void pingpong_update(void *state, const float *p)
{
  pingpong_t *s = state;
  if (!params_changed(s->cache, p, 5))
  {
    return;
  }
  s->tgt = time_samples(p[0], (float)PP_MAX_SAMPLES);
  s->lp_k = dsp_onepole_k(1500.0f * powf(10.0f, dsp_clampf(p[2], 0.0f, 1.0f)));
  s->fb.tgt = dsp_clampf(p[1], 0.0f, DLY_FB_MAX);
  s->spread.tgt = dsp_clampf(p[3], 0.0f, 1.0f);
  s->mix.tgt = dsp_clampf(p[4], 0.0f, 1.0f);
  if (!s->primed)
  {
    s->cur = s->tgt;
    dsp_smooth_snap(&s->fb, s->fb.tgt);
    dsp_smooth_snap(&s->spread, s->spread.tgt);
    dsp_smooth_snap(&s->mix, s->mix.tgt);
    s->primed = true;
  }
}

static void pingpong_process(void *state, float *l, float *r, uint32_t n)
{
  pingpong_t *s = state;

  for (uint32_t i = 0; i < n; i++)
  {
    const float x = mono_in(l[i], r[i]);

    s->cur = slew_delay(s->cur, s->tgt);
    const float ra = dline_read(&s->a, s->cur);
    const float rb = dline_read(&s->b, s->cur);
    s->lp_a += s->lp_k * (ra - s->lp_a) + DSP_ANTI_DENORM;
    s->lp_b += s->lp_k * (rb - s->lp_b) + DSP_ANTI_DENORM;

    /* Each hop carries fb, so the repeat level falls by fb per delay time. */
    const float fb = dsp_smooth_next(&s->fb);
    dline_write(&s->a, dsp_tanh(x + fb * s->lp_b));
    dline_write(&s->b, dsp_tanh(fb * s->lp_a));

    const float sp = dsp_smooth_next(&s->spread);
    const float near_g = 0.5f + 0.5f * sp;
    const float far_g = 0.5f - 0.5f * sp;
    const float m = dsp_smooth_next(&s->mix);
    l[i] = l[i] * (1.0f - m) + (ra * near_g + rb * far_g) * m;
    r[i] = r[i] * (1.0f - m) + (rb * near_g + ra * far_g) * m;
  }
}

const fx_algo_t g_fx_algo_PINGPONG = {
  .id = FX_ALGO_PINGPONG,
  .mem_bytes = 2u * PP_LEN * sizeof(int16_t),
  .cost = 300,
  .tails = 1,
  .init = pingpong_init,
  .update = pingpong_update,
  .process = pingpong_process,
};

/* ---- Reverse ------------------------------------------------------------- */

/*
 * One buffer of exactly one window. Every slot is read and then overwritten
 * with the new input at the same index, and the access direction flips each
 * window: the window just recorded is always played back in reverse order of
 * its recording while the next one is recorded in place.
 */
typedef struct
{
  int16_t *buf;
  uint32_t win, tgt_win;   /* window length in use / requested for the next one */
  uint32_t t;              /* position inside the window */
  bool descending;         /* index runs win-1 .. 0 in this window */
  bool filled;             /* buffer holds a complete previous window of length win */
  dsp_smooth_t fb, mix;
  float cache[FX_ALGO_MAX_PARAMS];
  bool primed;
} reverse_t;

_Static_assert(sizeof(reverse_t) <= FX_STATE_BYTES, "reverse_t exceeds state area");
_Static_assert(DLY_MAX_SAMPLES * sizeof(int16_t) <= DLY_MEM_LIMIT, "reverse buffer too large");

static void reverse_init(void *state, void *mem, uint32_t mem_len)
{
  (void)mem_len;
  reverse_t *s = state;
  memset(s, 0, sizeof(*s));
  s->buf = mem;
  memset(mem, 0, DLY_MAX_SAMPLES * sizeof(int16_t));
  memset(s->cache, 0xFF, sizeof(s->cache));
}

/* p = time, feedback, mix */
static void reverse_update(void *state, const float *p)
{
  reverse_t *s = state;
  if (!params_changed(s->cache, p, 3))
  {
    return;
  }
  const float samples = dsp_clampf(dsp_ms_to_samples(p[0]), (float)REV_MIN_WIN, (float)DLY_MAX_SAMPLES);
  s->tgt_win = (uint32_t)samples;
  s->fb.tgt = dsp_clampf(p[1], 0.0f, 0.9f);
  s->mix.tgt = dsp_clampf(p[2], 0.0f, 1.0f);
  if (!s->primed)
  {
    s->win = s->tgt_win;
    dsp_smooth_snap(&s->fb, s->fb.tgt);
    dsp_smooth_snap(&s->mix, s->mix.tgt);
    s->primed = true;
  }
}

static void reverse_next_window(reverse_t *s)
{
  s->t = 0;
  if (s->tgt_win != s->win)
  {
    /* Layout of the old window is meaningless at a new length: play silence once. */
    s->win = s->tgt_win;
    s->filled = false;
    s->descending = false;
  }
  else
  {
    s->filled = true;
    s->descending = !s->descending;
  }
}

static void reverse_process(void *state, float *l, float *r, uint32_t n)
{
  reverse_t *s = state;
  const float inv_fade = 1.0f / (float)REV_FADE;

  for (uint32_t i = 0; i < n; i++)
  {
    if (s->t >= s->win)
    {
      reverse_next_window(s);
    }
    const uint32_t back = s->win - 1u - s->t;
    const uint32_t idx = s->descending ? back : s->t;
    const uint32_t edge = s->t < back ? s->t : back;
    const float env = edge < REV_FADE ? (float)edge * inv_fade : 1.0f;
    const float wet = s->filled ? dsp_s16_to_f(s->buf[idx]) * env : 0.0f;
    s->t++;

    s->buf[idx] = dsp_f_to_s16(dsp_tanh(mono_in(l[i], r[i]) + dsp_smooth_next(&s->fb) * wet));

    const float m = dsp_smooth_next(&s->mix);
    l[i] = l[i] * (1.0f - m) + wet * m;
    r[i] = r[i] * (1.0f - m) + wet * m;
  }
}

const fx_algo_t g_fx_algo_REVERSE_DLY = {
  .id = FX_ALGO_REVERSE_DLY,
  .mem_bytes = DLY_MAX_SAMPLES * sizeof(int16_t),
  .cost = 150,
  .tails = 1,
  .init = reverse_init,
  .update = reverse_update,
  .process = reverse_process,
};
