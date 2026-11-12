/* Host test for algo_mod.c: modulation effects. */
#include <stdio.h>
#include <string.h>

#include "harness.h"

#define NPARAMS FX_ALGO_MAX_PARAMS

extern const fx_algo_t g_fx_algo_CHORUS;
extern const fx_algo_t g_fx_algo_VIBRATO;
extern const fx_algo_t g_fx_algo_FLANGER;
extern const fx_algo_t g_fx_algo_PHASER;
extern const fx_algo_t g_fx_algo_TREMOLO;
extern const fx_algo_t g_fx_algo_ROTARY;
extern const fx_algo_t g_fx_algo_RINGMOD;

static int s_fail;

static void run(const fx_algo_t *a, const char *name, const char *tag, const float *p,
                float max_peak, int wav, float tail_s)
{
  char label[64];
  char path[128];
  snprintf(label, sizeof(label), "%s_%s", name, tag);
  snprintf(path, sizeof(path), "out/%s.wav", label);

  const harness_cfg_t cfg = {
    .algo = a,
    .params = p,
    .input_seconds = 3.0f,
    .tail_seconds = tail_s,
    .wav_path = wav ? path : NULL,
  };
  harness_result_t res;
  if (harness_run(&cfg, &res))
  {
    s_fail = 1;
    return;
  }
  s_fail |= harness_check(label, &res, max_peak, 1e-3f);
}

typedef struct
{
  float lo[NPARAMS];
  float hi[NPARAMS];
  uint32_t n;
  uint32_t rng;
} toggle_t;

static void toggle_block(const fx_algo_t *algo, void *state, float *params, uint32_t block,
                         void *user)
{
  (void)algo;
  (void)state;
  (void)block;
  toggle_t *t = user;
  for (uint32_t i = 0; i < t->n; i++)
  {
    t->rng = t->rng * 1664525u + 1013904223u;
    params[i] = (t->rng >> 31) ? t->hi[i] : t->lo[i];
  }
}

static void run_toggle(const fx_algo_t *a, const char *name, uint32_t nparams, float max_peak)
{
  toggle_t t = { .n = nparams, .rng = 12345u };
  harness_extremes(a->id, 0, t.lo);
  harness_extremes(a->id, 1, t.hi);
  float p[NPARAMS];
  harness_defaults(a->id, p);

  char label[64];
  snprintf(label, sizeof(label), "%s_toggle", name);
  const harness_cfg_t cfg = {
    .algo = a,
    .params = p,
    .input_seconds = 2.0f,
    .tail_seconds = 0.5f,
    .on_block = toggle_block,
    .user = &t,
  };
  harness_result_t res;
  if (harness_run(&cfg, &res))
  {
    s_fail = 1;
    return;
  }
  s_fail |= harness_check(label, &res, max_peak, -1.0f);
}

/* Standard cases: defaults (WAV), all-min, all-max. */
static void basic(const fx_algo_t *a, const char *name, float max_peak)
{
  float p[NPARAMS];
  harness_defaults(a->id, p);
  run(a, name, "default", p, max_peak, 1, 1.0f);
  harness_extremes(a->id, 0, p);
  run(a, name, "min", p, max_peak, 0, 1.0f);
  harness_extremes(a->id, 1, p);
  run(a, name, "max", p, max_peak, 0, 1.0f);
}

/* Defaults overridden by the first n values; tail_s is the zero-input tail length. */
static void with_tail(const fx_algo_t *a, const char *name, const char *tag, float max_peak,
                      const float *vals, uint32_t n, float tail_s)
{
  float p[NPARAMS];
  harness_defaults(a->id, p);
  memcpy(p, vals, n * sizeof(float));
  run(a, name, tag, p, max_peak, 0, tail_s);
}


static void with(const fx_algo_t *a, const char *name, const char *tag, float max_peak,
                 const float *vals, uint32_t n)
{
  with_tail(a, name, tag, max_peak, vals, n, 1.0f);
}

int main(void)
{
  {
    const fx_algo_t *a = &g_fx_algo_CHORUS;
    basic(a, "chorus", 4.0f);
    with(a, "chorus", "slow_wide", 4.0f, (float[]){ 0.3f, 0.6f, 1.0f, 0.5f }, 4);
    with(a, "chorus", "deep_mono", 4.0f, (float[]){ 2.0f, 1.0f, 0.0f, 1.0f }, 4);
    run_toggle(a, "chorus", 4, 4.0f);
  }
  {
    const fx_algo_t *a = &g_fx_algo_VIBRATO;
    basic(a, "vibrato", 4.0f);
    with(a, "vibrato", "slow_deep", 4.0f, (float[]){ 0.5f, 1.0f }, 2);
    with(a, "vibrato", "fast_shallow", 4.0f, (float[]){ 10.0f, 0.2f }, 2);
    run_toggle(a, "vibrato", 2, 4.0f);
  }
  {
    const fx_algo_t *a = &g_fx_algo_FLANGER;
    basic(a, "flanger", 4.0f);
    with(a, "flanger", "jet", 4.0f, (float[]){ 0.2f, 1.0f, 0.2f, 0.9f, 0.5f }, 5);
    with(a, "flanger", "negative_fb", 4.0f, (float[]){ 0.5f, 0.8f, 0.5f, -0.9f, 0.5f }, 5);
    /* Feedback 0.95 rings for about a second, so the tail needs more time to fall under the limit. */
    with_tail(a, "flanger", "max_fb_slow", 4.0f, (float[]){ 0.05f, 1.0f, 1.0f, 0.95f, 1.0f }, 5, 3.0f);
    run_toggle(a, "flanger", 5, 4.0f);
  }
  {
    const fx_algo_t *a = &g_fx_algo_PHASER;
    basic(a, "phaser", 4.0f);
    with(a, "phaser", "two_stage", 4.0f, (float[]){ 0.4f, 1.0f, 0.0f, 2.0f, 0.5f }, 5);
    with(a, "phaser", "eight_fb", 4.0f, (float[]){ 0.3f, 1.0f, 0.9f, 8.0f, 0.5f }, 5);
    with(a, "phaser", "fast_wet", 4.0f, (float[]){ 5.0f, 0.5f, 0.6f, 6.0f, 1.0f }, 5);
    run_toggle(a, "phaser", 5, 4.0f);
  }
  {
    const fx_algo_t *a = &g_fx_algo_TREMOLO;
    basic(a, "tremolo", 4.0f);
    with(a, "tremolo", "square", 4.0f, (float[]){ 6.0f, 1.0f, 2.0f, 0.0f }, 4);
    with(a, "tremolo", "pan_sine", 4.0f, (float[]){ 3.0f, 1.0f, 0.0f, 1.0f }, 4);
    with(a, "tremolo", "pan_tri_fast", 4.0f, (float[]){ 15.0f, 0.8f, 1.0f, 1.0f }, 4);
    run_toggle(a, "tremolo", 4, 4.0f);
  }
  {
    const fx_algo_t *a = &g_fx_algo_ROTARY;
    basic(a, "rotary", 4.0f);
    with(a, "rotary", "fast_slow_accel", 4.0f, (float[]){ 1.0f, 1.0f, 0.2f, 1.0f }, 4);
    with(a, "rotary", "fast_snappy_drive", 4.0f, (float[]){ 1.0f, 0.0f, 1.0f, 1.0f }, 4);
    with(a, "rotary", "slow_half_mix", 4.0f, (float[]){ 0.0f, 0.5f, 0.5f, 0.5f }, 4);
    run_toggle(a, "rotary", 4, 4.0f);
  }
  {
    const fx_algo_t *a = &g_fx_algo_RINGMOD;
    basic(a, "ringmod", 4.0f);
    with(a, "ringmod", "bell", 4.0f, (float[]){ 440.0f, 0.0f, 0.0f, 1.0f }, 4);
    with(a, "ringmod", "lfo_sweep", 4.0f, (float[]){ 1000.0f, 5.0f, 1.0f, 0.7f }, 4);
    with(a, "ringmod", "tremolo_range", 4.0f, (float[]){ 20.0f, 10.0f, 1.0f, 1.0f }, 4);
    run_toggle(a, "ringmod", 4, 4.0f);
  }

  puts(s_fail ? "test_mod: FAILED" : "test_mod: all passed");
  return s_fail;
}
