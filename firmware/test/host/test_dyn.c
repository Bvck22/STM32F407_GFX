/* Host test for algo_dyn.c: drive, dynamics, wah, EQ. */
#include <stdio.h>
#include <string.h>

#include "harness.h"

#define NPARAMS FX_ALGO_MAX_PARAMS

extern const fx_algo_t g_fx_algo_OVERDRIVE;
extern const fx_algo_t g_fx_algo_FUZZ;
extern const fx_algo_t g_fx_algo_BITCRUSH;
extern const fx_algo_t g_fx_algo_COMPRESSOR;
extern const fx_algo_t g_fx_algo_AUTOWAH;
extern const fx_algo_t g_fx_algo_EQ;

static int s_fail;

static void run(const fx_algo_t *a, const char *name, const char *tag, const float *p,
                float max_peak, int wav)
{
  char label[64];
  char path[128];
  snprintf(label, sizeof(label), "%s_%s", name, tag);
  snprintf(path, sizeof(path), "out/%s.wav", label);

  const harness_cfg_t cfg = {
    .algo = a,
    .params = p,
    .input_seconds = 3.0f,
    .tail_seconds = 1.0f,
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

/* Standard cases: defaults (WAV), all-min, all-max (own, higher peak limit). */
static void basic(const fx_algo_t *a, const char *name, float max_peak, float max_peak_all_max)
{
  float p[NPARAMS];
  harness_defaults(a->id, p);
  run(a, name, "default", p, max_peak, 1);
  harness_extremes(a->id, 0, p);
  run(a, name, "min", p, max_peak, 0);
  harness_extremes(a->id, 1, p);
  run(a, name, "max", p, max_peak_all_max, 0);
}

static void with(const fx_algo_t *a, const char *name, const char *tag, float max_peak,
                 const float *vals, uint32_t n)
{
  float p[NPARAMS];
  harness_defaults(a->id, p);
  memcpy(p, vals, n * sizeof(float));
  run(a, name, tag, p, max_peak, 0);
}

int main(void)
{
  {
    const fx_algo_t *a = &g_fx_algo_OVERDRIVE;
    basic(a, "overdrive", 4.0f, 4.0f);
    with(a, "overdrive", "clean", 4.0f, (float[]){ 0.1f, 0.8f, 0.5f }, 3);
    with(a, "overdrive", "crunch", 4.0f, (float[]){ 0.6f, 0.3f, 0.7f }, 3);
    with(a, "overdrive", "hi_gain_dark", 4.0f, (float[]){ 1.0f, 0.0f, 1.0f }, 3);
    run_toggle(a, "overdrive", 3, 4.0f);
  }
  {
    const fx_algo_t *a = &g_fx_algo_FUZZ;
    basic(a, "fuzz", 4.0f, 4.0f);
    with(a, "fuzz", "sputter", 4.0f, (float[]){ 0.5f, 0.6f, 1.0f, 0.6f }, 4);
    with(a, "fuzz", "smooth", 4.0f, (float[]){ 0.9f, 0.4f, 0.0f, 0.5f }, 4);
    with(a, "fuzz", "full", 4.0f, (float[]){ 1.0f, 1.0f, 0.5f, 1.0f }, 4);
    run_toggle(a, "fuzz", 4, 4.0f);
  }
  {
    const fx_algo_t *a = &g_fx_algo_BITCRUSH;
    basic(a, "bitcrush", 4.0f, 4.0f);
    with(a, "bitcrush", "lofi", 4.0f, (float[]){ 6.0f, 8.0f, 0.7f }, 3);
    with(a, "bitcrush", "gritty", 4.0f, (float[]){ 3.0f, 2.0f, 1.0f }, 3);
    with(a, "bitcrush", "alias", 4.0f, (float[]){ 16.0f, 32.0f, 1.0f }, 3);
    run_toggle(a, "bitcrush", 3, 4.0f);
  }
  {
    const fx_algo_t *a = &g_fx_algo_COMPRESSOR;
    basic(a, "compressor", 8.0f, 16.0f);
    with(a, "compressor", "squash", 8.0f, (float[]){ -30.0f, 20.0f, 1.0f, 200.0f, 12.0f, 1.0f }, 6);
    with(a, "compressor", "parallel", 8.0f, (float[]){ -25.0f, 8.0f, 5.0f, 100.0f, 6.0f, 0.5f }, 6);
    with(a, "compressor", "limit_makeup", 8.0f, (float[]){ -40.0f, 20.0f, 1.0f, 20.0f, 24.0f, 1.0f }, 6);
    run_toggle(a, "compressor", 6, 16.0f);
  }
  {
    const fx_algo_t *a = &g_fx_algo_AUTOWAH;
    basic(a, "autowah", 4.0f, 4.0f);
    with(a, "autowah", "env_sharp", 4.0f, (float[]){ 0.9f, 1.0f, 8.0f, 0.0f, 1.0f, 1.0f }, 6);
    with(a, "autowah", "lfo_slow", 4.0f, (float[]){ 0.5f, 1.0f, 5.0f, 1.0f, 0.5f, 1.0f }, 6);
    with(a, "autowah", "lfo_fast_q10", 4.0f, (float[]){ 0.5f, 1.0f, 10.0f, 1.0f, 5.0f, 0.7f }, 6);
    run_toggle(a, "autowah", 6, 4.0f);
  }
  {
    const fx_algo_t *a = &g_fx_algo_EQ;
    basic(a, "eq", 8.0f, 32.0f);
    with(a, "eq", "smile", 8.0f, (float[]){ 6.0f, -3.0f, 800.0f, 6.0f, 0.0f }, 5);
    with(a, "eq", "mid_boost", 8.0f, (float[]){ 0.0f, 12.0f, 2500.0f, 0.0f, -3.0f }, 5);
    with(a, "eq", "low_cut_hi_cut", 8.0f, (float[]){ -12.0f, 0.0f, 200.0f, -12.0f, 6.0f }, 5);
    run_toggle(a, "eq", 5, 32.0f);
  }

  puts(s_fail ? "test_dyn: FAILED" : "test_dyn: all passed");
  return s_fail;
}
