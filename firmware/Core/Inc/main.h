/*
 * main.h - Application-wide definitions.
 */
#ifndef MAIN_H
#define MAIN_H

#include "stm32f4xx_hal.h"

#define SYSCLK_HZ 168000000u

/* Halts with the red LED blinking; used for unrecoverable setup failures. */
void Error_Handler(void);

#endif /* MAIN_H */
