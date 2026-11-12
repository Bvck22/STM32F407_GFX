/*
 * usb_device.h - USB CDC device bring-up.
 */
#ifndef USB_DEVICE_H
#define USB_DEVICE_H

#include "usbd_def.h"

extern USBD_HandleTypeDef hUsbDeviceFS;

/* Initializes the USB stack as a CDC-ACM device and starts it. */
void MX_USB_DEVICE_Init(void);

#endif /* USB_DEVICE_H */
