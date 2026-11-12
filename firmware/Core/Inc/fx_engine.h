/*
 * fx_engine.h - 4-slot effect chain.
 *
 *   in.level -> slot 0 -> slot 1 -> slot 2 -> slot 3 -> out.level -> int16 L/R
 *   (bypass = 1 crossfades the whole chain to the dry input x out.level)
 *
 * Each slot holds one algorithm from fx_algo_table.h (or Empty), an on/off
 * switch and up to FX_ALGO_MAX_PARAMS parameter values.
 *
 * Concurrency contract:
 *   - fx_process() runs in the audio DMA ISR and owns all DSP state.
 *   - Every other function runs in main-loop context. Parameter writes store a
 *     clamped value and raise a dirty flag consumed at the next block.
 *     Structural changes (assign, move, apply_config, defaults) are validated
 *     synchronously (memory arenas + CPU budget), then handed to the ISR, which
 *     applies them at the start of a block; the call returns once applied
 *     (or applies directly if audio is not running yet).
 *   - fx_init() must be called before the audio DMA is started.
 */
#ifndef FX_ENGINE_H
#define FX_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#include "fx_algo.h"
#include "fx_params.h"

#define FX_SLOTS 4u

typedef enum
{
  FX_OK = 0,
  FX_ERR_RANGE,     /* bad slot / param index / algorithm id */
  FX_ERR_MEMORY,    /* chain does not fit the RAM arenas */
  FX_ERR_CPU,       /* chain exceeds the CPU budget */
  FX_ERR_NO_CMD,    /* algorithm has no command()/get_status() */
} fx_result_t;

typedef struct
{
  uint8_t algo;     /* fx_algo_id_t */
  uint8_t on;
  uint8_t reserved[2];
  float params[FX_ALGO_MAX_PARAMS];
} fx_slot_cfg_t;

typedef struct
{
  float globals[FXP_COUNT];
  fx_slot_cfg_t slots[FX_SLOTS];
} fx_chain_cfg_t;

/* Clears all state and loads fx_load_defaults(). */
void fx_init(void);

/* in: mono int16, frames long. out_lr: interleaved stereo int16, 2*frames.
 * frames == FX_BLOCK_FRAMES. Audio DMA ISR only. */
void fx_process(const int16_t *in, int16_t *out_lr, uint32_t frames);

/* Globals (bypass, in.level, out.level). Return the stored (clamped) value. */
float fx_set_global(fx_param_id_t id, float value);
float fx_get_global(fx_param_id_t id);

/* Slots. Assigning resets the slot's params to the algorithm defaults and
 * switches it on (Empty: off). Move removes the slot at `from` and inserts it
 * at `to`, shifting the others. On error the chain is left unchanged. */
fx_result_t fx_slot_assign(uint32_t slot, fx_algo_id_t id);
fx_result_t fx_slot_move(uint32_t from, uint32_t to);
fx_result_t fx_slot_enable(uint32_t slot, bool on);
fx_result_t fx_slot_set_param(uint32_t slot, uint32_t idx, float value, float *applied);
fx_algo_id_t fx_slot_algo(uint32_t slot);
bool fx_slot_enabled(uint32_t slot);
float fx_slot_get_param(uint32_t slot, uint32_t idx);

/* Looper (or any algorithm exposing command/get_status). */
fx_result_t fx_slot_command(uint32_t slot, fx_cmd_t cmd);
fx_result_t fx_slot_status(uint32_t slot, fx_status_t *out);

/* Whole-chain snapshot, used by presets. apply validates first; on error the
 * chain is left unchanged. Params are clamped. */
void fx_get_config(fx_chain_cfg_t *out);
fx_result_t fx_apply_config(const fx_chain_cfg_t *cfg);

/* Globals to defaults; chain = Overdrive, Chorus (off), Digital Delay, Hall. */
void fx_load_defaults(void);

/* Resource report for STATUS / MEM. Bytes and cycles per frame. */
uint32_t fx_mem_used(void);
uint32_t fx_mem_total(void);
uint32_t fx_cost_used(void);
uint32_t fx_cost_budget(void);
/* Largest arena block a FX_MEM_REST algorithm would get right now, if added. */
uint32_t fx_mem_free_largest(void);

#endif /* FX_ENGINE_H */
