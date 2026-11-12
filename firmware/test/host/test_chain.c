/*
 * test_chain.c - Host tests for the chain core (fx_engine.c) with the real
 * algorithms. fx_process() is driven from the engine's wait hook, the way the
 * audio ISR consumes structural requests on target.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fx_engine.h"
#include "sample_data.h"

/* Engine diagnostics (fx_engine.c, not part of the public header). */
extern void (*fx_engine_wait_hook)(void);
extern volatile uint32_t fx_engine_nan_count;
const void *fx_engine_slot_mem(uint32_t slot, uint32_t *len);
const void *fx_engine_arena(uint32_t arena, uint32_t *len);

#define BLOCK FX_BLOCK_FRAMES

static int g_fail;
static int g_checks;

#define CHECK(cond, ...)                                  \
  do                                                      \
  {                                                       \
    g_checks++;                                           \
    if (!(cond))                                          \
    {                                                     \
      g_fail++;                                           \
      printf("FAIL %s:%d  %s\n     ", __FILE__, __LINE__, #cond); \
      printf(__VA_ARGS__);                                \
      printf("\n");                                       \
    }                                                     \
  } while (0)

/* ---- audio driving ------------------------------------------------------ */

static uint32_t s_src;
static int s_silence;
static int16_t s_in[BLOCK];
static int16_t s_out[2 * BLOCK];

typedef struct
{
  int peak;
  uint32_t saturated;
  int max_step;
  int in_step;   /* largest input sample-to-sample step */
  double energy;
} stats_t;

static stats_t s_stats;

static void stats_reset(void)
{
  memset(&s_stats, 0, sizeof(s_stats));
}

static void run_block(void)
{
  for (uint32_t i = 0; i < BLOCK; i++)
  {
    s_in[i] = s_silence ? 0 : g_sample_data[s_src];
    s_src = (s_src + 1u == g_sample_len) ? 0u : s_src + 1u;
  }
  fx_process(s_in, s_out, BLOCK);
  static int prev;
  static int in_prev;
  for (uint32_t i = 0; i < BLOCK; i++)
  {
    const int v = s_out[2 * i];
    const int a = abs(v);
    if (a > s_stats.peak) s_stats.peak = a;
    if (v >= 32767 || v <= -32768) s_stats.saturated++;
    const int d = abs(v - prev);
    if (d > s_stats.max_step) s_stats.max_step = d;
    prev = v;
    const int di = abs(s_in[i] - in_prev);
    if (di > s_stats.in_step) s_stats.in_step = di;
    in_prev = s_in[i];
    s_stats.energy += (double)v * v / (32768.0 * 32768.0);
  }
}

static void run_blocks(uint32_t n)
{
  while (n--) run_block();
}

static void pump(void)
{
  run_block();
}

static void engine_start(void)
{
  fx_engine_wait_hook = NULL;
  fx_init();
  s_src = 0;
  s_silence = 0;
  stats_reset();
}

/* ---- helpers ------------------------------------------------------------ */

static void set_chain(fx_algo_id_t a, fx_algo_id_t b, fx_algo_id_t c, fx_algo_id_t d, fx_chain_cfg_t *cfg)
{
  const fx_algo_id_t ids[FX_SLOTS] = { a, b, c, d };
  fx_get_config(cfg);
  for (uint32_t j = 0; j < FX_SLOTS; j++)
  {
    memset(&cfg->slots[j], 0, sizeof(cfg->slots[j]));
    cfg->slots[j].algo = (uint8_t)ids[j];
    cfg->slots[j].on = ids[j] != FX_ALGO_NONE;
    const fx_param_desc_t *pd = fx_algo_params(ids[j]);
    for (uint32_t i = 0; i < fx_algo_param_count(ids[j]); i++) cfg->slots[j].params[i] = pd[i].def;
  }
}

static uint32_t cost_of(const fx_chain_cfg_t *cfg, uint32_t overhead)
{
  uint32_t c = overhead;
  for (uint32_t j = 0; j < FX_SLOTS; j++)
  {
    const fx_algo_t *a = fx_algo_get((fx_algo_id_t)cfg->slots[j].algo);
    if (a) c += a->cost;
  }
  return c;
}

static uint32_t align8(uint32_t v) { return (v + 7u) & ~7u; }

/* Independent check: can the fixed blocks be split over the two arenas at all,
 * and what is the largest region left afterwards (for FX_MEM_REST)? */
static int brute_feasible(const fx_chain_cfg_t *cfg, uint32_t *rest_largest)
{
  uint32_t sizes[FX_SLOTS], n = 0, rest_min = 0;
  int rest_count = 0;
  uint32_t cap[2];
  (void)fx_engine_arena(0, &cap[0]);
  (void)fx_engine_arena(1, &cap[1]);
  for (uint32_t j = 0; j < FX_SLOTS; j++)
  {
    const fx_algo_t *a = fx_algo_get((fx_algo_id_t)cfg->slots[j].algo);
    if (!a) continue;
    if (a->mem_bytes == FX_MEM_REST)
    {
      rest_count++;
      if (a->min_mem_bytes > rest_min) rest_min = a->min_mem_bytes;
    }
    else if (a->mem_bytes) sizes[n++] = align8(a->mem_bytes);
  }
  int feasible = 0;
  uint32_t best_rest = 0;
  for (uint32_t mask = 0; mask < (1u << n); mask++)
  {
    uint32_t used[2] = { 0, 0 };
    for (uint32_t i = 0; i < n; i++) used[(mask >> i) & 1u] += sizes[i];
    if (used[0] > cap[0] || used[1] > cap[1]) continue;
    const uint32_t f0 = cap[0] - used[0], f1 = cap[1] - used[1];
    const uint32_t big = f0 > f1 ? f0 : f1;
    if (rest_count && big < rest_min) continue;
    feasible = 1;
    if (big > best_rest) best_rest = big;
  }
  *rest_largest = best_rest;
  return feasible;
}

static void check_blocks(const char *label)
{
  uint32_t cap[2];
  const uint8_t *base[2] = { fx_engine_arena(0, &cap[0]), fx_engine_arena(1, &cap[1]) };
  const uint8_t *p[FX_SLOTS];
  uint32_t len[FX_SLOTS];
  for (uint32_t j = 0; j < FX_SLOTS; j++)
  {
    p[j] = fx_engine_slot_mem(j, &len[j]);
    if (!p[j]) continue;
    const fx_algo_t *a = fx_algo_get(fx_slot_algo(j));
    int inside = 0;
    for (int ar = 0; ar < 2; ar++)
      if (p[j] >= base[ar] && p[j] + len[j] <= base[ar] + cap[ar]) inside = 1;
    CHECK(inside, "%s: slot %u block outside arenas", label, j);
    CHECK(((uintptr_t)p[j] & 7u) == 0 && (len[j] & 7u) == 0, "%s: slot %u misaligned", label, j);
    CHECK(a && (a->mem_bytes == FX_MEM_REST ? len[j] >= a->min_mem_bytes : len[j] >= a->mem_bytes),
          "%s: slot %u block too small (%u)", label, j, len[j]);
  }
  for (uint32_t i = 0; i < FX_SLOTS; i++)
    for (uint32_t j = i + 1; j < FX_SLOTS; j++)
      if (p[i] && p[j])
        CHECK(p[i] + len[i] <= p[j] || p[j] + len[j] <= p[i], "%s: slots %u and %u overlap", label, i, j);
  CHECK(fx_mem_used() <= fx_mem_total(), "%s: mem used > total", label);
}

static uint32_t fnv(const uint8_t *p, uint32_t n)
{
  uint32_t h = 2166136261u;
  while (n--) h = (h ^ *p++) * 16777619u;
  return h;
}

static int cfg_equal(const fx_chain_cfg_t *a, const fx_chain_cfg_t *b)
{
  return memcmp(a, b, sizeof(*a)) == 0;
}

static float ref_round(float v, float mn, float mx, float st)
{
  if (!(v > mn)) v = mn; else if (v > mx) v = mx;
  if (st > 0) v = mn + floorf((v - mn) / st + 0.5f) * st;
  return v > mx ? mx : v;
}

/* ---- tests -------------------------------------------------------------- */

static void test_defaults(void)
{
  engine_start();
  CHECK(fx_slot_algo(0) == FX_ALGO_OVERDRIVE && fx_slot_algo(1) == FX_ALGO_CHORUS &&
        fx_slot_algo(2) == FX_ALGO_DIGITAL_DLY && fx_slot_algo(3) == FX_ALGO_HALL, "default chain algos");
  CHECK(fx_slot_enabled(0) && !fx_slot_enabled(1) && fx_slot_enabled(2) && fx_slot_enabled(3), "default on flags");
  CHECK(fx_get_global(FXP_OUT_LEVEL) == g_fx_param_info[FXP_OUT_LEVEL].def, "default out.level");
  check_blocks("defaults");
  CHECK(fx_cost_used() <= fx_cost_budget(), "default chain over budget");
  printf("     defaults: mem %u/%u B, cost %u/%u cycles, largest free %u B\n", fx_mem_used(),
         fx_mem_total(), fx_cost_used(), fx_cost_budget(), fx_mem_free_largest());
}

static void test_placement(void)
{
  engine_start();
  fx_chain_cfg_t cfg;
  set_chain(FX_ALGO_NONE, FX_ALGO_NONE, FX_ALGO_NONE, FX_ALGO_NONE, &cfg);
  CHECK(fx_apply_config(&cfg) == FX_OK, "empty chain");
  const uint32_t overhead = fx_cost_used();
  CHECK(fx_mem_used() == 0 && fx_mem_free_largest() > 0, "empty chain memory");

  /* delay + delay + reverb + looper */
  set_chain(FX_ALGO_DIGITAL_DLY, FX_ALGO_TAPE_ECHO, FX_ALGO_HALL, FX_ALGO_LOOPER, &cfg);
  uint32_t best_rest = 0;
  const int feas = brute_feasible(&cfg, &best_rest);
  const int cost_ok = cost_of(&cfg, overhead) <= fx_cost_budget();
  const fx_result_t r = fx_apply_config(&cfg);
  printf("     delay+echo+hall+looper: %s (brute-force feasible=%d, cost ok=%d)\n",
         r == FX_OK ? "accepted" : (r == FX_ERR_MEMORY ? "ERR_MEMORY" : "ERR_CPU"), feas, cost_ok);
  CHECK(r == (!cost_ok ? FX_ERR_CPU : (feas ? FX_OK : FX_ERR_MEMORY)), "expected result mismatch (%d)", (int)r);
  if (r == FX_OK)
  {
    check_blocks("d+d+r+l");
    uint32_t llen = 0;
    CHECK(fx_engine_slot_mem(3, &llen) != NULL, "looper has no block");
    CHECK(llen == best_rest, "looper got %u, largest free region is %u", llen, best_rest);
  }

  /* MEM_REST gets the largest remaining region. */
  set_chain(FX_ALGO_NONE, FX_ALGO_NONE, FX_ALGO_NONE, FX_ALGO_LOOPER, &cfg);
  CHECK(fx_apply_config(&cfg) == FX_OK, "looper alone");
  uint32_t ll = 0, cap0, cap1;
  (void)fx_engine_slot_mem(3, &ll);
  (void)fx_engine_arena(0, &cap0);
  (void)fx_engine_arena(1, &cap1);
  CHECK(ll == (cap0 > cap1 ? cap0 : cap1), "looper alone should get a whole arena (%u)", ll);

  /* Random chains: results must be consistent with cost and memory limits. */
  uint32_t seed = 12345u, n_ok = 0, n_cpu = 0, n_mem = 0, gaps = 0;
  fx_chain_cfg_t before, after;
  for (int iter = 0; iter < 1500; iter++)
  {
    fx_algo_id_t ids[FX_SLOTS];
    for (uint32_t j = 0; j < FX_SLOTS; j++)
    {
      seed = seed * 1664525u + 1013904223u;
      ids[j] = (fx_algo_id_t)((seed >> 16) % FX_ALGO_COUNT);
    }
    set_chain(ids[0], ids[1], ids[2], ids[3], &cfg);
    fx_get_config(&before);
    const fx_result_t rr = fx_apply_config(&cfg);
    uint32_t br;
    const int f = brute_feasible(&cfg, &br);
    const int c_ok = cost_of(&cfg, overhead) <= fx_cost_budget();
    if (!c_ok)
    {
      CHECK(rr == FX_ERR_CPU, "iter %d: expected ERR_CPU got %d", iter, (int)rr);
      n_cpu++;
    }
    else if (rr == FX_OK)
    {
      CHECK(f, "iter %d: accepted a chain the brute force says cannot fit", iter);
      check_blocks("random");
      n_ok++;
    }
    else
    {
      CHECK(rr == FX_ERR_MEMORY, "iter %d: unexpected result %d", iter, (int)rr);
      if (f) gaps++;
      n_mem++;
    }
    if (rr != FX_OK)
    {
      fx_get_config(&after);
      CHECK(cfg_equal(&before, &after), "iter %d: chain changed on error", iter);
    }
  }
  printf("     random chains: ok=%u cpu-reject=%u mem-reject=%u (heuristic gaps vs optimal: %u)\n",
         n_ok, n_cpu, n_mem, gaps);

  /* Budget: four of the most expensive algorithms. */
  fx_algo_id_t worst = FX_ALGO_NONE;
  for (uint32_t i = 1; i < FX_ALGO_COUNT; i++)
    if (worst == FX_ALGO_NONE || fx_algo_get((fx_algo_id_t)i)->cost > fx_algo_get(worst)->cost) worst = (fx_algo_id_t)i;
  set_chain(worst, worst, worst, worst, &cfg);
  fx_get_config(&before);
  if (cost_of(&cfg, overhead) > fx_cost_budget())
  {
    CHECK(fx_apply_config(&cfg) == FX_ERR_CPU, "4x %s should exceed the CPU budget", fx_algo_name(worst));
    fx_get_config(&after);
    CHECK(cfg_equal(&before, &after), "chain changed after CPU reject");
  }
  else
  {
    printf("     note: 4x costliest algorithm (%s, %u cycles) fits the budget, reject path covered by random chains only\n",
           fx_algo_name(worst), fx_algo_get(worst)->cost);
  }
}

static void test_move_keeps_loop(void)
{
  engine_start();
  fx_engine_wait_hook = pump;
  fx_chain_cfg_t cfg;
  set_chain(FX_ALGO_OVERDRIVE, FX_ALGO_LOOPER, FX_ALGO_CHORUS, FX_ALGO_HALL, &cfg);
  CHECK(fx_apply_config(&cfg) == FX_OK, "looper chain rejected");
  run_blocks(4);

  CHECK(fx_slot_command(1, FX_CMD_REC) == FX_OK, "REC");
  fx_status_t st;
  CHECK(fx_slot_status(1, &st) == FX_OK && st.state == FX_LOOP_RECORDING,
        "status right after REC returns state %u", st.state);
  run_blocks(60);
  CHECK(fx_slot_command(1, FX_CMD_REC) == FX_OK, "REC close");
  CHECK(fx_slot_status(1, &st) == FX_OK, "status");
  CHECK(st.state == FX_LOOP_PLAYING && st.len > 0, "loop not playing (state %u len %u)", st.state, st.len);
  const uint32_t len0 = st.len;
  uint32_t mlen = 0;
  const uint8_t *mem0 = fx_engine_slot_mem(1, &mlen);
  const uint32_t hash0 = fnv(mem0, mlen);

  CHECK(fx_slot_move(1, 3) == FX_OK, "move looper to the end");
  CHECK(fx_slot_algo(3) == FX_ALGO_LOOPER && fx_slot_algo(1) == FX_ALGO_CHORUS, "order after move");
  uint32_t mlen1 = 0;
  const uint8_t *mem1 = fx_engine_slot_mem(3, &mlen1);
  CHECK(mem1 == mem0 && mlen1 == mlen, "looper block moved");
  CHECK(fnv(mem1, mlen1) == hash0, "looper memory changed by MOVE");
  CHECK(fx_slot_status(3, &st) == FX_OK && st.len == len0 &&
            (st.state == FX_LOOP_PLAYING || st.state == FX_LOOP_OVERDUB),
        "looper state lost after MOVE (state %u len %u, was %u)", st.state, st.len, len0);

  CHECK(fx_slot_move(3, 0) == FX_OK, "move looper to the front");
  run_blocks(3);
  CHECK(fx_slot_status(0, &st) == FX_OK && st.len == len0 && st.state == FX_LOOP_PLAYING,
        "looper state lost after second MOVE (state %u len %u)", st.state, st.len);

  CHECK(fx_slot_assign(2, FX_ALGO_OVERDRIVE) == FX_OK, "assign elsewhere");
  CHECK(fx_slot_status(0, &st) == FX_OK && st.len == len0, "looper wiped by unrelated assign");

  CHECK(fx_slot_command(1, FX_CMD_REC) == FX_ERR_NO_CMD, "command on non-looper");
  CHECK(fx_slot_command(0, FX_CMD_STOP) == FX_OK && fx_slot_status(0, &st) == FX_OK &&
            st.state == FX_LOOP_STOPPED, "STOP not visible right after the call (state %u)", st.state);
  CHECK(fx_slot_command(0, FX_CMD_CLEAR) == FX_OK && fx_slot_status(0, &st) == FX_OK &&
            st.state == FX_LOOP_EMPTY && st.len == 0, "CLEAR not visible right after the call");
  CHECK(fx_slot_status(1, &st) == FX_ERR_NO_CMD, "status on non-looper");
  CHECK(fx_slot_command(9, FX_CMD_REC) == FX_ERR_RANGE, "command on bad slot");
  fx_engine_wait_hook = NULL;
}

static void toggle_run(int toggle)
{
  engine_start();
  fx_engine_wait_hook = pump;
  run_blocks(100);
  stats_reset();
  uint32_t seed = 777u;
  for (int b = 0; b < 600; b++)
  {
    seed = seed * 1664525u + 1013904223u;
    const uint32_t r = seed >> 16;
    if (toggle && r % 7u == 0u) (void)fx_slot_enable(r % FX_SLOTS, (r >> 4) & 1u);
    if (toggle && r % 53u == 0u) (void)fx_set_global(FXP_BYPASS, (float)((r >> 5) & 1u));
    if (toggle && r % 11u == 0u)
    {
      const uint32_t s = r % FX_SLOTS, cnt = fx_algo_param_count(fx_slot_algo(s));
      if (cnt)
      {
        const fx_param_desc_t *pd = &fx_algo_params(fx_slot_algo(s))[(r >> 3) % cnt];
        (void)fx_slot_set_param(s, (r >> 3) % cnt, pd->min + (pd->max - pd->min) * (float)((r >> 6) & 15u) / 15.0f, NULL);
      }
    }
    run_block();
  }
}

static void test_command_direct(void)
{
  engine_start(); /* audio not running: commands apply directly */
  fx_chain_cfg_t cfg;
  set_chain(FX_ALGO_NONE, FX_ALGO_LOOPER, FX_ALGO_NONE, FX_ALGO_NONE, &cfg);
  CHECK(fx_apply_config(&cfg) == FX_OK, "looper chain");
  fx_status_t st;
  CHECK(fx_slot_command(1, FX_CMD_REC) == FX_OK, "direct command before audio start");
  run_blocks(1); /* the looper latches commands and applies them in its next block */
  CHECK(fx_slot_status(1, &st) == FX_OK && st.state == FX_LOOP_RECORDING,
        "command lost before audio start (state %u)", st.state);
}

static void test_toggles(void)
{
  toggle_run(0);
  const stats_t base = s_stats;
  CHECK(fx_engine_nan_count == 0, "NaN in baseline");
  toggle_run(1);
  CHECK(fx_engine_nan_count == 0, "NaN produced while toggling (%u)", fx_engine_nan_count);
  CHECK(s_stats.saturated < 400, "excess clipping (%u samples)", s_stats.saturated);
  /* A crossfade to bypass may legitimately reach the dry signal's own steps (x out.level 0.7). */
  const int allowed = 2 * (base.max_step > base.in_step * 7 / 10 ? base.max_step : base.in_step * 7 / 10) + 1500;
  CHECK(s_stats.max_step <= allowed, "click: max step %d, allowed %d (baseline %d)", s_stats.max_step, allowed, base.max_step);
  printf("     toggles: peak %d, max step %d (baseline %d), saturated %u\n", s_stats.peak, s_stats.max_step,
         base.max_step, s_stats.saturated);

  /* Random structural churn. */
  uint32_t seed2 = 99u;
  for (int i = 0; i < 200; i++)
  {
    seed2 = seed2 * 1664525u + 1013904223u;
    (void)fx_slot_assign((seed2 >> 8) % FX_SLOTS, (fx_algo_id_t)((seed2 >> 16) % FX_ALGO_COUNT));
    if (i % 5 == 0) (void)fx_slot_move((seed2 >> 4) % FX_SLOTS, (seed2 >> 12) % FX_SLOTS);
    run_blocks(2);
    check_blocks("churn");
  }
  CHECK(fx_engine_nan_count == 0, "NaN produced during structural churn");

  /* Bypass settles to dry x out.level. */
  fx_chain_cfg_t cfg;
  set_chain(FX_ALGO_OVERDRIVE, FX_ALGO_CHORUS, FX_ALGO_DIGITAL_DLY, FX_ALGO_HALL, &cfg);
  CHECK(fx_apply_config(&cfg) == FX_OK, "default chain");
  (void)fx_set_global(FXP_BYPASS, 1.0f);
  (void)fx_set_global(FXP_OUT_LEVEL, 0.5f);
  run_blocks(40);
  int worst = 0;
  for (uint32_t i = 0; i < BLOCK; i++)
  {
    const int want = (int)lrintf((float)s_in[i] * 0.5f);
    worst = abs(s_out[2 * i] - want) > worst ? abs(s_out[2 * i] - want) : worst;
  }
  CHECK(worst <= 2, "bypass output deviates from dry*level by %d LSB", worst);
  fx_engine_wait_hook = NULL;
}

static void test_tail(void)
{
  engine_start();
  fx_engine_wait_hook = pump;
  fx_chain_cfg_t cfg;
  set_chain(FX_ALGO_NONE, FX_ALGO_NONE, FX_ALGO_DIGITAL_DLY, FX_ALGO_NONE, &cfg);
  CHECK(fx_apply_config(&cfg) == FX_OK, "delay chain");
  run_blocks(120);
  (void)fx_slot_enable(2, false);
  s_silence = 1;
  run_blocks(3);
  stats_reset();
  run_blocks(120);
  const int tails = fx_algo_get(FX_ALGO_DIGITAL_DLY)->tails;
  printf("     tail after switch-off: energy %.6f (tails=%d)\n", s_stats.energy, tails);
  if (tails) CHECK(s_stats.energy > 1e-6, "delay tail was cut when the slot was switched off");
  else CHECK(s_stats.energy < 1e-9, "non-tail slot still producing output");
  fx_engine_wait_hook = NULL;
}

static void test_config_and_params(void)
{
  engine_start();
  fx_chain_cfg_t a, b;
  fx_get_config(&a);
  a.globals[FXP_OUT_LEVEL] = 0.5f;
  a.slots[2].params[0] = 123.0f;
  a.slots[2].params[1] = 0.5f;
  a.slots[1].on = 1;
  CHECK(fx_apply_config(&a) == FX_OK, "apply_config");
  fx_get_config(&b);
  CHECK(b.globals[FXP_OUT_LEVEL] == 0.5f, "global round trip");
  CHECK(fabsf(b.slots[2].params[0] - 123.0f) < 1e-3f && fabsf(b.slots[2].params[1] - 0.5f) < 1e-3f &&
            b.slots[1].on == 1,
        "slot round trip");
  fx_chain_cfg_t c;
  fx_get_config(&c);
  CHECK(cfg_equal(&b, &c), "get_config not idempotent");
  CHECK(fx_apply_config(&b) == FX_OK, "re-apply");
  fx_get_config(&c);
  CHECK(cfg_equal(&b, &c), "config changed by re-apply");

  fx_chain_cfg_t bad = b;
  bad.slots[3].algo = FX_ALGO_COUNT;
  CHECK(fx_apply_config(&bad) == FX_ERR_RANGE, "bad algo id accepted");
  fx_get_config(&c);
  CHECK(cfg_equal(&b, &c), "chain changed after RANGE error");

  /* Clamping inside apply_config. */
  bad = b;
  bad.slots[2].params[0] = 1e9f;
  bad.slots[2].params[1] = -5.0f;
  CHECK(fx_apply_config(&bad) == FX_OK, "clamped apply");
  const fx_param_desc_t *pd = fx_algo_params(FX_ALGO_DIGITAL_DLY);
  CHECK(fx_slot_get_param(2, 0) == pd[0].max && fx_slot_get_param(2, 1) == pd[1].min, "apply_config clamping");

  /* Step rounding and ranges. */
  CHECK(fx_slot_assign(2, FX_ALGO_DIGITAL_DLY) == FX_OK, "assign delay");
  float ap = 0;
  CHECK(fx_slot_set_param(2, 0, 400.4f, &ap) == FX_OK && ap == 400.0f, "integer step: %f", (double)ap);
  CHECK(fx_slot_set_param(2, 0, 400.6f, &ap) == FX_OK && ap == 401.0f, "integer step up: %f", (double)ap);
  CHECK(fx_slot_set_param(2, 0, 5.0f, &ap) == FX_OK && ap == pd[0].min, "clamp min");
  CHECK(fx_slot_set_param(2, 0, 5000.0f, &ap) == FX_OK && ap == pd[0].max, "clamp max");
  CHECK(fx_slot_set_param(2, 1, 0.337f, &ap) == FX_OK && fabsf(ap - ref_round(0.337f, pd[1].min, pd[1].max, pd[1].step)) < 1e-5f &&
            fabsf(ap - 0.34f) < 1e-4f, "fractional step: %f", (double)ap);
  CHECK(fx_slot_set_param(2, 1, NAN, &ap) == FX_OK && ap == pd[1].min, "NaN clamps to min");
  CHECK(fx_slot_get_param(2, 1) == pd[1].min, "stored value");
  CHECK(fx_slot_set_param(2, 8, 1.0f, NULL) == FX_ERR_RANGE, "param idx out of range");
  CHECK(fx_slot_set_param(7, 0, 1.0f, NULL) == FX_ERR_RANGE, "slot out of range");
  CHECK(fx_slot_assign(0, FX_ALGO_COUNT) == FX_ERR_RANGE, "assign bad id");
  CHECK(fx_slot_assign(5, FX_ALGO_EQ) == FX_ERR_RANGE, "assign bad slot");
  CHECK(fx_slot_move(0, 4) == FX_ERR_RANGE, "move bad slot");

  CHECK(fx_set_global(FXP_BYPASS, 0.6f) == 1.0f, "bypass rounds to 1");
  CHECK(fx_set_global(FXP_IN_LEVEL, 5.0f) == 2.0f, "in.level clamp");
  CHECK(fabsf(fx_set_global(FXP_IN_LEVEL, 0.337f) - 0.34f) < 1e-4f, "in.level step");

  CHECK(fx_slot_assign(3, FX_ALGO_NONE) == FX_OK && !fx_slot_enabled(3), "assign Empty switches off");
  CHECK(fx_slot_enable(3, true) == FX_OK && !fx_slot_enabled(3), "empty slot cannot be enabled");
}

static void write_wav(const char *path, const int16_t *lr, uint32_t frames)
{
  FILE *f = fopen(path, "wb");
  CHECK(f != NULL, "cannot write %s", path);
  if (!f) return;
  const uint32_t bytes = frames * 4u, rate = FX_SAMPLE_RATE, brate = FX_SAMPLE_RATE * 4u;
  const uint16_t one = 1, two = 2, four = 4, sixteen = 16;
  const uint32_t riff = 36u + bytes, fmt = 16u;
  fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
  fwrite(&fmt, 4, 1, f); fwrite(&one, 2, 1, f); fwrite(&two, 2, 1, f); fwrite(&rate, 4, 1, f);
  fwrite(&brate, 4, 1, f); fwrite(&four, 2, 1, f); fwrite(&sixteen, 2, 1, f);
  fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
  fwrite(lr, 2, 2u * frames, f);
  fclose(f);
}

static void test_default_wav(void)
{
  engine_start();
  fx_engine_wait_hook = pump;
  const uint32_t in_blocks = (uint32_t)(4.0f * FX_SAMPLE_RATE) / BLOCK;
  const uint32_t tail_blocks = (uint32_t)(2.0f * FX_SAMPLE_RATE) / BLOCK;
  int16_t *wav = malloc((size_t)(in_blocks + tail_blocks) * BLOCK * 4u);
  CHECK(wav != NULL, "alloc");
  if (!wav) return;
  stats_reset();
  double tail_energy = 0;
  for (uint32_t b = 0; b < in_blocks + tail_blocks; b++)
  {
    s_silence = b >= in_blocks;
    const double e0 = s_stats.energy;
    run_block();
    if (b >= in_blocks + tail_blocks - 20u) tail_energy += s_stats.energy - e0;
    memcpy(wav + (size_t)b * BLOCK * 2u, s_out, sizeof(s_out));
  }
  write_wav("out/chain_default.wav", wav, (in_blocks + tail_blocks) * BLOCK);
  free(wav);
  CHECK(fx_engine_nan_count == 0, "NaN in default render");
  CHECK(s_stats.peak > 500 && s_stats.peak < 32767, "default render peak %d", s_stats.peak);
  CHECK(s_stats.saturated == 0, "default render clipped (%u samples)", s_stats.saturated);
  CHECK(tail_energy < 0.01, "reverb/delay tail not decaying (%.5f)", tail_energy);
  printf("     default chain: peak %d, tail energy %.6f -> out/chain_default.wav\n", s_stats.peak, tail_energy);
  fx_engine_wait_hook = NULL;
}

int main(void)
{
  test_defaults();
  test_placement();
  test_move_keeps_loop();
  test_command_direct();
  test_toggles();
  test_tail();
  test_config_and_params();
  test_default_wav();
  printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "ok", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
