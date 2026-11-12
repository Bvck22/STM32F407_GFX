/*
 * usbd_cdc_if.c - CDC-ACM byte-stream interface (see usbd_cdc_if.h).
 */
#include "usbd_cdc_if.h"

#include "usb_device.h"

#define RX_RING_SIZE     256U   /* power of two */
#define TX_RING_SIZE     2048U  /* power of two; long replies are streamed (usb_cmd.c) */
#define TX_STAGE_SIZE    256U   /* one USB transfer; ZLP handled by the class */
#define RX_PACKET_SIZE   CDC_DATA_FS_MAX_PACKET_SIZE

/* Single-producer / single-consumer byte ring; size must be a power of two. */
typedef struct
{
  volatile uint32_t head; /* next write index (producer) */
  volatile uint32_t tail; /* next read index (consumer) */
  uint8_t *data;
  uint32_t size;
} ring_t;

static uint8_t s_rx_data[RX_RING_SIZE];
static uint8_t s_tx_data[TX_RING_SIZE];
static ring_t s_rx_ring = { 0, 0, s_rx_data, RX_RING_SIZE };
static ring_t s_tx_ring = { 0, 0, s_tx_data, TX_RING_SIZE };

static uint8_t s_rx_packet[RX_PACKET_SIZE];
static uint8_t s_tx_stage[TX_STAGE_SIZE];

static USBD_CDC_LineCodingTypeDef s_line_coding = { 115200, 0x00, 0x00, 0x08 };

static uint32_t ring_count(const ring_t *r)
{
  return (r->head - r->tail) & (r->size - 1U);
}

static uint32_t ring_free(const ring_t *r)
{
  return r->size - 1U - ring_count(r);
}

static void ring_put(ring_t *r, uint8_t byte)
{
  r->data[r->head] = byte;
  r->head = (r->head + 1U) & (r->size - 1U);
}

static uint8_t ring_get(ring_t *r)
{
  const uint8_t byte = r->data[r->tail];
  r->tail = (r->tail + 1U) & (r->size - 1U);
  return byte;
}

static int8_t cdc_init(void)
{
  USBD_CDC_SetTxBuffer(&hUsbDeviceFS, s_tx_stage, 0);
  USBD_CDC_SetRxBuffer(&hUsbDeviceFS, s_rx_packet);
  return (int8_t)USBD_OK;
}

static int8_t cdc_deinit(void)
{
  return (int8_t)USBD_OK;
}

static void line_coding_to_bytes(uint8_t *pbuf)
{
  pbuf[0] = (uint8_t)(s_line_coding.bitrate);
  pbuf[1] = (uint8_t)(s_line_coding.bitrate >> 8);
  pbuf[2] = (uint8_t)(s_line_coding.bitrate >> 16);
  pbuf[3] = (uint8_t)(s_line_coding.bitrate >> 24);
  pbuf[4] = s_line_coding.format;
  pbuf[5] = s_line_coding.paritytype;
  pbuf[6] = s_line_coding.datatype;
}

static void line_coding_from_bytes(const uint8_t *pbuf)
{
  s_line_coding.bitrate = (uint32_t)pbuf[0] | ((uint32_t)pbuf[1] << 8) |
                          ((uint32_t)pbuf[2] << 16) | ((uint32_t)pbuf[3] << 24);
  s_line_coding.format = pbuf[4];
  s_line_coding.paritytype = pbuf[5];
  s_line_coding.datatype = pbuf[6];
}

/* The baud rate is only echoed back to the host; the link is USB-native. */
static int8_t cdc_control(uint8_t cmd, uint8_t *pbuf, uint16_t length)
{
  (void)length;
  switch (cmd)
  {
    case CDC_SET_LINE_CODING:
      line_coding_from_bytes(pbuf);
      break;
    case CDC_GET_LINE_CODING:
      line_coding_to_bytes(pbuf);
      break;
    default:
      break;
  }
  return (int8_t)USBD_OK;
}

/* Runs in the USB interrupt: only queue the bytes and re-arm the endpoint. */
static int8_t cdc_receive(uint8_t *buf, uint32_t *len)
{
  for (uint32_t i = 0; i < *len; i++)
  {
    if (ring_free(&s_rx_ring) == 0U)
    {
      break;
    }
    ring_put(&s_rx_ring, buf[i]);
  }
  USBD_CDC_ReceivePacket(&hUsbDeviceFS);
  return (int8_t)USBD_OK;
}

USBD_CDC_ItfTypeDef USBD_Interface_fops_FS = {
  cdc_init,
  cdc_deinit,
  cdc_control,
  cdc_receive,
  NULL, /* TransmitCplt: the TX state flag in the class is all we need */
};

uint32_t CDC_Read(uint8_t *dst, uint32_t max)
{
  uint32_t n = 0;

  while (n < max && ring_count(&s_rx_ring) > 0U)
  {
    dst[n++] = ring_get(&s_rx_ring);
  }
  return n;
}

bool CDC_IsConfigured(void)
{
  return hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED;
}

uint32_t CDC_Write(const uint8_t *src, uint32_t len)
{
  if (!CDC_IsConfigured())
  {
    return 0;
  }

  uint32_t n = 0;
  while (n < len && ring_free(&s_tx_ring) > 0U)
  {
    ring_put(&s_tx_ring, src[n++]);
  }
  return n;
}

uint32_t CDC_TxFree(void)
{
  return ring_free(&s_tx_ring);
}

static bool tx_busy(void)
{
  const USBD_CDC_HandleTypeDef *hcdc =
      (const USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassDataCmsit[hUsbDeviceFS.classId];

  return hcdc == NULL || hcdc->TxState != 0U;
}

void CDC_Poll(void)
{
  if (ring_count(&s_tx_ring) == 0U || !CDC_IsConfigured() || tx_busy())
  {
    return;
  }

  uint16_t n = 0;
  while (n < TX_STAGE_SIZE && ring_count(&s_tx_ring) > 0U)
  {
    s_tx_stage[n++] = ring_get(&s_tx_ring);
  }

  USBD_CDC_SetTxBuffer(&hUsbDeviceFS, s_tx_stage, n);
  (void)USBD_CDC_TransmitPacket(&hUsbDeviceFS);
}
