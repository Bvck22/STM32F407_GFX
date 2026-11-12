/*
 * usbd_desc.c - Device, string and serial-number descriptors.
 *
 * Device class 0x02 (CDC) lets the Windows 10 inbox usbser driver bind
 * without an INF file.
 */
#include "usbd_desc.h"

#include "usbd_conf.h"
#include "usbd_core.h"

#define USBD_VID                  0x0483
#define USBD_PID                  0x5740
#define USBD_LANGID_STRING        0x409
#define USBD_MANUFACTURER_STRING  "GFX"
#define USBD_PRODUCT_STRING       "GFX Guitar FX"
#define USBD_CONFIGURATION_STRING "CDC Config"
#define USBD_INTERFACE_STRING     "CDC Interface"

static uint8_t *device_descriptor(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *lang_id_descriptor(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *manufacturer_descriptor(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *product_descriptor(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *serial_descriptor(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *configuration_descriptor(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *interface_descriptor(USBD_SpeedTypeDef speed, uint16_t *length);

USBD_DescriptorsTypeDef FS_Desc = {
  device_descriptor,
  lang_id_descriptor,
  manufacturer_descriptor,
  product_descriptor,
  serial_descriptor,
  configuration_descriptor,
  interface_descriptor,
};

__ALIGN_BEGIN static uint8_t s_device_desc[USB_LEN_DEV_DESC] __ALIGN_END = {
  0x12,                       /* bLength */
  USB_DESC_TYPE_DEVICE,       /* bDescriptorType */
  0x00, 0x02,                 /* bcdUSB 2.00 */
  0x02,                       /* bDeviceClass: CDC */
  0x02,                       /* bDeviceSubClass */
  0x00,                       /* bDeviceProtocol */
  USB_MAX_EP0_SIZE,           /* bMaxPacketSize0 */
  LOBYTE(USBD_VID), HIBYTE(USBD_VID),
  LOBYTE(USBD_PID), HIBYTE(USBD_PID),
  0x00, 0x02,                 /* bcdDevice 2.00 */
  USBD_IDX_MFC_STR,
  USBD_IDX_PRODUCT_STR,
  USBD_IDX_SERIAL_STR,
  USBD_MAX_NUM_CONFIGURATION,
};

__ALIGN_BEGIN static uint8_t s_lang_id_desc[USB_LEN_LANGID_STR_DESC] __ALIGN_END = {
  USB_LEN_LANGID_STR_DESC,
  USB_DESC_TYPE_STRING,
  LOBYTE(USBD_LANGID_STRING),
  HIBYTE(USBD_LANGID_STRING),
};

__ALIGN_BEGIN static uint8_t s_serial_desc[USB_SIZ_STRING_SERIAL] __ALIGN_END = {
  USB_SIZ_STRING_SERIAL,
  USB_DESC_TYPE_STRING,
};

__ALIGN_BEGIN static uint8_t s_string_desc[USBD_MAX_STR_DESC_SIZ] __ALIGN_END;

static uint8_t *device_descriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
  (void)speed;
  *length = sizeof(s_device_desc);
  return s_device_desc;
}

static uint8_t *lang_id_descriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
  (void)speed;
  *length = sizeof(s_lang_id_desc);
  return s_lang_id_desc;
}

static uint8_t *string_descriptor(const char *text, uint16_t *length)
{
  USBD_GetString((uint8_t *)text, s_string_desc, length);
  return s_string_desc;
}

static uint8_t *manufacturer_descriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
  (void)speed;
  return string_descriptor(USBD_MANUFACTURER_STRING, length);
}

static uint8_t *product_descriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
  (void)speed;
  return string_descriptor(USBD_PRODUCT_STRING, length);
}

static uint8_t *configuration_descriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
  (void)speed;
  return string_descriptor(USBD_CONFIGURATION_STRING, length);
}

static uint8_t *interface_descriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
  (void)speed;
  return string_descriptor(USBD_INTERFACE_STRING, length);
}

/* Writes `digits` hex digits of value as UTF-16LE into out. */
static void hex_to_unicode(uint32_t value, uint8_t *out, uint8_t digits)
{
  for (uint8_t i = 0; i < digits; i++)
  {
    const uint8_t nibble = (uint8_t)(value >> 28);
    out[2U * i] = (nibble < 0xA) ? (uint8_t)('0' + nibble) : (uint8_t)('A' + nibble - 10);
    out[2U * i + 1U] = 0;
    value <<= 4;
  }
}

static uint8_t *serial_descriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
  (void)speed;
  *length = USB_SIZ_STRING_SERIAL;

  const uint32_t id0 = *(const uint32_t *)DEVICE_ID1 + *(const uint32_t *)DEVICE_ID3;
  const uint32_t id1 = *(const uint32_t *)DEVICE_ID2;
  if (id0 != 0U)
  {
    hex_to_unicode(id0, &s_serial_desc[2], 8);
    hex_to_unicode(id1, &s_serial_desc[18], 4);
  }
  return s_serial_desc;
}
