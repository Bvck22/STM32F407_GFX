/*
 * stm32f4xx_hal_msp.c - Global HAL low-level initialization.
 * Peripheral MSP hooks live next to their users (audio_out.c, usbd_conf.c).
 */
#include "main.h"

void HAL_MspInit(void)
{
  __HAL_RCC_SYSCFG_CLK_ENABLE();
  __HAL_RCC_PWR_CLK_ENABLE();
  HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);
}
