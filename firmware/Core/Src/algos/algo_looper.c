/*
 * algo_looper.c - Looper storing 8-bit mu-law at 24 kHz mono.
 *
 * Record: mono sum, 3-tap low-pass, decimate 2:1, G.711 mu-law encode.
 * Playback: decode, linear interpolation back to 48 kHz. The loop edges are
 * faded (first/last LOOP_XFADE samples) at record time so the wrap is silent
 * and click-free; start/stop use a short gain ramp.
 *
 * Commands arrive through a volatile request that process() applies at the
 * start of the next block, so command() is safe from any context.
 */
#include <stdbool.h>
#include <string.h>

#include "dsp_util.h"
#include "fx_algo.h"

#define LOOP_RATE_DIV    2u                       /* 48 kHz -> 24 kHz */
#define LOOP_XFADE       96u                      /* 4 ms of 24 kHz samples */
#define LOOP_MIN_LEN     (2u * LOOP_XFADE)
#define LOOP_MIN_MEM     24000u                   /* 1 s */
#define LOOP_FRAC_BITS   8u                       /* playback position fraction */
#define LOOP_FADE_STEP   (1.0f / 240.0f)          /* 5 ms play gain ramp */
#define LOOP_NO_SLOT     0xFFFFFFFFu

/* ---- Float <-> mu-law (shared codec in dsp_util.h) ---------------------- */

static inline uint8_t ulaw_encode(float x)
{
  return dsp_ulaw_encode((int16_t)(dsp_clampf(x, -1.0f, 1.0f) * 32767.0f));
}

static inline float ulaw_decode(uint8_t b)
{
  return (float)dsp_ulaw_decode(b) * (1.0f / 32768.0f);
}

/* ---- State --------------------------------------------------------------- */

typedef struct
{
  uint8_t *mem;
  uint32_t cap;                  /* capacity, 24 kHz samples */
  volatile uint32_t pending;     /* fx_cmd_t + 1, 0 = none */

  uint32_t state;                /* fx_loop_state_t */
  uint32_t len;                  /* closed loop length, 24 kHz samples */
  uint32_t wr;                   /* samples recorded so far */
  uint32_t pos;                  /* playback position, 24 kHz samples << LOOP_FRAC_BITS */
  uint32_t last_dub;             /* last slot overdubbed, LOOP_NO_SLOT = none */

  float x1, x2;                  /* decimation filter history */
  float dec;                     /* latest decimated input sample */
  bool odd;                      /* frame parity for 2:1 decimation */
  float play_g;                  /* start/stop fade */

  dsp_smooth_t level;
  float dub_fb;
  uint32_t step;                 /* position increment per output sample */
  bool reverse;
} looper_t;

_Static_assert(sizeof(looper_t) <= FX_STATE_BYTES, "looper_t exceeds state area");

static void looper_init(void *state, void *mem, uint32_t mem_len)
{
  looper_t *s = state;
  memset(s, 0, sizeof(*s));
  s->mem = mem;
  s->cap = mem_len;
  s->state = FX_LOOP_EMPTY;
  s->last_dub = LOOP_NO_SLOT;
  s->step = 1u << (LOOP_FRAC_BITS - 1u);
  s->dub_fb = 0.9f;
  dsp_smooth_snap(&s->level, 1.0f);
  memset(mem, 0xFF, mem_len);   /* mu-law positive zero */
}

/* p = level, dub_fb, reverse, half */
static void looper_update(void *state, const float *p)
{
  looper_t *s = state;
  s->level.tgt = dsp_clampf(p[0], 0.0f, 1.0f);
  s->dub_fb = dsp_clampf(p[1], 0.0f, 1.0f);
  s->reverse = p[2] > 0.5f;
  s->step = (p[3] > 0.5f ? 1u : 2u) << (LOOP_FRAC_BITS - 2u);
}

static void looper_command(void *state, fx_cmd_t cmd)
{
  looper_t *s = state;
  s->pending = (uint32_t)cmd + 1u;
}

static void start_record(looper_t *s)
{
  s->state = FX_LOOP_RECORDING;
  s->wr = 0;
  s->len = 0;
  s->pos = 0;
  s->x1 = 0.0f;
  s->x2 = 0.0f;
  s->odd = false;
}

static void fade_out_tail(looper_t *s)
{
  for (uint32_t k = 0; k < LOOP_XFADE; k++)
  {
    const uint32_t idx = s->wr - 1u - k;
    const float g = (float)k * (1.0f / (float)LOOP_XFADE);
    s->mem[idx] = ulaw_encode(ulaw_decode(s->mem[idx]) * g);
  }
}

/* Fixes the loop length; a recording too short to fade is discarded. */
static void close_loop(looper_t *s, fx_loop_state_t next)
{
  if (s->wr < LOOP_MIN_LEN)
  {
    s->state = FX_LOOP_EMPTY;
    s->len = 0;
    s->wr = 0;
    return;
  }
  fade_out_tail(s);
  s->len = s->wr;
  s->pos = 0;
  s->play_g = 0.0f;
  s->last_dub = LOOP_NO_SLOT;
  s->state = next;
}

static void apply_command(looper_t *s, fx_cmd_t cmd)
{
  switch (cmd)
  {
    case FX_CMD_REC:
      if (s->state == FX_LOOP_RECORDING)
      {
        close_loop(s, FX_LOOP_PLAYING);
      }
      else
      {
        start_record(s);   /* empty, or re-record over an existing loop */
      }
      break;
    case FX_CMD_DUB:
      if (s->state == FX_LOOP_PLAYING)
      {
        s->last_dub = LOOP_NO_SLOT;
        s->state = FX_LOOP_OVERDUB;
      }
      else if (s->state == FX_LOOP_OVERDUB)
      {
        s->state = FX_LOOP_PLAYING;
      }
      break;
    case FX_CMD_PLAY:
      if (s->state == FX_LOOP_RECORDING)
      {
        close_loop(s, FX_LOOP_PLAYING);
      }
      else if (s->state == FX_LOOP_STOPPED)
      {
        s->pos = 0;
        s->state = FX_LOOP_PLAYING;
      }
      else if (s->state == FX_LOOP_OVERDUB)
      {
        s->state = FX_LOOP_PLAYING;
      }
      break;
    case FX_CMD_STOP:
      if (s->state == FX_LOOP_RECORDING)
      {
        close_loop(s, FX_LOOP_STOPPED);
      }
      else if (s->state == FX_LOOP_PLAYING || s->state == FX_LOOP_OVERDUB)
      {
        s->state = FX_LOOP_STOPPED;
      }
      break;
    case FX_CMD_CLEAR:
      s->state = FX_LOOP_EMPTY;
      s->len = 0;
      s->wr = 0;
      s->pos = 0;
      break;
    default:
      break;
  }
}

/* ---- Audio --------------------------------------------------------------- */

/* Returns true when a new 24 kHz sample is available in s->dec. */
static inline bool decimate(looper_t *s, float x)
{
  const bool out = s->odd;
  s->odd = !s->odd;
  if (out)
  {
    s->dec = 0.25f * x + 0.5f * s->x1 + 0.25f * s->x2;
  }
  s->x2 = s->x1;
  s->x1 = x;
  return out;
}

static inline void record_sample(looper_t *s)
{
  const float g = s->wr < LOOP_XFADE ? (float)s->wr * (1.0f / (float)LOOP_XFADE) : 1.0f;
  s->mem[s->wr++] = ulaw_encode(s->dec * g);
  if (s->wr >= s->cap)
  {
    close_loop(s, FX_LOOP_PLAYING);   /* capacity reached: loop plays on its own */
  }
}

static inline void advance(looper_t *s)
{
  const uint32_t span = s->len << LOOP_FRAC_BITS;
  if (s->reverse)
  {
    if (s->pos < s->step)
    {
      s->pos += span;
    }
    s->pos -= s->step;
  }
  else
  {
    s->pos += s->step;
    if (s->pos >= span)
    {
      s->pos -= span;
    }
  }
}

/* Reads the loop at pos and, when overdubbing, mixes the input into that slot. */
static inline float play_sample(looper_t *s)
{
  const uint32_t idx = s->pos >> LOOP_FRAC_BITS;
  const uint32_t nxt = idx + 1u == s->len ? 0u : idx + 1u;
  const float fr = (float)(s->pos & ((1u << LOOP_FRAC_BITS) - 1u)) * (1.0f / (float)(1u << LOOP_FRAC_BITS));
  const float a = ulaw_decode(s->mem[idx]);
  const float b = ulaw_decode(s->mem[nxt]);

  if (s->state == FX_LOOP_OVERDUB && idx != s->last_dub)
  {
    s->mem[idx] = ulaw_encode(a * s->dub_fb + s->dec);
    s->last_dub = idx;
  }
  return a + fr * (b - a);
}

static void looper_process(void *state, float *l, float *r, uint32_t n)
{
  looper_t *s = state;

  const uint32_t req = s->pending;
  if (req)
  {
    s->pending = 0;
    apply_command(s, (fx_cmd_t)(req - 1u));
  }

  for (uint32_t i = 0; i < n; i++)
  {
    const float x = 0.5f * (l[i] + r[i]);
    if (decimate(s, x) && s->state == FX_LOOP_RECORDING)
    {
      record_sample(s);
    }

    const bool running = s->state == FX_LOOP_PLAYING || s->state == FX_LOOP_OVERDUB;
    s->play_g = dsp_clampf(s->play_g + (running ? LOOP_FADE_STEP : -LOOP_FADE_STEP), 0.0f, 1.0f);
    const float lvl = dsp_smooth_next(&s->level);

    if ((running || s->play_g > 0.0f) && s->len > 0)
    {
      const float y = play_sample(s) * s->play_g * lvl;
      advance(s);
      l[i] += y;
      r[i] += y;
    }
  }
}

static void looper_status(const void *state, fx_status_t *out)
{
  const looper_t *s = state;
  const uint32_t st = s->state;
  const uint32_t rec = (st == FX_LOOP_RECORDING);

  out->state = st;
  out->len = (rec ? s->wr : s->len) * LOOP_RATE_DIV;
  out->pos = rec ? out->len : (s->pos >> (LOOP_FRAC_BITS - 1u));
  out->max_len = s->cap * LOOP_RATE_DIV;
}

const fx_algo_t g_fx_algo_LOOPER = {
  .id = FX_ALGO_LOOPER,
  .mem_bytes = FX_MEM_REST,
  .min_mem_bytes = LOOP_MIN_MEM,
  .cost = 150,
  .tails = 0,
  .init = looper_init,
  .update = looper_update,
  .process = looper_process,
  .command = looper_command,
  .get_status = looper_status,
};
