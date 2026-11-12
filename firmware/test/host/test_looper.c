/*
 * test_looper.c - Looper state machine, capacity, playback modes and stability.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"

extern const fx_algo_t g_fx_algo_LOOPER;

#define SR 48000u

/* Input plus a clamped loop, each at most 1.0. */
#define MAX_PEAK 2.0f

static int expect(int cond, const char *what)
{
  printf("%-4s %s\n", cond ? "ok" : "FAIL", what);
  return !cond;
}

static fx_status_t status(const fx_algo_t *a, void *st)
{
  fx_status_t s;
  a->get_status(st, &s);
  return s;
}

/* ---- Harness-driven checks ----------------------------------------------- */

typedef struct
{
  int fail;
  uint32_t last_pos;
  uint32_t rng;
  float lo[FX_ALGO_MAX_PARAMS], hi[FX_ALGO_MAX_PARAMS];
} script_t;

static float frand(uint32_t *s)
{
  *s = *s * 1664525u + 1013904223u;
  return (float)(*s >> 8) * (1.0f / 16777216.0f);
}

/* REC at block 0, close after ~2 s, then DUB, STOP, PLAY, CLEAR. Each state is
 * checked on the block after the command (commands apply at block start). */
static void command_script(const fx_algo_t *a, void *st, float *p, uint32_t b, void *user)
{
  (void)p;
  script_t *sc = user;
  const fx_status_t s = status(a, st);

  switch (b)
  {
    case 0:
      sc->fail |= expect(s.state == FX_LOOP_EMPTY && s.len == 0, "script: starts empty");
      sc->fail |= expect(s.max_len == 2u * 96000u, "script: max_len = 4 s in 48 kHz units");
      a->command(st, FX_CMD_REC);
      break;
    case 1:
      sc->fail |= expect(s.state == FX_LOOP_RECORDING, "script: REC -> recording");
      break;
    case 375:
      sc->fail |= expect(s.state == FX_LOOP_RECORDING && s.len >= 94000u && s.len <= 98000u &&
                         s.pos == s.len, "script: ~2 s recorded, len grows");
      a->command(st, FX_CMD_REC);
      break;
    case 376:
      sc->fail |= expect(s.state == FX_LOOP_PLAYING && s.len >= 94000u && s.len <= 98000u,
                         "script: REC again -> playing, length fixed");
      break;
    case 420:
      sc->fail |= expect(s.state == FX_LOOP_PLAYING && s.pos < s.len && s.pos != sc->last_pos,
                         "script: playing, position moves");
      a->command(st, FX_CMD_DUB);
      break;
    case 421:
      sc->fail |= expect(s.state == FX_LOOP_OVERDUB, "script: DUB -> overdub");
      break;
    case 440:
      a->command(st, FX_CMD_STOP);
      break;
    case 441:
      sc->fail |= expect(s.state == FX_LOOP_STOPPED && s.len >= 94000u, "script: STOP -> stopped, loop kept");
      break;
    case 450:
      a->command(st, FX_CMD_PLAY);
      break;
    case 451:
      sc->fail |= expect(s.state == FX_LOOP_PLAYING && s.pos < 2000u, "script: PLAY -> playing from start");
      break;
    case 470:
      a->command(st, FX_CMD_CLEAR);
      break;
    case 471:
      sc->fail |= expect(s.state == FX_LOOP_EMPTY && s.len == 0 && s.pos == 0, "script: CLEAR -> empty");
      break;
    default:
      break;
  }
  sc->last_pos = s.pos;
}

/* Random params and random commands every few blocks. */
static void chaos(const fx_algo_t *a, void *st, float *p, uint32_t b, void *user)
{
  script_t *sc = user;
  for (int i = 0; i < FX_ALGO_MAX_PARAMS; i++)
  {
    p[i] = sc->lo[i] + frand(&sc->rng) * (sc->hi[i] - sc->lo[i]);
  }
  if (b % 7u == 0u)
  {
    a->command(st, (fx_cmd_t)(frand(&sc->rng) * (float)FX_CMD_COUNT));
  }
}

static void start_rec_close(const fx_algo_t *a, void *st, float *p, uint32_t b, void *user)
{
  (void)p; (void)user;
  if (b == 0) a->command(st, FX_CMD_REC);
  if (b == 100) a->command(st, FX_CMD_REC);
  if (b == 200) a->command(st, FX_CMD_DUB);
}

static int run_harness(const char *label, const float *params, float seconds, harness_block_fn fn,
                       void *user, const char *wav)
{
  harness_cfg_t cfg = { .algo = &g_fx_algo_LOOPER, .params = params, .input_seconds = seconds,
                        .tail_seconds = 0.0f, .wav_path = wav, .on_block = fn, .user = user };
  harness_result_t res;
  if (harness_run(&cfg, &res))
  {
    return 1;
  }
  char name[64];
  snprintf(name, sizeof(name), "looper %s", label);
  return harness_check(name, &res, MAX_PEAK, -1.0f);
}

/* ---- Direct drive -------------------------------------------------------- */

typedef struct
{
  const fx_algo_t *a;
  void *st;
  uint8_t *mem;
  float p[FX_ALGO_MAX_PARAMS];
} rig_t;

static void rig_init(rig_t *r, uint32_t mem_len)
{
  r->a = &g_fx_algo_LOOPER;
  r->mem = malloc(mem_len);
  r->st = malloc(FX_STATE_BYTES);
  memset(r->mem, 0xA5, mem_len);
  memset(r->st, 0xA5, FX_STATE_BYTES);
  r->a->init(r->st, r->mem, mem_len);
  harness_defaults(FX_ALGO_LOOPER, r->p);
}

static void rig_free(rig_t *r)
{
  free(r->mem);
  free(r->st);
}

/* Runs frames (multiple of the block size) of mono input; returns left output. */
static void rig_run(rig_t *r, const float *in, float *out, uint32_t frames)
{
  float l[FX_BLOCK_FRAMES], rr[FX_BLOCK_FRAMES];
  for (uint32_t b = 0; b < frames; b += FX_BLOCK_FRAMES)
  {
    for (uint32_t i = 0; i < FX_BLOCK_FRAMES; i++)
    {
      l[i] = rr[i] = in ? in[b + i] : 0.0f;
    }
    r->a->update(r->st, r->p);
    r->a->process(r->st, l, rr, FX_BLOCK_FRAMES);
    memcpy(out + b, l, sizeof(l));
  }
}

static int test_capacity_autoclose(void)
{
  rig_t r;
  rig_init(&r, 24000u);   /* minimum memory: 1 s */
  const uint32_t frames = FX_BLOCK_FRAMES * 380u;   /* ~2 s of input */
  float *in = malloc(frames * sizeof(float));
  float *out = malloc(frames * sizeof(float));
  for (uint32_t i = 0; i < frames; i++) in[i] = 0.4f * sinf(0.05f * (float)i);

  r.a->command(r.st, FX_CMD_REC);
  rig_run(&r, in, out, frames);
  const fx_status_t s = status(r.a, r.st);
  int f = expect(s.max_len == 48000u, "capacity: max_len = 1 s for 24000 bytes");
  f |= expect(s.state == FX_LOOP_PLAYING, "capacity: full recording auto-closes to playing");
  f |= expect(s.len == 48000u, "capacity: loop length = capacity");
  f |= expect(s.pos < s.len, "capacity: position stays inside the loop");
  free(in); free(out); rig_free(&r);
  return f;
}

#define LOOP_FRAMES (FX_BLOCK_FRAMES * 188u)   /* ~1.003 s */

/* Records `rec` for LOOP_FRAMES, closes, then feeds zeros for `play_frames`. */
static float *record_and_play(rig_t *r, const float *rec, uint32_t play_frames)
{
  float *scratch = malloc(LOOP_FRAMES * sizeof(float));
  float *out = malloc(play_frames * sizeof(float));
  r->a->command(r->st, FX_CMD_REC);
  rig_run(r, rec, scratch, LOOP_FRAMES);
  r->a->command(r->st, FX_CMD_REC);
  rig_run(r, NULL, out, play_frames);
  free(scratch);
  return out;
}

static float max_step(const float *x, uint32_t from, uint32_t to)
{
  float m = 0.0f;
  for (uint32_t i = from + 1; i < to; i++)
  {
    const float d = fabsf(x[i] - x[i - 1]);
    if (d > m) m = d;
  }
  return m;
}

static float rms(const float *x, uint32_t from, uint32_t to)
{
  double s = 0.0;
  for (uint32_t i = from; i < to; i++) s += (double)x[i] * (double)x[i];
  return (float)sqrt(s / (double)(to - from));
}

static int test_playback_quality(void)
{
  rig_t r;
  rig_init(&r, 96000u);
  float *in = malloc(LOOP_FRAMES * sizeof(float));
  for (uint32_t i = 0; i < LOOP_FRAMES; i++) in[i] = 0.5f * sinf(6.2831853f * 440.0f * (float)i / (float)SR);

  const uint32_t play = FX_BLOCK_FRAMES * 565u;   /* ~3 loops */
  float *out = record_and_play(&r, in, play);
  const uint32_t len = status(r.a, r.st).len;

  /* Sine slope is 0.5 * 2pi * 440 / 48000 = 0.029 per sample; a click at the
   * loop wrap or a mu-law glitch would exceed 3x that. Skip the 5 ms start fade. */
  const float step = max_step(out, 400, play);
  int f = expect(step < 0.09f, "playback: no clicks incl. loop wraps (max step < 0.09)");
  const float lvl = rms(out, len + 2000, len + 40000);
  f |= expect(lvl > 0.30f && lvl < 0.40f, "playback: level matches the recording (rms 0.354 +-)");
  printf("     max step %.4f, rms %.4f, len %u\n", (double)step, (double)lvl, (unsigned)len);

  /* Loop repeats itself: second pass equals third pass sample for sample. */
  double err = 0.0;
  for (uint32_t i = 2000; i < 20000; i++)
  {
    err += (double)(out[len + i] - out[2u * len + i]) * (double)(out[len + i] - out[2u * len + i]);
  }
  f |= expect(sqrt(err / 18000.0) < 0.02, "playback: loop repeats consistently");

  /* Silent when stopped, with a fade (no click). */
  float *z = malloc(FX_BLOCK_FRAMES * 20u * sizeof(float));
  r.a->command(r.st, FX_CMD_STOP);
  rig_run(&r, NULL, z, FX_BLOCK_FRAMES * 20u);
  f |= expect(max_step(z, 0, FX_BLOCK_FRAMES * 20u) < 0.09f && fabsf(z[FX_BLOCK_FRAMES * 20u - 1]) == 0.0f,
              "stop: fades out to silence without a click");

  free(z); free(in); free(out); rig_free(&r);
  return f;
}

static int test_modes(void)
{
  rig_t r;
  float *ramp = malloc(LOOP_FRAMES * sizeof(float));
  for (uint32_t i = 0; i < LOOP_FRAMES; i++) ramp[i] = 0.8f * (float)i / (float)LOOP_FRAMES;
  const uint32_t play = FX_BLOCK_FRAMES * 120u;
  const uint32_t mid = 8000;   /* well after the start fade, before wrap */
  int f = 0;

  rig_init(&r, 96000u);
  float *out = record_and_play(&r, ramp, play);
  const float fwd = out[mid + 9600] - out[mid];
  f |= expect(fwd > 0.0f, "mode: forward plays the ramp upward");
  free(out); rig_free(&r);

  rig_init(&r, 96000u);
  r.p[2] = 1.0f;
  out = record_and_play(&r, ramp, play);
  const float rev = out[mid + 9600] - out[mid];
  f |= expect(rev < 0.0f && fabsf(rev + fwd) < 0.25f * fwd, "mode: reverse plays the ramp downward at the same rate");
  free(out); rig_free(&r);

  rig_init(&r, 96000u);
  r.p[3] = 1.0f;
  out = record_and_play(&r, ramp, play);
  const float half = out[mid + 9600] - out[mid];
  f |= expect(fabsf(half - 0.5f * fwd) < 0.15f * fwd, "mode: half speed halves the slope");
  const fx_status_t s = status(r.a, r.st);
  f |= expect(s.pos < s.len, "mode: half speed position inside loop");
  free(out); rig_free(&r);

  free(ramp);
  return f;
}

static int test_overdub_scaling(void)
{
  rig_t r;
  rig_init(&r, 96000u);
  r.p[1] = 0.5f;   /* dub_fb */
  float *in = malloc(LOOP_FRAMES * sizeof(float));
  for (uint32_t i = 0; i < LOOP_FRAMES; i++) in[i] = 0.5f * sinf(6.2831853f * 440.0f * (float)i / (float)SR);

  float *out = record_and_play(&r, in, FX_BLOCK_FRAMES * 20u);
  const fx_status_t s0 = status(r.a, r.st);
  free(out);

  /* One overdub pass over silence only scales the old content by dub_fb. */
  const uint32_t pass = (s0.len / FX_BLOCK_FRAMES + 2u) * FX_BLOCK_FRAMES;
  float *o2 = malloc(pass * 2u * sizeof(float));
  r.a->command(r.st, FX_CMD_DUB);
  rig_run(&r, NULL, o2, pass);
  r.a->command(r.st, FX_CMD_DUB);   /* back to playing */
  rig_run(&r, NULL, o2 + pass, pass);
  const float before = rms(o2, 2000, 20000);
  const float after = rms(o2 + pass, 2000, 20000);
  int f = expect(after > 0.35f * before && after < 0.65f * before, "overdub: old content scaled by dub_fb (0.5)");
  printf("     rms before %.4f after %.4f\n", (double)before, (double)after);
  free(o2); free(in); rig_free(&r);
  return f;
}

int main(void)
{
  int fail = 0;
  script_t sc = { .rng = 4242u };
  float p[FX_ALGO_MAX_PARAMS];
  harness_extremes(FX_ALGO_LOOPER, 0, sc.lo);
  harness_extremes(FX_ALGO_LOOPER, 1, sc.hi);
  harness_defaults(FX_ALGO_LOOPER, p);

  fail |= run_harness("command script", p, 472.0f * 256.0f / (float)SR, command_script, &sc, "out/looper_default.wav");
  fail |= sc.fail;
  fail |= run_harness("defaults", p, 5.0f, start_rec_close, &sc, NULL);
  fail |= run_harness("all-min", sc.lo, 3.0f, start_rec_close, &sc, NULL);
  fail |= run_harness("all-max", sc.hi, 3.0f, start_rec_close, &sc, NULL);
  fail |= run_harness("random params + commands", p, 8.0f, chaos, &sc, NULL);

  fail |= test_capacity_autoclose();
  fail |= test_playback_quality();
  fail |= test_modes();
  fail |= test_overdub_scaling();

  printf(fail ? "test_looper: FAILED\n" : "test_looper: all passed\n");
  return fail ? 1 : 0;
}
