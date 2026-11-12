/*
 * fx_engine.c - 4-slot effect chain core (see fx_engine.h for the contract).
 *
 * Ownership:
 *   - s_main, s_glob, the command queues and the request mailbox are written by
 *     main-loop code. Everything else (s_rt, s_state, arenas, smoothers) belongs
 *     to the audio ISR, or to the main loop while audio is not running.
 *   - Structural changes are validated in main context, then applied at a block
 *     start by apply_structure().
 */
#include "fx_engine.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "dsp_util.h"

#define ARENA_SRAM_BYTES  (88u * 1024u)
#define ARENA_CCM_BYTES   (62u * 1024u)

/* Per-frame cost of the chain itself (input/output stages, crossfades). */
#define CHAIN_OVERHEAD_CYCLES  250u
#define CPU_BUDGET_CYCLES      2400u

/* Main-side wait for the ISR to take a request, in loop iterations (~tens of
 * ms at 168 MHz, i.e. several audio blocks). */
#define REQ_WAIT_SPINS  2000000u

#define CMD_QUEUE_LEN   8u

/* Slot on/off crossfade: 10 ms at 48 kHz. */
#define XFADE_PER_SAMPLE (1.0f / 480.0f)

enum { ARENA_SRAM, ARENA_CCM, ARENA_COUNT };

static uint8_t s_arena_sram[ARENA_SRAM_BYTES] __attribute__((aligned(8)));
/* NOLOAD: not zeroed by startup. Algorithms clear what they use in init(). */
static uint8_t s_arena_ccm[ARENA_CCM_BYTES] __attribute__((section(".ccmram"), aligned(8)));

static const uint32_t k_arena_cap[ARENA_COUNT] = { ARENA_SRAM_BYTES, ARENA_CCM_BYTES };

static uint8_t *arena_base(uint32_t arena)
{
  return arena == ARENA_SRAM ? s_arena_sram : s_arena_ccm;
}

static uint8_t s_state[FX_SLOTS][FX_STATE_BYTES] __attribute__((aligned(8)));

/* Optional hook run inside the main-side wait loop; host tests use it to drive
 * fx_process() as the ISR would. */
void (*fx_engine_wait_hook)(void);

/* Count of non-finite samples replaced by silence at the output. */
volatile uint32_t fx_engine_nan_count;

/* ------------------------------------------------------------------------- */
/* Placement (pure)                                                          */
/* ------------------------------------------------------------------------- */

typedef struct
{
  uint8_t arena;
  uint32_t off;
  uint32_t len;   /* 0 = no block */
} block_t;

typedef struct
{
  block_t blk[FX_SLOTS];
  uint32_t used[ARENA_COUNT];
} layout_t;

static uint32_t align8(uint32_t v)
{
  return (v + 7u) & ~7u;
}

static bool is_fixed_mem(const fx_algo_t *a)
{
  return a != NULL && a->mem_bytes != FX_MEM_REST && a->mem_bytes > 0u;
}

/* Largest-first by size; ties by algorithm id then slot, so equal blocks land
 * in the same place after a reorder. */
static bool placed_before(const uint8_t ids[FX_SLOTS], uint32_t a, uint32_t b)
{
  const uint32_t sa = fx_algo_get((fx_algo_id_t)ids[a])->mem_bytes;
  const uint32_t sb = fx_algo_get((fx_algo_id_t)ids[b])->mem_bytes;
  if (sa != sb)
  {
    return sa > sb;
  }
  if (ids[a] != ids[b])
  {
    return ids[a] < ids[b];
  }
  return a < b;
}

static uint32_t arena_free(const layout_t *lay, uint32_t arena)
{
  return k_arena_cap[arena] - lay->used[arena];
}

/* with_rest = false leaves FX_MEM_REST algorithms unplaced (free-space query). */
static fx_result_t place_chain(const uint8_t ids[FX_SLOTS], bool with_rest, layout_t *lay)
{
  memset(lay, 0, sizeof(*lay));

  uint32_t order[FX_SLOTS];
  uint32_t count = 0;
  for (uint32_t j = 0; j < FX_SLOTS; j++)
  {
    if (is_fixed_mem(fx_algo_get((fx_algo_id_t)ids[j])))
    {
      uint32_t k = count++;
      while (k > 0 && placed_before(ids, j, order[k - 1]))
      {
        order[k] = order[k - 1];
        k--;
      }
      order[k] = j;
    }
  }

  for (uint32_t n = 0; n < count; n++)
  {
    const uint32_t j = order[n];
    const uint32_t size = align8(fx_algo_get((fx_algo_id_t)ids[j])->mem_bytes);
    uint32_t best = ARENA_COUNT;
    uint32_t best_left = 0;
    for (uint32_t ar = 0; ar < ARENA_COUNT; ar++)
    {
      const uint32_t fr = arena_free(lay, ar);
      if (size <= fr && (best == ARENA_COUNT || fr - size < best_left))
      {
        best = ar;
        best_left = fr - size;
      }
    }
    if (best == ARENA_COUNT)
    {
      return FX_ERR_MEMORY;
    }
    lay->blk[j] = (block_t){ (uint8_t)best, lay->used[best], size };
    lay->used[best] += size;
  }

  if (!with_rest)
  {
    return FX_OK;
  }

  for (uint32_t j = 0; j < FX_SLOTS; j++)
  {
    const fx_algo_t *a = fx_algo_get((fx_algo_id_t)ids[j]);
    if (a == NULL || a->mem_bytes != FX_MEM_REST)
    {
      continue;
    }
    uint32_t best = ARENA_SRAM;
    if (arena_free(lay, ARENA_CCM) > arena_free(lay, ARENA_SRAM))
    {
      best = ARENA_CCM;
    }
    const uint32_t size = arena_free(lay, best) & ~7u;
    if (size == 0u || size < a->min_mem_bytes)
    {
      return FX_ERR_MEMORY;
    }
    lay->blk[j] = (block_t){ (uint8_t)best, lay->used[best], size };
    lay->used[best] += size;
  }
  return FX_OK;
}

static uint32_t chain_cost(const uint8_t ids[FX_SLOTS])
{
  uint32_t cost = CHAIN_OVERHEAD_CYCLES;
  for (uint32_t j = 0; j < FX_SLOTS; j++)
  {
    const fx_algo_t *a = fx_algo_get((fx_algo_id_t)ids[j]);
    if (a != NULL)
    {
      cost += a->cost;
    }
  }
  return cost;
}

static fx_result_t check_chain(const uint8_t ids[FX_SLOTS])
{
  layout_t lay;
  if (chain_cost(ids) > CPU_BUDGET_CYCLES)
  {
    return FX_ERR_CPU;
  }
  return place_chain(ids, true, &lay);
}

/* ------------------------------------------------------------------------- */
/* Shared state                                                              */
/* ------------------------------------------------------------------------- */

/* Main-owned configuration. The ISR reads params and `on` only for slots whose
 * algorithm id matches its live one, so a half-committed structural change is
 * never fed to the old algorithm. */
static fx_slot_cfg_t s_main[FX_SLOTS];
static volatile float s_glob[FXP_COUNT];
static volatile uint32_t s_dirty;                 /* bit per slot: params changed */
static volatile uint8_t s_live_id[FX_SLOTS];      /* ISR-published algorithm ids */
static volatile uint32_t s_audio_running;

typedef struct
{
  uint8_t q[CMD_QUEUE_LEN];
  volatile uint32_t head;   /* main */
  volatile uint32_t tail;   /* ISR */
  volatile uint32_t acked;  /* ISR: tail value at the end of the last processed block */
} cmd_queue_t;

static cmd_queue_t s_cmdq[FX_SLOTS];

/* Request mailbox: 0 idle, 1 pending, 2 taken by the ISR. */
static uint8_t s_req_ids[FX_SLOTS];
static volatile uint32_t s_req_state;
static volatile uint32_t s_req_seq;

/* ISR-owned runtime per slot. */
typedef struct
{
  const fx_algo_t *algo;
  block_t blk;
  uint8_t id;
  float g;        /* crossfade gain 0..1 */
  float on_tgt;
  float params[FX_ALGO_MAX_PARAMS];
} slot_rt_t;

static slot_rt_t s_rt[FX_SLOTS];

typedef struct
{
  float cur;
  float tgt;
} smooth_t;

enum { GS_IN, GS_BYPASS, GS_OUT, GS_COUNT };
static smooth_t s_gs[GS_COUNT];

static float s_l[FX_BLOCK_FRAMES];
static float s_r[FX_BLOCK_FRAMES];
/* Dry copies for the crossfade; reused as the state-swap buffer, since
 * structural changes never run in the middle of a chunk. */
static union
{
  struct
  {
    float l[FX_BLOCK_FRAMES];
    float r[FX_BLOCK_FRAMES];
  } dry;
  uint8_t swap[FX_STATE_BYTES];
} s_scratch __attribute__((aligned(8)));

#define s_tl s_scratch.dry.l
#define s_tr s_scratch.dry.r

/* ------------------------------------------------------------------------- */
/* Structural apply (ISR at block start, or main while audio is stopped)     */
/* ------------------------------------------------------------------------- */

static bool same_block(const block_t *a, const block_t *b)
{
  return a->len == b->len && (a->len == 0u || (a->arena == b->arena && a->off == b->off));
}

/* new[k] = old[perm[k]] for the state areas, in place with one temporary. */
static void permute_states(const uint8_t perm[FX_SLOTS])
{
  bool done[FX_SLOTS] = { false };
  for (uint32_t s = 0; s < FX_SLOTS; s++)
  {
    if (done[s] || perm[s] == s)
    {
      continue;
    }
    memcpy(s_scratch.swap, s_state[s], FX_STATE_BYTES);
    uint32_t k = s;
    for (;;)
    {
      done[k] = true;
      const uint32_t nxt = perm[k];
      if (nxt == s)
      {
        memcpy(s_state[k], s_scratch.swap, FX_STATE_BYTES);
        break;
      }
      memcpy(s_state[k], s_state[nxt], FX_STATE_BYTES);
      k = nxt;
    }
  }
}

/* Slots keep their running state when an identical algorithm with an identical
 * block exists in the old chain, even at another position (a MOVE). Algorithm
 * state areas must therefore not hold pointers into themselves. */
static void apply_structure(const uint8_t ids[FX_SLOTS])
{
  layout_t lay;
  (void)place_chain(ids, true, &lay); /* validated by the requester */

  int32_t src[FX_SLOTS];
  bool used[FX_SLOTS] = { false };
  for (uint32_t j = 0; j < FX_SLOTS; j++)
  {
    const fx_algo_t *a = fx_algo_get((fx_algo_id_t)ids[j]);
    src[j] = -1;
    for (uint32_t i = 0; a != NULL && i < FX_SLOTS; i++)
    {
      if (!used[i] && s_rt[i].algo == a && same_block(&s_rt[i].blk, &lay.blk[j]))
      {
        src[j] = (int32_t)i;
        used[i] = true;
        break;
      }
    }
  }

  uint8_t perm[FX_SLOTS];
  for (uint32_t j = 0; j < FX_SLOTS; j++)
  {
    if (src[j] >= 0)
    {
      perm[j] = (uint8_t)src[j];
    }
    else
    {
      uint32_t pick = used[j] ? 0u : j;
      while (used[pick])
      {
        pick++;
      }
      used[pick] = true;
      perm[j] = (uint8_t)pick;
    }
  }

  permute_states(perm);
  const slot_rt_t old[FX_SLOTS] = { s_rt[0], s_rt[1], s_rt[2], s_rt[3] };

  for (uint32_t j = 0; j < FX_SLOTS; j++)
  {
    slot_rt_t *s = &s_rt[j];
    *s = old[perm[j]];
    if (src[j] < 0)
    {
      const fx_algo_t *a = fx_algo_get((fx_algo_id_t)ids[j]);
      memset(s, 0, sizeof(*s));
      s->algo = a;
      s->blk = lay.blk[j];
      s->id = ids[j];
      s_cmdq[j].tail = s_cmdq[j].head;
      s_cmdq[j].acked = s_cmdq[j].head;
      if (a != NULL)
      {
        a->init(s_state[j], arena_base(s->blk.arena) + s->blk.off, s->blk.len);
      }
    }
    s_live_id[j] = ids[j];
  }
  __atomic_store_n(&s_dirty, (1u << FX_SLOTS) - 1u, __ATOMIC_SEQ_CST);
}

static void consume_request(void)
{
  uint32_t expect = 1u;
  if (__atomic_load_n(&s_req_state, __ATOMIC_SEQ_CST) == 1u &&
      __atomic_compare_exchange_n(&s_req_state, &expect, 2u, false,
                                  __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
  {
    apply_structure(s_req_ids);
    __atomic_store_n(&s_req_seq, s_req_seq + 1u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&s_req_state, 0u, __ATOMIC_SEQ_CST);
  }
}

static bool wait_for_seq(uint32_t seq0)
{
  for (uint32_t n = 0; n < REQ_WAIT_SPINS; n++)
  {
    if (__atomic_load_n(&s_req_seq, __ATOMIC_SEQ_CST) != seq0)
    {
      return true;
    }
    if (fx_engine_wait_hook != NULL)
    {
      fx_engine_wait_hook();
    }
  }
  return false;
}

/* True once the ISR has delivered every command up to `head` and finished the
 * block in which it did so. */
static bool wait_for_ack(const cmd_queue_t *q, uint32_t head)
{
  for (uint32_t n = 0; n < REQ_WAIT_SPINS; n++)
  {
    if ((int32_t)(__atomic_load_n(&q->acked, __ATOMIC_SEQ_CST) - head) >= 0)
    {
      return true;
    }
    if (fx_engine_wait_hook != NULL)
    {
      fx_engine_wait_hook();
    }
  }
  return false;
}

/* Main context. Hands the chain to the ISR and waits for it; falls back to
 * applying directly when audio is not running (or stopped consuming). */
static void apply_chain(const uint8_t ids[FX_SLOTS])
{
  if (!s_audio_running)
  {
    apply_structure(ids);
    return;
  }

  const uint32_t seq0 = s_req_seq;
  memcpy(s_req_ids, ids, FX_SLOTS);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  __atomic_store_n(&s_req_state, 1u, __ATOMIC_SEQ_CST);

  if (wait_for_seq(seq0))
  {
    return;
  }
  uint32_t expect = 1u;
  if (__atomic_compare_exchange_n(&s_req_state, &expect, 0u, false,
                                  __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
  {
    apply_structure(ids);
  }
  else
  {
    (void)wait_for_seq(seq0); /* the ISR already took it and is applying */
  }
}

/* ------------------------------------------------------------------------- */
/* Main-side configuration                                                   */
/* ------------------------------------------------------------------------- */

static float clamp_round(float v, float mn, float mx, float step)
{
  if (!(v > mn)) /* also catches NaN */
  {
    v = mn;
  }
  else if (v > mx)
  {
    v = mx;
  }
  if (step > 0.0f)
  {
    v = mn + floorf((v - mn) / step + 0.5f) * step;
    if (v > mx)
    {
      v = mx;
    }
  }
  return v;
}

static void sanitize_slot(const fx_slot_cfg_t *in, fx_slot_cfg_t *out)
{
  const fx_algo_id_t id = (fx_algo_id_t)in->algo;
  const fx_param_desc_t *pd = fx_algo_params(id);
  const uint32_t count = fx_algo_param_count(id);

  memset(out, 0, sizeof(*out));
  out->algo = in->algo;
  out->on = (in->on != 0u && id != FX_ALGO_NONE) ? 1u : 0u;
  for (uint32_t i = 0; i < count; i++)
  {
    out->params[i] = clamp_round(in->params[i], pd[i].min, pd[i].max, pd[i].step);
  }
}

static void default_slot(fx_slot_cfg_t *c, fx_algo_id_t id, bool on)
{
  const fx_param_desc_t *pd = fx_algo_params(id);
  memset(c, 0, sizeof(*c));
  c->algo = (uint8_t)id;
  c->on = (on && id != FX_ALGO_NONE) ? 1u : 0u;
  for (uint32_t i = 0; i < fx_algo_param_count(id); i++)
  {
    c->params[i] = pd[i].def;
  }
}

/* Validates and installs a whole chain; nothing changes on error. */
static fx_result_t install_chain(const fx_slot_cfg_t in[FX_SLOTS])
{
  fx_slot_cfg_t cfg[FX_SLOTS];
  uint8_t ids[FX_SLOTS];

  for (uint32_t j = 0; j < FX_SLOTS; j++)
  {
    if (in[j].algo >= FX_ALGO_COUNT)
    {
      return FX_ERR_RANGE;
    }
    sanitize_slot(&in[j], &cfg[j]);
    ids[j] = cfg[j].algo;
  }
  const fx_result_t res = check_chain(ids);
  if (res != FX_OK)
  {
    return res;
  }

  /* Ids first: the ISR then skips slots whose algorithm is about to change. */
  for (uint32_t j = 0; j < FX_SLOTS; j++)
  {
    s_main[j].algo = cfg[j].algo;
  }
  for (uint32_t j = 0; j < FX_SLOTS; j++)
  {
    for (uint32_t i = 0; i < FX_ALGO_MAX_PARAMS; i++)
    {
      s_main[j].params[i] = cfg[j].params[i];
    }
    s_main[j].on = cfg[j].on;
  }
  apply_chain(ids);
  return FX_OK;
}

static void main_ids(uint8_t ids[FX_SLOTS])
{
  for (uint32_t j = 0; j < FX_SLOTS; j++)
  {
    ids[j] = s_main[j].algo;
  }
}

static void main_snapshot(fx_slot_cfg_t out[FX_SLOTS])
{
  for (uint32_t j = 0; j < FX_SLOTS; j++)
  {
    out[j] = s_main[j];
  }
}

float fx_set_global(fx_param_id_t id, float value)
{
  if ((uint32_t)id >= (uint32_t)FXP_COUNT)
  {
    return 0.0f;
  }
  const fx_param_info_t *p = &g_fx_param_info[id];
  value = clamp_round(value, p->min, p->max, p->step);
  s_glob[id] = value;
  return value;
}

float fx_get_global(fx_param_id_t id)
{
  return (uint32_t)id < (uint32_t)FXP_COUNT ? s_glob[id] : 0.0f;
}

fx_result_t fx_slot_assign(uint32_t slot, fx_algo_id_t id)
{
  if (slot >= FX_SLOTS || (uint32_t)id >= FX_ALGO_COUNT)
  {
    return FX_ERR_RANGE;
  }
  fx_slot_cfg_t cfg[FX_SLOTS];
  main_snapshot(cfg);
  default_slot(&cfg[slot], id, true);
  return install_chain(cfg);
}

fx_result_t fx_slot_move(uint32_t from, uint32_t to)
{
  if (from >= FX_SLOTS || to >= FX_SLOTS)
  {
    return FX_ERR_RANGE;
  }
  fx_slot_cfg_t cur[FX_SLOTS];
  fx_slot_cfg_t cfg[FX_SLOTS];
  main_snapshot(cur);

  fx_slot_cfg_t rest[FX_SLOTS - 1u];
  uint32_t n = 0;
  for (uint32_t j = 0; j < FX_SLOTS; j++)
  {
    if (j != from)
    {
      rest[n++] = cur[j];
    }
  }
  for (uint32_t k = 0; k < FX_SLOTS; k++)
  {
    cfg[k] = (k < to) ? rest[k] : (k == to ? cur[from] : rest[k - 1u]);
  }
  return install_chain(cfg);
}

fx_result_t fx_slot_enable(uint32_t slot, bool on)
{
  if (slot >= FX_SLOTS)
  {
    return FX_ERR_RANGE;
  }
  s_main[slot].on = (on && s_main[slot].algo != FX_ALGO_NONE) ? 1u : 0u;
  return FX_OK;
}

fx_result_t fx_slot_set_param(uint32_t slot, uint32_t idx, float value, float *applied)
{
  if (slot >= FX_SLOTS)
  {
    return FX_ERR_RANGE;
  }
  const fx_algo_id_t id = (fx_algo_id_t)s_main[slot].algo;
  if (idx >= fx_algo_param_count(id))
  {
    return FX_ERR_RANGE;
  }
  const fx_param_desc_t *pd = &fx_algo_params(id)[idx];
  value = clamp_round(value, pd->min, pd->max, pd->step);
  s_main[slot].params[idx] = value;
  __atomic_fetch_or(&s_dirty, 1u << slot, __ATOMIC_SEQ_CST);
  if (applied != NULL)
  {
    *applied = value;
  }
  return FX_OK;
}

fx_algo_id_t fx_slot_algo(uint32_t slot)
{
  return slot < FX_SLOTS ? (fx_algo_id_t)s_main[slot].algo : FX_ALGO_NONE;
}

bool fx_slot_enabled(uint32_t slot)
{
  return slot < FX_SLOTS && s_main[slot].on != 0u;
}

float fx_slot_get_param(uint32_t slot, uint32_t idx)
{
  if (slot >= FX_SLOTS || idx >= fx_algo_param_count((fx_algo_id_t)s_main[slot].algo))
  {
    return 0.0f;
  }
  return s_main[slot].params[idx];
}

fx_result_t fx_slot_command(uint32_t slot, fx_cmd_t cmd)
{
  if (slot >= FX_SLOTS || (uint32_t)cmd >= FX_CMD_COUNT)
  {
    return FX_ERR_RANGE;
  }
  const fx_algo_t *a = fx_algo_get((fx_algo_id_t)s_main[slot].algo);
  if (a == NULL || a->command == NULL)
  {
    return FX_ERR_NO_CMD;
  }
  if (!s_audio_running)
  {
    if (s_live_id[slot] == s_main[slot].algo)
    {
      a->command(s_state[slot], cmd);
    }
    return FX_OK;
  }

  cmd_queue_t *q = &s_cmdq[slot];
  const uint32_t head = q->head;
  if (head - q->tail >= CMD_QUEUE_LEN)
  {
    return FX_ERR_RANGE; /* queue full: the ISR has not run for several commands */
  }
  q->q[head % CMD_QUEUE_LEN] = (uint8_t)cmd;
  __atomic_store_n(&q->head, head + 1u, __ATOMIC_SEQ_CST);
  /* On timeout the command stays queued and is delivered when the ISR resumes. */
  (void)wait_for_ack(q, head + 1u);
  return FX_OK;
}

fx_result_t fx_slot_status(uint32_t slot, fx_status_t *out)
{
  if (slot >= FX_SLOTS || out == NULL)
  {
    return FX_ERR_RANGE;
  }
  const fx_algo_t *a = fx_algo_get((fx_algo_id_t)s_main[slot].algo);
  if (a == NULL || a->get_status == NULL)
  {
    return FX_ERR_NO_CMD;
  }
  memset(out, 0, sizeof(*out));
  if (s_live_id[slot] == s_main[slot].algo) /* else a change is still pending */
  {
    a->get_status(s_state[slot], out);
  }
  return FX_OK;
}

void fx_get_config(fx_chain_cfg_t *out)
{
  memset(out, 0, sizeof(*out));
  for (uint32_t i = 0; i < FXP_COUNT; i++)
  {
    out->globals[i] = s_glob[i];
  }
  main_snapshot(out->slots);
  for (uint32_t j = 0; j < FX_SLOTS; j++)
  {
    memset(out->slots[j].reserved, 0, sizeof(out->slots[j].reserved));
  }
}

fx_result_t fx_apply_config(const fx_chain_cfg_t *cfg)
{
  const fx_result_t res = install_chain(cfg->slots);
  if (res == FX_OK)
  {
    for (uint32_t i = 0; i < FXP_COUNT; i++)
    {
      (void)fx_set_global((fx_param_id_t)i, cfg->globals[i]);
    }
  }
  return res;
}

void fx_load_defaults(void)
{
  fx_slot_cfg_t cfg[FX_SLOTS];
  default_slot(&cfg[0], FX_ALGO_OVERDRIVE, true);
  default_slot(&cfg[1], FX_ALGO_CHORUS, false);
  default_slot(&cfg[2], FX_ALGO_DIGITAL_DLY, true);
  default_slot(&cfg[3], FX_ALGO_HALL, true);
  for (uint32_t i = 0; i < FXP_COUNT; i++)
  {
    s_glob[i] = g_fx_param_info[i].def;
  }
  (void)install_chain(cfg);
}

uint32_t fx_mem_used(void)
{
  uint8_t ids[FX_SLOTS];
  layout_t lay;
  main_ids(ids);
  (void)place_chain(ids, true, &lay);
  return lay.used[ARENA_SRAM] + lay.used[ARENA_CCM];
}

uint32_t fx_mem_total(void)
{
  return ARENA_SRAM_BYTES + ARENA_CCM_BYTES;
}

uint32_t fx_cost_used(void)
{
  uint8_t ids[FX_SLOTS];
  main_ids(ids);
  return chain_cost(ids);
}

uint32_t fx_cost_budget(void)
{
  return CPU_BUDGET_CYCLES;
}

uint32_t fx_mem_free_largest(void)
{
  uint8_t ids[FX_SLOTS];
  layout_t lay;
  main_ids(ids);
  if (place_chain(ids, false, &lay) != FX_OK)
  {
    return 0u;
  }
  const uint32_t a = arena_free(&lay, ARENA_SRAM);
  const uint32_t b = arena_free(&lay, ARENA_CCM);
  return (a > b ? a : b) & ~7u;
}

/* Diagnostics for host tests: live arena block of a slot (NULL if none). */
const void *fx_engine_slot_mem(uint32_t slot, uint32_t *len)
{
  if (slot >= FX_SLOTS || s_rt[slot].blk.len == 0u)
  {
    return NULL;
  }
  *len = s_rt[slot].blk.len;
  return arena_base(s_rt[slot].blk.arena) + s_rt[slot].blk.off;
}

const void *fx_engine_arena(uint32_t arena, uint32_t *len)
{
  *len = k_arena_cap[arena];
  return arena_base(arena);
}

/* ------------------------------------------------------------------------- */
/* Initialisation                                                            */
/* ------------------------------------------------------------------------- */

void fx_init(void)
{
  memset(s_state, 0, sizeof(s_state));
  memset(s_rt, 0, sizeof(s_rt));
  memset(s_main, 0, sizeof(s_main));
  memset(s_cmdq, 0, sizeof(s_cmdq));
  memset(s_gs, 0, sizeof(s_gs));
  memset((void *)s_live_id, 0, sizeof(s_live_id));
  s_req_state = 0u;
  s_req_seq = 0u;
  s_dirty = 0u;
  s_audio_running = 0u;
  fx_engine_nan_count = 0u;

  fx_load_defaults();
  s_gs[GS_IN].cur = s_glob[FXP_IN_LEVEL];
  s_gs[GS_BYPASS].cur = s_glob[FXP_BYPASS];
  s_gs[GS_OUT].cur = s_glob[FXP_OUT_LEVEL];
}

/* ------------------------------------------------------------------------- */
/* Audio                                                                     */
/* ------------------------------------------------------------------------- */

static inline float smooth_next(smooth_t *s)
{
  const float d = s->tgt - s->cur;
  if (fabsf(d) < DSP_SMOOTH_SNAP)
  {
    s->cur = s->tgt;
  }
  else
  {
    s->cur += DSP_SMOOTH_K * d;
  }
  return s->cur;
}

static inline int16_t to_s16(float v)
{
  if (v != v)
  {
    fx_engine_nan_count++;
    v = 0.0f;
  }
  v = dsp_clampf(v, -32768.0f, 32767.0f);
  return (int16_t)(int32_t)(v + (v < 0.0f ? -0.5f : 0.5f));
}

static void deliver_commands(void)
{
  for (uint32_t j = 0; j < FX_SLOTS; j++)
  {
    cmd_queue_t *q = &s_cmdq[j];
    const uint32_t head = __atomic_load_n(&q->head, __ATOMIC_SEQ_CST);
    while (q->tail != head)
    {
      const fx_algo_t *a = s_rt[j].algo;
      if (a != NULL && a->command != NULL)
      {
        a->command(s_state[j], (fx_cmd_t)q->q[q->tail % CMD_QUEUE_LEN]);
      }
      q->tail++;
    }
  }
}

static void refresh_slots(void)
{
  const uint32_t dirty = __atomic_exchange_n(&s_dirty, 0u, __ATOMIC_SEQ_CST);

  for (uint32_t j = 0; j < FX_SLOTS; j++)
  {
    slot_rt_t *s = &s_rt[j];
    const volatile fx_slot_cfg_t *m = &s_main[j];
    if (s->algo == NULL || m->algo != s->id)
    {
      continue;
    }
    s->on_tgt = m->on ? 1.0f : 0.0f;
    if (dirty & (1u << j))
    {
      for (uint32_t i = 0; i < FX_ALGO_MAX_PARAMS; i++)
      {
        s->params[i] = m->params[i];
      }
      s->algo->update(s_state[j], s->params);
    }
  }
}

/* Off slots fade to dry. Tail algorithms keep running on silence after the
 * fade so their tail rings out on top of the dry signal. */
static void run_slot(uint32_t j, uint32_t n)
{
  slot_rt_t *s = &s_rt[j];
  const fx_algo_t *a = s->algo;
  void *st = s_state[j];

  const float g0 = s->g;
  const float step = (float)n * XFADE_PER_SAMPLE;
  const float g1 = g0 < s->on_tgt ? dsp_clampf(g0 + step, 0.0f, s->on_tgt)
                                  : dsp_clampf(g0 - step, s->on_tgt, 1.0f);
  s->g = g1;

  if (g0 >= 1.0f && g1 >= 1.0f)
  {
    a->process(st, s_l, s_r, n);
    return;
  }

  if (g1 <= 0.0f && g0 <= 0.0f)
  {
    if (a->tails == 0u)
    {
      return;
    }
    memcpy(s_tl, s_l, n * sizeof(float));
    memcpy(s_tr, s_r, n * sizeof(float));
    memset(s_l, 0, n * sizeof(float));
    memset(s_r, 0, n * sizeof(float));
    a->process(st, s_l, s_r, n);
    for (uint32_t i = 0; i < n; i++)
    {
      s_l[i] += s_tl[i];
      s_r[i] += s_tr[i];
    }
    return;
  }

  memcpy(s_tl, s_l, n * sizeof(float));
  memcpy(s_tr, s_r, n * sizeof(float));
  const float dg = (g1 - g0) / (float)n;

  if (a->tails != 0u)
  {
    for (uint32_t i = 0; i < n; i++)
    {
      const float g = g0 + dg * (float)(i + 1u);
      s_l[i] *= g;
      s_r[i] *= g;
    }
    a->process(st, s_l, s_r, n);
    for (uint32_t i = 0; i < n; i++)
    {
      const float g = g0 + dg * (float)(i + 1u);
      s_l[i] += s_tl[i] * (1.0f - g);
      s_r[i] += s_tr[i] * (1.0f - g);
    }
  }
  else
  {
    a->process(st, s_l, s_r, n);
    for (uint32_t i = 0; i < n; i++)
    {
      const float g = g0 + dg * (float)(i + 1u);
      s_l[i] = s_tl[i] + g * (s_l[i] - s_tl[i]);
      s_r[i] = s_tr[i] + g * (s_r[i] - s_tr[i]);
    }
  }
}

static void process_chunk(const int16_t *in, int16_t *out_lr, uint32_t n)
{
  for (uint32_t i = 0; i < n; i++)
  {
    const float x = (float)in[i] * (1.0f / 32768.0f) * smooth_next(&s_gs[GS_IN]);
    s_l[i] = x;
    s_r[i] = x;
  }

  for (uint32_t j = 0; j < FX_SLOTS; j++)
  {
    if (s_rt[j].algo != NULL)
    {
      run_slot(j, n);
    }
  }

  for (uint32_t i = 0; i < n; i++)
  {
    const float dry = (float)in[i] * (1.0f / 32768.0f);
    const float b = smooth_next(&s_gs[GS_BYPASS]);
    const float o = smooth_next(&s_gs[GS_OUT]) * 32768.0f;
    out_lr[2u * i] = to_s16((s_l[i] + b * (dry - s_l[i])) * o);
    out_lr[2u * i + 1u] = to_s16((s_r[i] + b * (dry - s_r[i])) * o);
  }
}

void fx_process(const int16_t *in, int16_t *out_lr, uint32_t frames)
{
  s_audio_running = 1u;
  consume_request();
  deliver_commands();
  refresh_slots();

  s_gs[GS_IN].tgt = s_glob[FXP_IN_LEVEL];
  s_gs[GS_BYPASS].tgt = s_glob[FXP_BYPASS];
  s_gs[GS_OUT].tgt = s_glob[FXP_OUT_LEVEL];

  while (frames > 0u)
  {
    const uint32_t n = frames < FX_BLOCK_FRAMES ? frames : FX_BLOCK_FRAMES;
    process_chunk(in, out_lr, n);
    in += n;
    out_lr += 2u * n;
    frames -= n;
  }

  for (uint32_t j = 0; j < FX_SLOTS; j++)
  {
    __atomic_store_n(&s_cmdq[j].acked, s_cmdq[j].tail, __ATOMIC_SEQ_CST);
  }
}
