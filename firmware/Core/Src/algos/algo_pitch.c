/*
 * algo_pitch.c - Octaver, Harmonizer and Detune.
 *
 * All three use one granular (delay-sweep) pitch shifter: two read heads half
 * a grain apart sweep through a short int16 history, each faded by a
 * smoothstep window so the pair always sums to unity gain. A voice reads
 * the shared mono history, so several voices cost one buffer.
 */
#include <stdbool.h>
#include <string.h>

#include "dsp_util.h"
#include "fx_algo.h"

#define PS_BUF_SAMPLES   4096u                    /* octaver / harmonizer history */
#define PS_BUF_DETUNE    8192u                    /* detune adds up to 50 ms offset */
#define PS_GRAIN_LOW     2048.0f                  /* ~43 ms, suits guitar fundamentals */
#define PS_GRAIN_DETUNE  1536.0f                  /* ~32 ms */
/* Fraction of the grain a head takes to fade; short overlaps keep two
 * out-of-phase copies from cancelling for long. */
#define PS_XFADE         0.2f
#define PS_ALIGN_RANGE   640u                     /* > one period of the lowest guitar note */
#define PS_COARSE_STRIDE 16u
#define PS_COARSE_N      32u
#define PS_FINE_STRIDE   4u
#define PS_FINE_N        64u
#define PS_FINE_SPAN     12u
#define PS_MEM_LIMIT     (16u * 1024u)

/* ---- Shared granular shifter -------------------------------------------- */

typedef struct
{
  int16_t *buf;
  uint32_t mask;
  uint32_t wp;        /* slot of the newest sample */
  float grain;        /* samples */
} psbuf_t;

typedef struct
{
  float phase;        /* head 1 position in the grain, [0, 1); head 2 sits half a grain away */
  float step;         /* (1 - ratio) / grain */
  float adj[2];       /* per-head alignment offset in samples, chosen when the head restarts */
} psvoice_t;

static void psbuf_init(psbuf_t *b, void *mem, uint32_t samples, float grain)
{
  memset(mem, 0, samples * sizeof(int16_t));
  b->buf = mem;
  b->mask = samples - 1u;
  b->wp = 0;
  b->grain = grain;
}

static inline void psbuf_push(psbuf_t *b, float x)
{
  b->wp = (b->wp + 1u) & b->mask;
  b->buf[b->wp] = dsp_f_to_s16(x);
}

static inline int16_t psbuf_at(const psbuf_t *b, uint32_t delay)
{
  return b->buf[(b->wp - delay) & b->mask];
}

static inline float psbuf_read(const psbuf_t *b, float delay)
{
  const uint32_t di = (uint32_t)delay;
  const float fr = delay - (float)di;
  const float a = (float)psbuf_at(b, di);
  const float c = (float)psbuf_at(b, di + 1u);
  return (a + fr * (c - a)) * (1.0f / 32768.0f);
}

static void psvoice_set_ratio(psvoice_t *v, const psbuf_t *b, float ratio)
{
  v->step = (1.0f - ratio) / b->grain;
}

/*
 * Waveform alignment. Two heads reading at unrelated delays carry the same
 * tone at unrelated phases; each head hand-over then adds a phase jump that
 * biases the pitch by up to a dozen Hz and makes the crossfade cancel. When a
 * head restarts (weight 0), pick the delay offset that makes its signal best
 * match what the other head is playing now, so the crossfade is coherent.
 */
static float align_score(const psbuf_t *b, uint32_t d_ref, uint32_t d_cand, uint32_t stride, uint32_t n)
{
  int32_t dot = 0, en = 0;
  for (uint32_t k = 0; k < n; k++)
  {
    const int32_t a = psbuf_at(b, d_ref + k * stride) >> 4;
    const int32_t c = psbuf_at(b, d_cand + k * stride) >> 4;
    dot += a * c;
    en += c * c;
  }
  return dot > 0 ? (float)dot / sqrtf((float)en + 1.0f) : 0.0f;
}

static float align_pick(const psbuf_t *b, uint32_t d_ref, uint32_t d_base)
{
  float best = 0.0f;
  uint32_t coarse = 0;
  for (uint32_t adj = 0; adj <= PS_ALIGN_RANGE; adj += PS_COARSE_STRIDE)
  {
    const float sc = align_score(b, d_ref, d_base + adj, PS_COARSE_STRIDE, PS_COARSE_N);
    if (sc > best)
    {
      best = sc;
      coarse = adj;
    }
  }
  if (best <= 0.0f)
  {
    return 0.0f;   /* silence or no correlation: keep the nominal position */
  }

  uint32_t pick = coarse;
  best = 0.0f;
  const uint32_t lo = coarse > PS_FINE_SPAN ? coarse - PS_FINE_SPAN : 0u;
  for (uint32_t adj = lo; adj <= coarse + PS_FINE_SPAN; adj++)
  {
    const float sc = align_score(b, d_ref, d_base + adj, PS_FINE_STRIDE, PS_FINE_N);
    if (sc > best)
    {
      best = sc;
      pick = adj;
    }
  }
  return (float)pick;
}

/* extra = constant added to both head delays (samples). */
static inline float psvoice_run(psvoice_t *v, const psbuf_t *b, float extra)
{
  const float p1 = v->phase;
  const bool second = p1 >= 0.5f;
  const float p2 = second ? p1 - 0.5f : p1 + 0.5f;
  const float u = dsp_clampf((second ? p2 : p1) * (1.0f / PS_XFADE), 0.0f, 1.0f);
  const float sm = u * u * (3.0f - 2.0f * u);
  const float w1 = second ? 1.0f - sm : sm;   /* heads sum to unity */

  const float y = w1 * psbuf_read(b, p1 * b->grain + v->adj[0] + extra) +
                  (1.0f - w1) * psbuf_read(b, p2 * b->grain + v->adj[1] + extra);

  float np = p1 + v->step;
  int restart = -1;   /* head that just wrapped */
  if (np >= 1.0f || np < 0.0f)
  {
    np += np >= 1.0f ? -1.0f : 1.0f;
    restart = 0;
  }
  else if ((p1 < 0.5f) != (np < 0.5f))
  {
    restart = 1;
  }
  v->phase = np;

  if (restart >= 0)
  {
    const float q1 = np;
    const float q2 = q1 < 0.5f ? q1 + 0.5f : q1 - 0.5f;
    const float pr = restart ? q2 : q1;
    const float po = restart ? q1 : q2;
    const uint32_t d_ref = (uint32_t)(po * b->grain + v->adj[1 - restart]);
    v->adj[restart] = align_pick(b, d_ref, (uint32_t)(pr * b->grain));
  }
  return y;
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

static inline float semitone_ratio(float st)
{
  return powf(2.0f, st * (1.0f / 12.0f));
}

/* ---- Octaver ------------------------------------------------------------- */

typedef struct
{
  psbuf_t buf;
  psvoice_t sub1, sub2, up;
  dsp_smooth_t g_sub1, g_sub2, g_up, g_dry;
  float lp, lp_k;
  float cache[FX_ALGO_MAX_PARAMS];
  bool primed;
} octaver_t;

_Static_assert(sizeof(octaver_t) <= FX_STATE_BYTES, "octaver_t exceeds state area");
_Static_assert(PS_BUF_SAMPLES * sizeof(int16_t) <= PS_MEM_LIMIT, "pitch buffer too large");

static void octaver_init(void *state, void *mem, uint32_t mem_len)
{
  (void)mem_len;
  octaver_t *s = state;
  memset(s, 0, sizeof(*s));
  psbuf_init(&s->buf, mem, PS_BUF_SAMPLES, PS_GRAIN_LOW);
  psvoice_set_ratio(&s->sub1, &s->buf, 0.5f);
  psvoice_set_ratio(&s->sub2, &s->buf, 0.25f);
  psvoice_set_ratio(&s->up, &s->buf, 2.0f);
  s->sub2.phase = 0.25f;   /* spread grain edges so voices do not cross-fade together */
  s->up.phase = 0.5f;
  memset(s->cache, 0xFF, sizeof(s->cache));
}

/* p = sub1, sub2, up, tone, dry */
static void octaver_update(void *state, const float *p)
{
  octaver_t *s = state;
  if (!params_changed(s->cache, p, 5))
  {
    return;
  }
  s->g_sub1.tgt = dsp_clampf(p[0], 0.0f, 1.0f);
  s->g_sub2.tgt = dsp_clampf(p[1], 0.0f, 1.0f);
  s->g_up.tgt = dsp_clampf(p[2], 0.0f, 1.0f);
  s->g_dry.tgt = dsp_clampf(p[4], 0.0f, 1.0f);
  s->lp_k = dsp_onepole_k(500.0f * powf(16.0f, dsp_clampf(p[3], 0.0f, 1.0f)));
  if (!s->primed)
  {
    dsp_smooth_snap(&s->g_sub1, s->g_sub1.tgt);
    dsp_smooth_snap(&s->g_sub2, s->g_sub2.tgt);
    dsp_smooth_snap(&s->g_up, s->g_up.tgt);
    dsp_smooth_snap(&s->g_dry, s->g_dry.tgt);
    s->primed = true;
  }
}

static void octaver_process(void *state, float *l, float *r, uint32_t n)
{
  octaver_t *s = state;

  for (uint32_t i = 0; i < n; i++)
  {
    psbuf_push(&s->buf, 0.5f * (l[i] + r[i]));

    const float g1 = dsp_smooth_next(&s->g_sub1);
    const float g2 = dsp_smooth_next(&s->g_sub2);
    const float gu = dsp_smooth_next(&s->g_up);
    float v = 0.0f;
    if (g1 > 0.0f)
    {
      v += g1 * psvoice_run(&s->sub1, &s->buf, 0.0f);
    }
    if (g2 > 0.0f)
    {
      v += g2 * psvoice_run(&s->sub2, &s->buf, 0.0f);
    }
    if (gu > 0.0f)
    {
      v += gu * psvoice_run(&s->up, &s->buf, 0.0f);
    }
    s->lp += s->lp_k * (v - s->lp) + DSP_ANTI_DENORM;

    const float gd = dsp_smooth_next(&s->g_dry);
    l[i] = l[i] * gd + s->lp;
    r[i] = r[i] * gd + s->lp;
  }
}

const fx_algo_t g_fx_algo_OCTAVER = {
  .id = FX_ALGO_OCTAVER,
  .mem_bytes = PS_BUF_SAMPLES * sizeof(int16_t),
  .cost = 650,   /* 3 voices + worst-case simultaneous waveform alignments */
  .tails = 1,
  .init = octaver_init,
  .update = octaver_update,
  .process = octaver_process,
};

/* ---- Harmonizer ---------------------------------------------------------- */

typedef struct
{
  psbuf_t buf;
  psvoice_t v1, v2;
  dsp_smooth_t g1, g2, g_dry;
  float cache[FX_ALGO_MAX_PARAMS];
  bool primed;
} harmonizer_t;

_Static_assert(sizeof(harmonizer_t) <= FX_STATE_BYTES, "harmonizer_t exceeds state area");

/* Voices sit slightly left and right of centre. */
#define HARM_NEAR 1.0f
#define HARM_FAR  0.45f

static void harmonizer_init(void *state, void *mem, uint32_t mem_len)
{
  (void)mem_len;
  harmonizer_t *s = state;
  memset(s, 0, sizeof(*s));
  psbuf_init(&s->buf, mem, PS_BUF_SAMPLES, PS_GRAIN_LOW);
  s->v2.phase = 0.25f;
  memset(s->cache, 0xFF, sizeof(s->cache));
}

/* p = voice1, voice2, level1, level2, dry */
static void harmonizer_update(void *state, const float *p)
{
  harmonizer_t *s = state;
  if (!params_changed(s->cache, p, 5))
  {
    return;
  }
  psvoice_set_ratio(&s->v1, &s->buf, semitone_ratio(dsp_clampf(p[0], -12.0f, 12.0f)));
  psvoice_set_ratio(&s->v2, &s->buf, semitone_ratio(dsp_clampf(p[1], -12.0f, 12.0f)));
  s->g1.tgt = dsp_clampf(p[2], 0.0f, 1.0f);
  s->g2.tgt = dsp_clampf(p[3], 0.0f, 1.0f);
  s->g_dry.tgt = dsp_clampf(p[4], 0.0f, 1.0f);
  if (!s->primed)
  {
    dsp_smooth_snap(&s->g1, s->g1.tgt);
    dsp_smooth_snap(&s->g2, s->g2.tgt);
    dsp_smooth_snap(&s->g_dry, s->g_dry.tgt);
    s->primed = true;
  }
}

static void harmonizer_process(void *state, float *l, float *r, uint32_t n)
{
  harmonizer_t *s = state;

  for (uint32_t i = 0; i < n; i++)
  {
    psbuf_push(&s->buf, 0.5f * (l[i] + r[i]));

    const float g1 = dsp_smooth_next(&s->g1);
    const float g2 = dsp_smooth_next(&s->g2);
    const float a = g1 > 0.0f ? g1 * psvoice_run(&s->v1, &s->buf, 0.0f) : 0.0f;
    const float b = g2 > 0.0f ? g2 * psvoice_run(&s->v2, &s->buf, 0.0f) : 0.0f;

    const float gd = dsp_smooth_next(&s->g_dry);
    l[i] = l[i] * gd + a * HARM_NEAR + b * HARM_FAR;
    r[i] = r[i] * gd + a * HARM_FAR + b * HARM_NEAR;
  }
}

const fx_algo_t g_fx_algo_HARMONIZER = {
  .id = FX_ALGO_HARMONIZER,
  .mem_bytes = PS_BUF_SAMPLES * sizeof(int16_t),
  .cost = 450,
  .tails = 1,
  .init = harmonizer_init,
  .update = harmonizer_update,
  .process = harmonizer_process,
};

/* ---- Detune -------------------------------------------------------------- */

typedef struct
{
  psbuf_t buf;
  psvoice_t up, down;
  float offset;           /* samples */
  dsp_smooth_t mix;
  float cache[FX_ALGO_MAX_PARAMS];
  bool primed;
} detune_t;

_Static_assert(sizeof(detune_t) <= FX_STATE_BYTES, "detune_t exceeds state area");
_Static_assert(PS_BUF_DETUNE * sizeof(int16_t) <= PS_MEM_LIMIT, "detune buffer too large");

static void detune_init(void *state, void *mem, uint32_t mem_len)
{
  (void)mem_len;
  detune_t *s = state;
  memset(s, 0, sizeof(*s));
  psbuf_init(&s->buf, mem, PS_BUF_DETUNE, PS_GRAIN_DETUNE);
  s->down.phase = 0.5f;
  memset(s->cache, 0xFF, sizeof(s->cache));
}

/* p = cents, delay, mix */
static void detune_update(void *state, const float *p)
{
  detune_t *s = state;
  if (!params_changed(s->cache, p, 3))
  {
    return;
  }
  const float cents = dsp_clampf(p[0], 0.0f, 50.0f);
  psvoice_set_ratio(&s->up, &s->buf, semitone_ratio(cents * 0.01f));
  psvoice_set_ratio(&s->down, &s->buf, semitone_ratio(-cents * 0.01f));
  s->offset = dsp_ms_to_samples(dsp_clampf(p[1], 0.0f, 50.0f));
  s->mix.tgt = dsp_clampf(p[2], 0.0f, 1.0f);
  if (!s->primed)
  {
    dsp_smooth_snap(&s->mix, s->mix.tgt);
    s->primed = true;
  }
}

static void detune_process(void *state, float *l, float *r, uint32_t n)
{
  detune_t *s = state;

  for (uint32_t i = 0; i < n; i++)
  {
    psbuf_push(&s->buf, 0.5f * (l[i] + r[i]));
    const float wl = psvoice_run(&s->up, &s->buf, s->offset);
    const float wr = psvoice_run(&s->down, &s->buf, s->offset);

    const float m = dsp_smooth_next(&s->mix);
    l[i] = l[i] * (1.0f - m) + wl * m;
    r[i] = r[i] * (1.0f - m) + wr * m;
  }
}

const fx_algo_t g_fx_algo_DETUNE = {
  .id = FX_ALGO_DETUNE,
  .mem_bytes = PS_BUF_DETUNE * sizeof(int16_t),
  .cost = 420,
  .tails = 1,
  .init = detune_init,
  .update = detune_update,
  .process = detune_process,
};
