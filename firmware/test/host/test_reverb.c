/* Host test for algo_reverb.c: ROOM, HALL, PLATE, SPRING, SHIMMER. */
#include <stdio.h>
#include <string.h>

#include "harness.h"

#define NPARAMS FX_ALGO_MAX_PARAMS
#define TAIL_SECONDS 6.0f

extern const fx_algo_t g_fx_algo_ROOM;
extern const fx_algo_t g_fx_algo_HALL;
extern const fx_algo_t g_fx_algo_PLATE;
extern const fx_algo_t g_fx_algo_SPRING;
extern const fx_algo_t g_fx_algo_SHIMMER;

/* All five list decay first and mix last (index differs). */
#define P_DECAY 0

typedef struct
{
  const fx_algo_t *algo;
  const char *name;
  uint32_t mix_idx;
  /* Default-decay tail rms over 5.5..6 s after the input stops (about -60 dBFS;
   * the measured values sit near 1e-5, the int16 dither noise floor). */
  float tail_max;
  /* Peak ceiling for the default render and for the stress renders. */
  float peak_default;
  float peak_stress;
} entry_t;

static const entry_t k_entries[] = {
  { &g_fx_algo_ROOM,    "room",    3, 1e-3f, 1.5f, 3.0f },
  { &g_fx_algo_HALL,    "hall",    4, 1e-3f, 1.5f, 3.0f },
  { &g_fx_algo_PLATE,   "plate",   3, 1e-3f, 1.5f, 3.0f },
  { &g_fx_algo_SPRING,  "spring",  3, 1e-3f, 1.5f, 3.0f },
  { &g_fx_algo_SHIMMER, "shimmer", 4, 1e-3f, 1.5f, 3.0f },
};

static int s_fail;

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

static void run(const entry_t *e, const char *tag, const float *p, float input_s, float tail_s,
                float max_peak, float max_tail, int wav, harness_block_fn on_block, void *user)
{
  char label[64];
  char path[128];
  snprintf(label, sizeof(label), "%s_%s", e->name, tag);
  snprintf(path, sizeof(path), "out/reverb_%s.wav", e->name);

  const harness_cfg_t cfg = {
    .algo = e->algo,
    .params = p,
    .input_seconds = input_s,
    .tail_seconds = tail_s,
    .wav_path = wav ? path : NULL,
    .on_block = on_block,
    .user = user,
  };
  harness_result_t res;
  if (harness_run(&cfg, &res))
  {
    s_fail = 1;
    return;
  }
  s_fail |= harness_check(label, &res, max_peak, max_tail);
}

static void check_wet_audible(const entry_t *e, fx_algo_id_t id)
{
  float p[NPARAMS];
  harness_defaults(id, p);
  p[e->mix_idx] = 1.0f;

  /* Tail window = the whole 0.5 s after the input stops. */
  const harness_cfg_t cfg = { .algo = e->algo, .params = p, .input_seconds = 2.0f,
                              .tail_seconds = 0.5f };
  harness_result_t res;
  if (harness_run(&cfg, &res))
  {
    s_fail = 1;
    return;
  }
  const int fail = res.tail_rms < 1e-3f || res.nonfinite > 0;
  printf("%-4s %-36s tail(0-0.5s, mix=1) rms=%8.6f\n", fail ? "FAIL" : "ok", e->name, (double)res.tail_rms);
  s_fail |= fail;
}

int main(void)
{
  for (uint32_t k = 0; k < sizeof(k_entries) / sizeof(k_entries[0]); k++)
  {
    const entry_t *e = &k_entries[k];
    const fx_algo_id_t id = e->algo->id;
    float p[NPARAMS];

    harness_defaults(id, p);
    run(e, "default", p, 3.0f, TAIL_SECONDS, e->peak_default, e->tail_max, 1, NULL, NULL);

    harness_extremes(id, 0, p);
    run(e, "all_min", p, 3.0f, TAIL_SECONDS, e->peak_stress, 1e-3f, 0, NULL, NULL);

    harness_extremes(id, 1, p);
    run(e, "all_max", p, 3.0f, TAIL_SECONDS, e->peak_stress, -1.0f, 0, NULL, NULL);

    /* Decay at maximum, everything else default, mix wet-only: finite and bounded. */
    harness_defaults(id, p);
    p[P_DECAY] = 1.0f;
    p[e->mix_idx] = 1.0f;
    run(e, "decay1_wet", p, 3.0f, TAIL_SECONDS, e->peak_stress, -1.0f, 0, NULL, NULL);

    toggle_t t = { .n = NPARAMS, .rng = 12345u + k };
    harness_extremes(id, 0, t.lo);
    harness_extremes(id, 1, t.hi);
    harness_defaults(id, p);
    run(e, "jumps_2s", p, 2.0f, 0.5f, e->peak_stress, -1.0f, 0, toggle_block, &t);

    check_wet_audible(e, id);
  }

  if (s_fail)
  {
    printf("test_reverb: FAILED\n");
    return 1;
  }
  printf("test_reverb: all passed\n");
  return 0;
}
