/*
 * preset_store.c - Chain presets in flash sector 11 (see preset_store.h).
 */
#include "preset_store.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "audio_out.h"
#include "main.h"

#define SECTOR_ADDRESS   0x080E0000u
#define SECTOR_SIZE      0x20000u
#define SECTOR_ID        FLASH_SECTOR_11
#define PRESET_MAGIC     0x47465850u /* "GFXP" */
#define PRESET_VERSION   2u          /* version 1 (flat parameter list) is rejected */

#define MARKER_TAG       0xC0DE0000u
#define MARKER_NONE      0xFFFFu
#define ERASED_WORD      0xFFFFFFFFu

typedef struct
{
  uint32_t used;
  char name[PRESET_NAME_MAX + 1u];
  fx_chain_cfg_t cfg;
} preset_entry_t;

typedef struct
{
  uint32_t magic;
  uint32_t version;
  uint32_t layout;   /* sizeof(preset_image_t): guards against struct changes */
  int32_t current;
  preset_entry_t entries[PRESET_COUNT];
  uint32_t crc;
} preset_image_t;

_Static_assert(sizeof(preset_image_t) % 4u == 0u, "image is programmed in words");
_Static_assert(sizeof(preset_image_t) < SECTOR_SIZE, "image must fit the sector");

static preset_image_t s_image;
static int s_current = -1;

/* CRC-32 (IEEE 802.3, reflected), bitwise: tiny and only run on save/load. */
static uint32_t crc32(const void *data, uint32_t len)
{
  const uint8_t *p = data;
  uint32_t crc = 0xFFFFFFFFu;

  while (len--)
  {
    crc ^= *p++;
    for (int bit = 0; bit < 8; bit++)
    {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

static uint32_t image_crc(const preset_image_t *img)
{
  return crc32(img, offsetof(preset_image_t, crc));
}

static const preset_image_t *flash_image(void)
{
  return (const preset_image_t *)SECTOR_ADDRESS;
}

static const uint32_t *marker_log(void)
{
  return (const uint32_t *)(SECTOR_ADDRESS + sizeof(preset_image_t));
}

static uint32_t marker_capacity(void)
{
  return (SECTOR_SIZE - sizeof(preset_image_t)) / 4u;
}

static bool image_valid(const preset_image_t *img)
{
  return img->magic == PRESET_MAGIC && img->version == PRESET_VERSION &&
         img->layout == sizeof(preset_image_t) && img->crc == image_crc(img);
}

static bool index_ok(uint32_t index)
{
  return index < PRESET_COUNT;
}

/* The newest marker wins; the log ends at the first erased word. */
static int current_from_log(int fallback)
{
  int current = fallback;

  for (uint32_t i = 0; i < marker_capacity() && marker_log()[i] != ERASED_WORD; i++)
  {
    const uint32_t w = marker_log()[i];
    if ((w & 0xFFFF0000u) == MARKER_TAG)
    {
      const uint32_t idx = w & 0xFFFFu;
      current = idx < PRESET_COUNT ? (int)idx : -1;
    }
  }
  return current;
}

static void mirror_flash(void)
{
  if (image_valid(flash_image()))
  {
    memcpy(&s_image, flash_image(), sizeof(s_image));
    s_current = current_from_log(s_image.current);
  }
  else
  {
    memset(&s_image, 0, sizeof(s_image));
    s_current = -1;
  }
}

static bool erase_sector(void)
{
  FLASH_EraseInitTypeDef erase = {0};
  uint32_t sector_error = 0;

  erase.TypeErase = FLASH_TYPEERASE_SECTORS;
  erase.Sector = SECTOR_ID;
  erase.NbSectors = 1;
  erase.VoltageRange = FLASH_VOLTAGE_RANGE_3; /* 2.7-3.6 V: word programming */
  return HAL_FLASHEx_Erase(&erase, &sector_error) == HAL_OK;
}

static bool program_word(uint32_t address, uint32_t word)
{
  return HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address, word) == HAL_OK;
}

static bool program_image(const preset_image_t *img)
{
  const uint32_t *words = (const uint32_t *)img;

  for (uint32_t i = 0; i < sizeof(*img) / 4u; i++)
  {
    if (!program_word(SECTOR_ADDRESS + 4u * i, words[i]))
    {
      return false;
    }
  }
  return true;
}

/* Rewrites the whole sector from s_image (with s_current as the header value). */
static bool commit_image(void)
{
  s_image.magic = PRESET_MAGIC;
  s_image.version = PRESET_VERSION;
  s_image.layout = sizeof(s_image);
  s_image.current = s_current;
  s_image.crc = image_crc(&s_image);

  audio_set_muted(true);
  HAL_FLASH_Unlock();
  const bool written = erase_sector() && program_image(&s_image);
  HAL_FLASH_Lock();
  audio_set_muted(false);

  const bool ok = written && memcmp(flash_image(), &s_image, sizeof(s_image)) == 0;
  if (!ok)
  {
    mirror_flash();
  }
  return ok;
}

/* Appends a "current preset" marker; a full log just loses persistence. */
static void log_current(int index)
{
  for (uint32_t i = 0; i < marker_capacity(); i++)
  {
    if (marker_log()[i] == ERASED_WORD)
    {
      HAL_FLASH_Unlock();
      program_word((uint32_t)&marker_log()[i], MARKER_TAG | (index < 0 ? MARKER_NONE : (uint32_t)index));
      HAL_FLASH_Lock();
      return;
    }
  }
}

void preset_init(void)
{
  mirror_flash();
  if (s_current >= 0 && s_image.entries[s_current].used != 0u)
  {
    fx_apply_config(&s_image.entries[s_current].cfg);
  }
}

bool preset_used(uint32_t index)
{
  return index_ok(index) && s_image.entries[index].used != 0u;
}

const char *preset_name(uint32_t index)
{
  return index_ok(index) ? s_image.entries[index].name : "";
}

int preset_current(void)
{
  return s_current;
}

bool preset_save(uint32_t index, const char *name)
{
  if (!index_ok(index) || strlen(name) > PRESET_NAME_MAX)
  {
    return false;
  }

  preset_entry_t *e = &s_image.entries[index];
  e->used = 1u;
  memset(e->name, 0, sizeof(e->name));
  strcpy(e->name, name);
  fx_get_config(&e->cfg);
  s_current = (int)index;
  return commit_image();
}

fx_result_t preset_load(uint32_t index)
{
  if (!preset_used(index))
  {
    return FX_ERR_RANGE;
  }

  const fx_result_t result = fx_apply_config(&s_image.entries[index].cfg);
  if (result == FX_OK && s_current != (int)index)
  {
    s_current = (int)index;
    log_current(s_current);
  }
  return result;
}

bool preset_delete(uint32_t index)
{
  if (!index_ok(index))
  {
    return false;
  }

  memset(&s_image.entries[index], 0, sizeof(s_image.entries[index]));
  if (s_current == (int)index)
  {
    s_current = -1;
  }
  return commit_image();
}
