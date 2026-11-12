/*
 * stm32f4xx_it.h - Interrupt handlers.
 */
#ifndef STM32F4XX_IT_H
#define STM32F4XX_IT_H

void NMI_Handler(void);
void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);
void SVC_Handler(void);
void DebugMon_Handler(void);
void PendSV_Handler(void);
void SysTick_Handler(void);
void DMA1_Stream7_IRQHandler(void);
void OTG_FS_IRQHandler(void);

#endif /* STM32F4XX_IT_H */
