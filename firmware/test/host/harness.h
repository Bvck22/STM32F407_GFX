/*
 * harness.h - Host-side (PC) test harness for fx algorithms.
 *
 * Runs one algorithm descriptor over the flash test sample, the same way the
 * chain does on target (256-frame stereo float blocks), and reports stability
 * metrics. Memory and state areas are pre-filled with garbage so a missing
 * clear in init() shows up as noise/NaN.
 */
#ifndef HARNESS_H
#define HARNESS_H

#include <stdint.h>

#include "fx_algo.h"

typedef struct
{
  float peak;          /* max |sample| over the whole run */
  float rms;           /* whole run */
  float tail_rms;      /* last 0.5 s of the zero-input tail */
  int nonfinite;       /* count of NaN/Inf output samples */
  double ns_per_frame; /* host time, only for relative comparison */
} harness_result_t;

/* Called before each block; lets a test drive commands or change params. */
typedef void (*harness_block_fn)(const fx_algo_t *algo, void *state, float *params,
                                 uint32_t block, void *user);

typedef struct
{
  const fx_algo_t *algo;
  const float *params;     /* FX_ALGO_MAX_PARAMS values, table order */
  float input_seconds;     /* sample is looped to this length */
  float tail_seconds;      /* zeros fed afterwards */
  const char *wav_path;    /* NULL = do not write */
  harness_block_fn on_block;
  void *user;
} harness_cfg_t;

void harness_defaults(fx_algo_id_t id, float *params);
/* which: 0 = every param at min, 1 = every param at max. */
void harness_extremes(fx_algo_id_t id, int which, float *params);

int harness_run(const harness_cfg_t *cfg, harness_result_t *res);

/* Prints one line; returns 1 on failure (non-finite output, peak > max_peak,
 * or tail_rms > max_tail_rms when max_tail_rms >= 0). */
int harness_check(const char *label, const harness_result_t *res, float max_peak, float max_tail_rms);

#endif /* HARNESS_H */
