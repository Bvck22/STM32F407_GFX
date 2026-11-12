/*
 * stm32f4xx_it.c - Interrupt handlers.
 */
#include "stm32f4xx_it.h"

#include "audio_out.h"
#include "main.h"
#include "usbd_conf.h"

/* Faults are unrecoverable: stop with the red LED blinking. */
void NMI_Handler(void)
{
  Error_Handler();
}

void HardFault_Handler(void)
{
  Error_Handler();
}

void MemManage_Handler(void)
{
  Error_Handler();
}

void BusFault_Handler(void)
{
  Error_Handler();
}

void UsageFault_Handler(void)
{
  Error_Handler();
}

void SVC_Handler(void)
{
}

void DebugMon_Handler(void)
{
}

void PendSV_Handler(void)
{
}

void SysTick_Handler(void)
{
  HAL_IncTick();
}

void DMA1_Stream7_IRQHandler(void)
{
  audio_out_dma_irq();
}

void OTG_FS_IRQHandler(void)
{
  HAL_PCD_IRQHandler(&hpcd_USB_OTG_FS);
}
