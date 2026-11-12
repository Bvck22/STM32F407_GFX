/*
 * fx_algo.h - Interface every effect algorithm implements.
 *
 * Signal format: stereo float blocks, nominal range [-1, 1], processed in place.
 * Mono-natured effects (drive, EQ, compressor...) process L and R with
 * independent state. Effects with large buffers (delay, reverb, pitch) sum the
 * input to mono for their buffer and produce a stereo output, so memory is not
 * doubled.
 *
 * Ownership (see fx_engine.h): every function below runs in the audio ISR,
 * except get_status(), which runs in main-loop context and must only read
 * fields that are written atomically (aligned 32-bit).
 *
 * Memory:
 *   - Small state lives in a per-slot static area of FX_STATE_BYTES.
 *     Each algorithm asserts: _Static_assert(sizeof(my_state_t) <= FX_STATE_BYTES, ...)
 *   - Large buffers come from an arena block of mem_bytes (4-byte aligned),
 *     possibly in CCM RAM (CPU-only, no DMA). Contents are garbage on init():
 *     init() must clear whatever it uses.
 *   - mem_bytes == FX_MEM_REST: the algorithm takes the largest free arena
 *     block left after all other slots are placed (looper only). min_mem_bytes
 *     is the smallest block it accepts.
 */
#ifndef FX_ALGO_H
#define FX_ALGO_H

#include <stdint.h>

#include "fx_algo_table.h"

#define FX_SAMPLE_RATE  48000u
#define FX_BLOCK_FRAMES 256u
#define FX_STATE_BYTES  2048u
#define FX_MEM_REST     0xFFFFFFFFu

#define FX_ENUM_ENTRY(num, sfx, name, cat) FX_ALGO_##sfx = num,
typedef enum
{
  FX_ALGO_LIST(FX_ENUM_ENTRY)
  FX_ALGO_COUNT
} fx_algo_id_t;
#undef FX_ENUM_ENTRY

typedef struct
{
  const char *key;
  float min;
  float max;
  float def;
  float step;
  const char *unit;
  const char *labels;
} fx_param_desc_t;

/* Momentary actions (looper). */
typedef enum
{
  FX_CMD_REC = 0,   /* empty: start recording; recording: close loop and play */
  FX_CMD_DUB,       /* toggle overdub while playing */
  FX_CMD_PLAY,
  FX_CMD_STOP,
  FX_CMD_CLEAR,
  FX_CMD_COUNT
} fx_cmd_t;

typedef enum
{
  FX_LOOP_EMPTY = 0,
  FX_LOOP_RECORDING,
  FX_LOOP_PLAYING,
  FX_LOOP_OVERDUB,
  FX_LOOP_STOPPED
} fx_loop_state_t;

typedef struct
{
  uint32_t state;     /* fx_loop_state_t */
  uint32_t pos;       /* playback position, samples at 48 kHz scale */
  uint32_t len;       /* loop length, same unit */
  uint32_t max_len;   /* capacity, same unit */
} fx_status_t;

typedef struct
{
  fx_algo_id_t id;
  uint32_t mem_bytes;       /* arena bytes, 0 = none, FX_MEM_REST = remainder */
  uint32_t min_mem_bytes;   /* only for FX_MEM_REST */
  uint32_t cost;            /* worst-case CPU cycles per stereo frame, measured or estimated */
  uint32_t tails;           /* 1: output = dry*(1-mix) + wet, so feeding zeros yields
                               only the tail; the chain keeps it ringing when the slot is
                               switched off */

  /* Clear state and buffers. Called before the first update(). */
  void (*init)(void *state, void *mem, uint32_t mem_len);
  /* Recompute coefficients from parameter values (table order). May be called
   * every block; must be cheap when nothing changed or guarded by the caller. */
  void (*update)(void *state, const float *params);
  void (*process)(void *state, float *l, float *r, uint32_t n);
  /* Optional (NULL): momentary action. */
  void (*command)(void *state, fx_cmd_t cmd);
  /* Optional (NULL): main-loop status read. */
  void (*get_status)(const void *state, fx_status_t *out);
} fx_algo_t;

/* Catalogue lookups (fx_algo_registry.c). */
const fx_algo_t *fx_algo_get(fx_algo_id_t id);          /* NULL for FX_ALGO_NONE */
const char *fx_algo_name(fx_algo_id_t id);
fx_category_t fx_algo_category(fx_algo_id_t id);
uint32_t fx_algo_param_count(fx_algo_id_t id);
const fx_param_desc_t *fx_algo_params(fx_algo_id_t id);  /* array of param_count */
const char *fx_category_name(fx_category_t cat);

#endif /* FX_ALGO_H */
