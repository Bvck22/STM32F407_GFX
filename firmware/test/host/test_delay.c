/*
 * test_delay.c - Digital, Tape, Mod, Ping-Pong and Reverse delay checks.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"

extern const fx_algo_t g_fx_algo_DIGITAL_DLY, g_fx_algo_TAPE_ECHO, g_fx_algo_MOD_DLY,
                       g_fx_algo_PINGPONG, g_fx_algo_REVERSE_DLY;

/* Output must stay within a bounded range at every parameter extreme. */
#define MAX_PEAK 2.0f
/* Feedback max, 4 s of zeros: the last 0.5 s must be below -46 dBFS rms
 * (0.005). With a 40 ms delay that is ~90 passes of 0.95 (0.95^90 = 0.01), plus
 * the low-pass and limiter losses; reverse uses 100 ms windows at its 0.9 max
 * (0.9^37 = 0.02). A long delay time would fit too few passes to prove decay,
 * so the time is fixed short for this check. */
#define MAX_TAIL 0.005f

typedef struct
{
  const char *name;
  fx_algo_id_t id;
  const fx_algo_t *algo;
  int time_idx, fb_idx, mix_idx;
  float tail_ms;
} case_t;

static const case_t k_cases[] = {
  { "digital",  FX_ALGO_DIGITAL_DLY, &g_fx_algo_DIGITAL_DLY, 0, 1, 4, 40.0f },
  { "tape",     FX_ALGO_TAPE_ECHO,   &g_fx_algo_TAPE_ECHO,   0, 1, 5, 40.0f },
  { "mod",      FX_ALGO_MOD_DLY,     &g_fx_algo_MOD_DLY,     0, 1, 4, 40.0f },
  { "pingpong", FX_ALGO_PINGPONG,    &g_fx_algo_PINGPONG,    0, 1, 4, 40.0f },
  { "reverse",  FX_ALGO_REVERSE_DLY, &g_fx_algo_REVERSE_DLY, 0, 1, 2, 100.0f },
};

typedef struct
{
  const case_t *c;
  float lo[FX_ALGO_MAX_PARAMS], hi[FX_ALGO_MAX_PARAMS];
  uint32_t rng;
  uint32_t sweep_blocks;
} drive_t;

static float frand(uint32_t *s)
{
  *s = *s * 1664525u + 1013904223u;
  return (float)(*s >> 8) * (1.0f / 16777216.0f);
}

static void jump_params(const fx_algo_t *a, void *st, float *p, uint32_t b, void *user)
{
  (void)a; (void)st; (void)b;
  drive_t *d = user;
  for (int i = 0; i < FX_ALGO_MAX_PARAMS; i++)
  {
    p[i] = d->lo[i] + frand(&d->rng) * (d->hi[i] - d->lo[i]);
  }
}

static void sweep_time(const fx_algo_t *a, void *st, float *p, uint32_t b, void *user)
{
  (void)a; (void)st;
  drive_t *d = user;
  const int i = d->c->time_idx;
  const float ph = (float)(b % d->sweep_blocks) / (float)d->sweep_blocks;
  const float tri = ph < 0.5f ? 2.0f * ph : 2.0f - 2.0f * ph;
  p[i] = d->lo[i] + tri * (d->hi[i] - d->lo[i]);
}

static int run(const char *label, const case_t *c, const float *params, float in_s, float tail_s,
               const char *wav, harness_block_fn fn, drive_t *d, float max_tail)
{
  harness_cfg_t cfg = { .algo = c->algo, .params = params, .input_seconds = in_s,
                        .tail_seconds = tail_s, .wav_path = wav, .on_block = fn, .user = d };
  harness_result_t res;
  if (harness_run(&cfg, &res))
  {
    return 1;
  }
  char name[64];
  snprintf(name, sizeof(name), "%s %s", c->name, label);
  return harness_check(name, &res, MAX_PEAK, max_tail);
}

/* ---- Direct drive for functional checks ---------------------------------- */

typedef struct
{
  float *l, *r;
} stereo_t;

static stereo_t run_direct(const fx_algo_t *a, const float *params, const float *in, uint32_t frames)
{
  uint8_t *mem = malloc(a->mem_bytes);
  uint8_t *st = malloc(FX_STATE_BYTES);
  stereo_t o = { calloc(frames, sizeof(float)), calloc(frames, sizeof(float)) };
  memset(mem, 0xA5, a->mem_bytes);
  memset(st, 0xA5, FX_STATE_BYTES);
  a->init(st, mem, a->mem_bytes);
  for (uint32_t b = 0; b < frames; b += FX_BLOCK_FRAMES)
  {
    memcpy(o.l + b, in + b, FX_BLOCK_FRAMES * sizeof(float));
    memcpy(o.r + b, in + b, FX_BLOCK_FRAMES * sizeof(float));
    a->update(st, params);
    a->process(st, o.l + b, o.r + b, FX_BLOCK_FRAMES);
  }
  free(mem);
  free(st);
  return o;
}

static int expect(int cond, const char *what)
{
  printf("%-4s %s\n", cond ? "ok" : "FAIL", what);
  return !cond;
}

static uint32_t argmax_abs(const float *x, uint32_t from, uint32_t to)
{
  uint32_t best = from;
  for (uint32_t i = from; i < to; i++)
  {
    if (fabsf(x[i]) > fabsf(x[best])) best = i;
  }
  return best;
}

static int test_digital_impulse(void)
{
  const uint32_t frames = 256u * 60u;
  float *in = calloc(frames, sizeof(float));
  in[0] = 0.5f;
  float p[FX_ALGO_MAX_PARAMS];
  harness_defaults(FX_ALGO_DIGITAL_DLY, p);
  p[0] = 100.0f; p[1] = 0.5f; p[2] = 1.0f; p[3] = 0.0f; p[4] = 1.0f;
  stereo_t o = run_direct(&g_fx_algo_DIGITAL_DLY, p, in, frames);
  const uint32_t e1 = argmax_abs(o.l, 100, 7000);
  const uint32_t e2 = argmax_abs(o.l, 7000, 12000);
  int f = expect(e1 >= 4798 && e1 <= 4802, "digital: first echo at 100 ms");
  f |= expect(e2 >= 9598 && e2 <= 9604, "digital: second echo at 200 ms");
  f |= expect(fabsf(o.l[e2]) > 0.2f * fabsf(o.l[e1]) && fabsf(o.l[e2]) < 0.7f * fabsf(o.l[e1]),
              "digital: second echo ~ feedback * first");
  free(in); free(o.l); free(o.r);
  return f;
}

static int test_pingpong_alternation(void)
{
  const uint32_t frames = 256u * 60u;
  float *in = calloc(frames, sizeof(float));
  in[0] = 0.5f;
  float p[FX_ALGO_MAX_PARAMS];
  harness_defaults(FX_ALGO_PINGPONG, p);
  p[0] = 100.0f; p[1] = 0.6f; p[2] = 1.0f; p[3] = 1.0f; p[4] = 1.0f;
  stereo_t o = run_direct(&g_fx_algo_PINGPONG, p, in, frames);
  int f = expect(fabsf(o.l[4800]) > 0.1f && fabsf(o.r[4800]) < 1e-3f, "pingpong: first repeat left only");
  f |= expect(fabsf(o.r[9600]) > 0.05f && fabsf(o.l[9600]) < 1e-3f, "pingpong: second repeat right only");
  free(o.l); free(o.r);

  p[3] = 0.0f;
  o = run_direct(&g_fx_algo_PINGPONG, p, in, frames);
  f |= expect(fabsf(o.l[4800] - o.r[4800]) < 1e-4f && fabsf(o.l[4800]) > 0.05f,
              "pingpong: spread 0 gives mono repeats");
  free(in); free(o.l); free(o.r);
  return f;
}

static int test_reverse_playback(void)
{
  const uint32_t win = 4800;
  const uint32_t frames = 256u * 60u;
  float *in = calloc(frames, sizeof(float));
  for (uint32_t i = 0; i < win; i++)
  {
    in[i] = 0.8f * (float)i / (float)win;   /* ramp up */
  }
  float p[FX_ALGO_MAX_PARAMS];
  harness_defaults(FX_ALGO_REVERSE_DLY, p);
  p[0] = 100.0f; p[1] = 0.0f; p[2] = 1.0f;
  stereo_t o = run_direct(&g_fx_algo_REVERSE_DLY, p, in, frames);
  /* Window 2 plays window 1 backwards: a ramp down, starting high. */
  int f = expect(o.l[win + 600] > o.l[win + 2400] && o.l[win + 2400] > o.l[win + 4200],
                 "reverse: window plays backwards");
  f |= expect(fabsf(o.l[win + 2400] - 0.4f) < 0.03f,
              "reverse: mid-window value matches");
  f |= expect(fabsf(o.l[win + 2]) < 0.01f && fabsf(o.l[2 * win - 2]) < 0.01f,
              "reverse: window edges faded");
  free(in); free(o.l); free(o.r);
  return f;
}

int main(void)
{
  int fail = 0;
  for (size_t k = 0; k < sizeof(k_cases) / sizeof(k_cases[0]); k++)
  {
    const case_t *c = &k_cases[k];
    drive_t d = { .c = c, .rng = 12345u + (uint32_t)k, .sweep_blocks = 400 };
    float p[FX_ALGO_MAX_PARAMS];
    char wav[96];

    harness_extremes(c->id, 0, d.lo);
    harness_extremes(c->id, 1, d.hi);

    harness_defaults(c->id, p);
    snprintf(wav, sizeof(wav), "out/%s_default.wav", c->name);
    fail |= run("defaults", c, p, 4.0f, 2.0f, wav, NULL, &d, -1.0f);
    fail |= run("all-min", c, d.lo, 2.0f, 1.0f, NULL, NULL, &d, -1.0f);
    fail |= run("all-max", c, d.hi, 4.0f, 2.0f, NULL, NULL, &d, -1.0f);
    fail |= run("param jumps every block", c, p, 2.0f, 1.0f, NULL, jump_params, &d, -1.0f);
    fail |= run("time sweep", c, p, 8.0f, 1.0f, NULL, sweep_time, &d, -1.0f);

    harness_defaults(c->id, p);
    p[c->time_idx] = c->tail_ms;
    p[c->fb_idx] = d.hi[c->fb_idx];
    p[c->mix_idx] = 1.0f;
    fail |= run("fb max tail decays", c, p, 2.0f, 4.0f, NULL, NULL, &d, MAX_TAIL);
  }

  fail |= test_digital_impulse();
  fail |= test_pingpong_alternation();
  fail |= test_reverse_playback();

  printf(fail ? "test_delay: FAILED\n" : "test_delay: all passed\n");
  return fail ? 1 : 0;
}
