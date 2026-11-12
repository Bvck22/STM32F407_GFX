#include "fx_params.h"

#include <string.h>

#define FXP_INFO_ENTRY(id, name, mn, mx, def, step) [id] = { name, mn, mx, def, step },
const fx_param_info_t g_fx_param_info[FXP_COUNT] = {
  FX_PARAM_LIST(FXP_INFO_ENTRY)
};
#undef FXP_INFO_ENTRY

int fx_param_find(const char *name)
{
  for (int i = 0; i < FXP_COUNT; i++)
  {
    if (strcmp(g_fx_param_info[i].name, name) == 0)
    {
      return i;
    }
  }
  return -1;
}
