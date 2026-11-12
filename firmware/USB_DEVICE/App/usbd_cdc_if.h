/*
 * usbd_cdc_if.h - CDC-ACM byte-stream interface.
 *
 * RX: the USB interrupt only pushes received bytes into a ring buffer;
 * CDC_Read() drains it from the main loop.
 * TX: CDC_Write() queues bytes in a ring buffer; CDC_Poll() hands them to the
 * USB stack in the main loop whenever the previous transfer has completed.
 */
#ifndef USBD_CDC_IF_H
#define USBD_CDC_IF_H

#include <stdbool.h>
#include <stdint.h>

#include "usbd_cdc.h"

extern USBD_CDC_ItfTypeDef USBD_Interface_fops_FS;

/* Copies up to max received bytes into dst; returns the count. */
uint32_t CDC_Read(uint8_t *dst, uint32_t max);

/* Queues bytes for transmission. Bytes that do not fit, or that are written
 * while the host has not configured the device, are dropped. Returns the
 * number of bytes queued. */
uint32_t CDC_Write(const uint8_t *src, uint32_t len);

/* Free space in the TX queue, in bytes. */
uint32_t CDC_TxFree(void);

/* Starts the next USB transfer if the endpoint is idle and data is queued. */
void CDC_Poll(void);

bool CDC_IsConfigured(void);

#endif /* USBD_CDC_IF_H */
