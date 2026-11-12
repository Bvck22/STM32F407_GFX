/*
 * preset_store.h - 16 chain presets in internal flash (sector 11).
 *
 * The linker script keeps code and data out of sector 11. The sector holds one
 * CRC-protected image (header + 16 entries) followed by an append-only log of
 * "current preset" markers, so loading a preset persists the choice without an
 * erase. Saving or deleting rewrites the sector: audio is muted and the CPU
 * stalls for 1-2 s.
 */
#ifndef PRESET_STORE_H
#define PRESET_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "fx_engine.h"

#define PRESET_COUNT     16u
#define PRESET_NAME_MAX  15u

/* Mirrors the flash image and applies the last current preset, if any. */
void preset_init(void);

bool preset_used(uint32_t index);
const char *preset_name(uint32_t index);

/* Index of the last loaded/saved preset, or -1. */
int preset_current(void);

/* Stores the current chain config under name (<= PRESET_NAME_MAX chars). */
bool preset_save(uint32_t index, const char *name);

/* Applies a preset. FX_ERR_RANGE means the slot is empty/invalid; other errors
 * come from fx_apply_config(). */
fx_result_t preset_load(uint32_t index);

bool preset_delete(uint32_t index);

#endif /* PRESET_STORE_H */
