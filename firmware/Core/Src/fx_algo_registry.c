/*
 * fx_algo_registry.c - Binds the catalogue in fx_algo_table.h to the
 * implementations. Each algorithm source defines `const fx_algo_t g_fx_algo_<SUFFIX>`
 * (e.g. g_fx_algo_DIGITAL_DLY).
 */
#include "fx_algo.h"

#include <stddef.h>

#define FX_DECL_ALGO(num, sfx, name, cat) extern const fx_algo_t g_fx_algo_##sfx;
FX_ALGO_LIST(FX_DECL_ALGO)
#undef FX_DECL_ALGO

const fx_algo_t g_fx_algo_NONE = { .id = FX_ALGO_NONE };

#define FX_PARAM_ENTRY(k, mn, mx, df, st, un, lb) { #k, mn, mx, df, st, un, lb },
#define FX_PARAM_ARRAY(num, sfx, name, cat) \
  static const fx_param_desc_t s_params_##sfx[] = { FX_PARAMS_##sfx(FX_PARAM_ENTRY) { NULL, 0, 0, 0, 0, NULL, NULL } };
FX_ALGO_LIST(FX_PARAM_ARRAY)
#undef FX_PARAM_ARRAY
#undef FX_PARAM_ENTRY

typedef struct
{
  const char *name;
  fx_category_t category;
  const fx_param_desc_t *params;
  uint32_t param_count;
  const fx_algo_t *impl;
} fx_algo_entry_t;

/* The sentinel row in each params array keeps empty lists legal C. */
#define FX_ENTRY(num, sfx, nm, cat) \
  [num] = { nm, cat, s_params_##sfx, sizeof(s_params_##sfx) / sizeof(s_params_##sfx[0]) - 1u, &g_fx_algo_##sfx },
static const fx_algo_entry_t s_algos[FX_ALGO_COUNT] = {
  FX_ALGO_LIST(FX_ENTRY)
};
#undef FX_ENTRY

#define FX_ASSERT_PARAMS(num, sfx, name, cat) \
  _Static_assert(sizeof(s_params_##sfx) / sizeof(s_params_##sfx[0]) - 1u <= FX_ALGO_MAX_PARAMS, \
                 "too many params for " name);
FX_ALGO_LIST(FX_ASSERT_PARAMS)
#undef FX_ASSERT_PARAMS

static const char *const s_category_names[FX_CAT_COUNT] = FX_CATEGORY_NAMES;

static int valid(fx_algo_id_t id)
{
  return (uint32_t)id < FX_ALGO_COUNT;
}

const fx_algo_t *fx_algo_get(fx_algo_id_t id)
{
  return (valid(id) && id != FX_ALGO_NONE) ? s_algos[id].impl : NULL;
}

const char *fx_algo_name(fx_algo_id_t id)
{
  return valid(id) ? s_algos[id].name : "?";
}

fx_category_t fx_algo_category(fx_algo_id_t id)
{
  return valid(id) ? s_algos[id].category : FX_CAT_NONE;
}

uint32_t fx_algo_param_count(fx_algo_id_t id)
{
  return valid(id) ? s_algos[id].param_count : 0u;
}

const fx_param_desc_t *fx_algo_params(fx_algo_id_t id)
{
  return valid(id) ? s_algos[id].params : NULL;
}

const char *fx_category_name(fx_category_t cat)
{
  return ((uint32_t)cat < FX_CAT_COUNT) ? s_category_names[cat] : "?";
}
