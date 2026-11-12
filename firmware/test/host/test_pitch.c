/*
 * test_pitch.c - Octaver, Harmonizer and Detune checks.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"

extern const fx_algo_t g_fx_algo_OCTAVER, g_fx_algo_HARMONIZER, g_fx_algo_DETUNE;

/* Voices are bounded by dry + a few unity-gain voices. */
#define MAX_PEAK 4.0f
/* Pitch effects hold nothing but a 40-90 ms history: once the input stops the
 * output must be digital silence for practical purposes (-80 dBFS). */
#define MAX_TAIL 1e-4f

typedef struct
{
  const char *name;
  fx_algo_id_t id;
  const fx_algo_t *algo;
} case_t;

static const case_t k_cases[] = {
  { "octaver",    FX_ALGO_OCTAVER,    &g_fx_algo_OCTAVER },
  { "harmonizer", FX_ALGO_HARMONIZER, &g_fx_algo_HARMONIZER },
  { "detune",     FX_ALGO_DETUNE,     &g_fx_algo_DETUNE },
};

typedef struct
{
  float lo[FX_ALGO_MAX_PARAMS], hi[FX_ALGO_MAX_PARAMS];
  uint32_t rng;
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

/* ---- Pitch accuracy on a pure tone --------------------------------------- */

#define TONE_HZ     200.0f
#define TONE_FRAMES (FX_BLOCK_FRAMES * 560u)   /* ~3 s, whole blocks */

/* Magnitude of the DFT of x at hz. */
static double dft_mag(const float *x, uint32_t n, double hz)
{
  const double w = 6.283185307179586 * hz / (double)FX_SAMPLE_RATE;
  double re = 0.0, im = 0.0;
  for (uint32_t i = 0; i < n; i++)
  {
    re += (double)x[i] * cos(w * (double)i);
    im += (double)x[i] * sin(w * (double)i);
  }
  return sqrt(re * re + im * im);
}

/* Runs a sine through the algorithm and returns the strongest frequency
 * (1 Hz resolution, 0.2x .. 2.6x the tone) in the last two seconds of the left
 * channel. A spectral peak is used because grain crossfades add or remove
 * zero crossings without moving the pitch. */
static float measure_hz(const fx_algo_t *a, const float *params)
{
  uint8_t *mem = malloc(a->mem_bytes);
  uint8_t *st = malloc(FX_STATE_BYTES);
  float *out = malloc(TONE_FRAMES * sizeof(float));
  float l[FX_BLOCK_FRAMES], r[FX_BLOCK_FRAMES];
  memset(mem, 0xA5, a->mem_bytes);
  memset(st, 0xA5, FX_STATE_BYTES);
  a->init(st, mem, a->mem_bytes);

  double phase = 0.0;
  for (uint32_t n = 0; n < TONE_FRAMES; n += FX_BLOCK_FRAMES)
  {
    for (uint32_t i = 0; i < FX_BLOCK_FRAMES; i++)
    {
      l[i] = r[i] = 0.5f * (float)sin(phase);
      phase += 6.283185307179586 * (double)TONE_HZ / (double)FX_SAMPLE_RATE;
    }
    a->update(st, params);
    a->process(st, l, r, FX_BLOCK_FRAMES);
    memcpy(out + n, l, sizeof(l));
  }

  const float *tail = out + FX_SAMPLE_RATE;
  const uint32_t n = TONE_FRAMES - FX_SAMPLE_RATE;
  double best = 0.0, best_hz = 0.0;
  for (double hz = 0.2 * (double)TONE_HZ; hz <= 2.6 * (double)TONE_HZ; hz += 1.0)
  {
    const double m = dft_mag(tail, n, hz);
    if (m > best)
    {
      best = m;
      best_hz = hz;
    }
  }
  free(mem);
  free(st);
  free(out);
  return (float)best_hz;
}

static int expect_hz(const char *what, const fx_algo_t *a, const float *p, float ratio)
{
  const float hz = measure_hz(a, p);
  const float want = TONE_HZ * ratio;
  const int fail = fabsf(hz - want) > 0.02f * want;
  printf("%-4s %-36s measured %.1f Hz, expected %.1f Hz\n", fail ? "FAIL" : "ok", what,
         (double)hz, (double)want);
  return fail;
}

static int test_pitch_ratios(void)
{
  float p[FX_ALGO_MAX_PARAMS];
  int f = 0;

  harness_defaults(FX_ALGO_HARMONIZER, p);
  p[0] = 12.0f; p[1] = 7.0f; p[2] = 1.0f; p[3] = 0.0f; p[4] = 0.0f;
  f |= expect_hz("harmonizer voice1 +12 st", &g_fx_algo_HARMONIZER, p, 2.0f);
  p[0] = -12.0f;
  f |= expect_hz("harmonizer voice1 -12 st", &g_fx_algo_HARMONIZER, p, 0.5f);
  p[0] = 5.0f;
  f |= expect_hz("harmonizer voice1 +5 st", &g_fx_algo_HARMONIZER, p, 1.3348f);

  harness_defaults(FX_ALGO_OCTAVER, p);
  p[0] = 1.0f; p[1] = 0.0f; p[2] = 0.0f; p[3] = 1.0f; p[4] = 0.0f;
  f |= expect_hz("octaver sub1", &g_fx_algo_OCTAVER, p, 0.5f);
  p[0] = 0.0f; p[1] = 1.0f;
  f |= expect_hz("octaver sub2", &g_fx_algo_OCTAVER, p, 0.25f);
  p[1] = 0.0f; p[2] = 1.0f;
  f |= expect_hz("octaver up", &g_fx_algo_OCTAVER, p, 2.0f);

  harness_defaults(FX_ALGO_DETUNE, p);
  p[0] = 50.0f; p[1] = 0.0f; p[2] = 1.0f;
  f |= expect_hz("detune left +50 cents", &g_fx_algo_DETUNE, p, 1.0293f);
  return f;
}

int main(void)
{
  int fail = 0;
  for (size_t k = 0; k < sizeof(k_cases) / sizeof(k_cases[0]); k++)
  {
    const case_t *c = &k_cases[k];
    drive_t d = { .rng = 777u + (uint32_t)k };
    float p[FX_ALGO_MAX_PARAMS];
    char wav[96];

    harness_extremes(c->id, 0, d.lo);
    harness_extremes(c->id, 1, d.hi);

    harness_defaults(c->id, p);
    snprintf(wav, sizeof(wav), "out/%s_default.wav", c->name);
    fail |= run("defaults", c, p, 4.0f, 1.0f, wav, NULL, &d, MAX_TAIL);
    fail |= run("all-min", c, d.lo, 2.0f, 1.0f, NULL, NULL, &d, MAX_TAIL);
    fail |= run("all-max", c, d.hi, 3.0f, 1.0f, NULL, NULL, &d, MAX_TAIL);
    fail |= run("param jumps every block", c, p, 2.0f, 1.0f, NULL, jump_params, &d, MAX_TAIL);
  }

  fail |= test_pitch_ratios();

  printf(fail ? "test_pitch: FAILED\n" : "test_pitch: all passed\n");
  return fail ? 1 : 0;
}
