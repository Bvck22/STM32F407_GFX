#include "harness.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sample_data.h"

#define REST_BYTES 96000u   /* arena size handed to FX_MEM_REST algorithms */

typedef struct
{
  float min, max, def;
} range_t;

#define H_PARAM(k, mn, mx, df, st, un, lb) { mn, mx, df },
#define H_RANGES(num, sfx, name, cat) static const range_t s_r_##sfx[] = { FX_PARAMS_##sfx(H_PARAM) { 0, 0, 0 } };
FX_ALGO_LIST(H_RANGES)
#undef H_RANGES
#undef H_PARAM

typedef struct
{
  const range_t *r;
  uint32_t n;
} range_list_t;

#define H_ENTRY(num, sfx, name, cat) [num] = { s_r_##sfx, sizeof(s_r_##sfx) / sizeof(range_t) - 1u },
static const range_list_t s_ranges[FX_ALGO_COUNT] = { FX_ALGO_LIST(H_ENTRY) };
#undef H_ENTRY

void harness_defaults(fx_algo_id_t id, float *params)
{
  memset(params, 0, sizeof(float) * FX_ALGO_MAX_PARAMS);
  for (uint32_t i = 0; i < s_ranges[id].n; i++)
  {
    params[i] = s_ranges[id].r[i].def;
  }
}

void harness_extremes(fx_algo_id_t id, int which, float *params)
{
  memset(params, 0, sizeof(float) * FX_ALGO_MAX_PARAMS);
  for (uint32_t i = 0; i < s_ranges[id].n; i++)
  {
    params[i] = which ? s_ranges[id].r[i].max : s_ranges[id].r[i].min;
  }
}

static void put_u32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put_u16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

static void write_wav(const char *path, const float *lr, uint32_t frames)
{
  FILE *f = fopen(path, "wb");
  if (!f)
  {
    fprintf(stderr, "cannot write %s\n", path);
    return;
  }
  const uint32_t data_bytes = frames * 4u;
  fwrite("RIFF", 1, 4, f); put_u32(f, 36u + data_bytes); fwrite("WAVE", 1, 4, f);
  fwrite("fmt ", 1, 4, f); put_u32(f, 16); put_u16(f, 1); put_u16(f, 2);
  put_u32(f, FX_SAMPLE_RATE); put_u32(f, FX_SAMPLE_RATE * 4u); put_u16(f, 4); put_u16(f, 16);
  fwrite("data", 1, 4, f); put_u32(f, data_bytes);
  for (uint32_t i = 0; i < frames * 2u; i++)
  {
    float x = lr[i];
    x = x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
    const int16_t s = (int16_t)lrintf(x * 32767.0f);
    fwrite(&s, 2, 1, f);
  }
  fclose(f);
}

int harness_run(const harness_cfg_t *cfg, harness_result_t *res)
{
  const fx_algo_t *a = cfg->algo;
  const uint32_t in_frames = (uint32_t)(cfg->input_seconds * FX_SAMPLE_RATE);
  const uint32_t tail_frames = (uint32_t)(cfg->tail_seconds * FX_SAMPLE_RATE);
  const uint32_t blocks = (in_frames + tail_frames + FX_BLOCK_FRAMES - 1u) / FX_BLOCK_FRAMES;
  const uint32_t total = blocks * FX_BLOCK_FRAMES;
  const uint32_t tail_window = FX_SAMPLE_RATE / 2u;

  const uint32_t mem_len = (a->mem_bytes == FX_MEM_REST) ? REST_BYTES : a->mem_bytes;
  uint8_t *mem = mem_len ? malloc(mem_len) : NULL;
  /* malloc is 16-byte aligned on 64-bit hosts; mingw lacks aligned_alloc. */
  uint8_t *state = malloc(FX_STATE_BYTES);
  float *out = malloc(sizeof(float) * 2u * total);
  if ((mem_len && !mem) || !state || !out)
  {
    fprintf(stderr, "out of memory\n");
    return 1;
  }
  if (mem) memset(mem, 0xA5, mem_len);
  memset(state, 0xA5, FX_STATE_BYTES);

  float params[FX_ALGO_MAX_PARAMS];
  memcpy(params, cfg->params, sizeof(params));

  a->init(state, mem, mem_len);
  a->update(state, params);

  memset(res, 0, sizeof(*res));
  double sum_sq = 0.0, tail_sq = 0.0;
  uint32_t src = 0;
  const clock_t t0 = clock();

  for (uint32_t b = 0; b < blocks; b++)
  {
    float l[FX_BLOCK_FRAMES], r[FX_BLOCK_FRAMES];
    for (uint32_t i = 0; i < FX_BLOCK_FRAMES; i++)
    {
      const uint32_t n = b * FX_BLOCK_FRAMES + i;
      float x = 0.0f;
      if (n < in_frames)
      {
        x = (float)g_sample_data[src] * (1.0f / 32768.0f);
        src = (src + 1u == g_sample_len) ? 0u : src + 1u;
      }
      l[i] = x;
      r[i] = x;
    }
    if (cfg->on_block)
    {
      cfg->on_block(a, state, params, b, cfg->user);
    }
    a->update(state, params);
    a->process(state, l, r, FX_BLOCK_FRAMES);

    for (uint32_t i = 0; i < FX_BLOCK_FRAMES; i++)
    {
      const uint32_t n = b * FX_BLOCK_FRAMES + i;
      out[2u * n] = l[i];
      out[2u * n + 1u] = r[i];
      for (int c = 0; c < 2; c++)
      {
        const float v = c ? r[i] : l[i];
        if (!isfinite(v))
        {
          res->nonfinite++;
          continue;
        }
        const float m = fabsf(v);
        if (m > res->peak) res->peak = m;
        const double vd = (double)v;
        sum_sq += vd * vd;
        if (n >= total - tail_window) tail_sq += vd * vd;
      }
    }
  }

  const double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
  res->ns_per_frame = secs * 1e9 / total;
  res->rms = (float)sqrt(sum_sq / (2.0 * total));
  res->tail_rms = (float)sqrt(tail_sq / (2.0 * tail_window));

  if (cfg->wav_path)
  {
    write_wav(cfg->wav_path, out, total);
  }
  free(mem);
  free(state);
  free(out);
  return 0;
}

int harness_check(const char *label, const harness_result_t *res, float max_peak, float max_tail_rms)
{
  const int fail = res->nonfinite > 0 || res->peak > max_peak ||
                   (max_tail_rms >= 0.0f && res->tail_rms > max_tail_rms);
  printf("%-4s %-36s peak=%6.3f rms=%6.4f tail=%8.6f nonfinite=%d host=%.1f ns/frame\n",
         fail ? "FAIL" : "ok", label, (double)res->peak, (double)res->rms, (double)res->tail_rms, res->nonfinite,
         res->ns_per_frame);
  return fail;
}
