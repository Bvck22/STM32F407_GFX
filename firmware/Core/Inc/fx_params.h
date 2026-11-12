/*
 * fx_params.h - Global (chain-wide) parameters.
 *
 * Per-slot effect parameters live in fx_algo_table.h. The PC tool discovers
 * these globals at runtime via the STATE command.
 */
#ifndef FX_PARAMS_H
#define FX_PARAMS_H

#include <stdint.h>

/*  X(id,            name,           min,    max,    def,    step) */
#define FX_PARAM_LIST(X)                                               \
  X(FXP_BYPASS,      "bypass",       0.0f,   1.0f,   0.0f,   1.0f)     \
  X(FXP_IN_LEVEL,    "in.level",     0.0f,   2.0f,   1.0f,   0.01f)    \
  X(FXP_OUT_LEVEL,   "out.level",    0.0f,   1.0f,   0.7f,   0.01f)

#define FXP_ENUM_ENTRY(id, name, mn, mx, def, step) id,
typedef enum
{
  FX_PARAM_LIST(FXP_ENUM_ENTRY)
  FXP_COUNT
} fx_param_id_t;
#undef FXP_ENUM_ENTRY

typedef struct
{
  const char *name;
  float min;
  float max;
  float def;
  float step;
} fx_param_info_t;

extern const fx_param_info_t g_fx_param_info[FXP_COUNT];

/* Returns the parameter id for an exact name match, or -1 if unknown. */
int fx_param_find(const char *name);

#endif /* FX_PARAMS_H */
