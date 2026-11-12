/*
 * main.c - GFX guitar effects: bring-up, board I/O and the main loop.
 *
 * Audio is rendered in the DMA interrupt (audio_out.c); USB parsing, the user
 * button and the LEDs are serviced here.
 */
#include "main.h"

#include <stdbool.h>

#include "audio_out.h"
#include "fx_engine.h"
#include "preset_store.h"
#include "stm32f4_discovery.h"
#include "usb_cmd.h"
#include "usb_device.h"
#include "usbd_cdc_if.h"

#define BUTTON_DEBOUNCE_MS   20u
#define BUTTON_LONG_MS       700u
#define REC_BLINK_HALF_MS    100u
#define USB_BLINK_HALF_MS    500u
#define ERROR_BLINK_LOOPS    2000000u /* a few Hz at any clock setting */

static void system_clock_config(void);
static void board_io_init(void);
static void poll_button(uint32_t now);
static void update_leds(uint32_t now);

int main(void)
{
  HAL_Init();
  system_clock_config();
  board_io_init();

  fx_init();
  preset_init();

  if (!audio_out_init())
  {
    Error_Handler();
  }
  audio_set_playing(true);
  audio_out_start();

  MX_USB_DEVICE_Init();

  for (;;)
  {
    const uint32_t now = HAL_GetTick();

    usb_cmd_poll();
    poll_button(now);
    update_leds(now);
  }
}

/* HSE 8 MHz -> PLL 336 MHz /2 = 168 MHz SYSCLK, /7 = 48 MHz for USB. */
static void system_clock_config(void)
{
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  RCC_OscInitTypeDef osc = {0};
  osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  osc.HSEState = RCC_HSE_ON;
  osc.PLL.PLLState = RCC_PLL_ON;
  osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  osc.PLL.PLLM = 8;
  osc.PLL.PLLN = 336;
  osc.PLL.PLLP = RCC_PLLP_DIV2;
  osc.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&osc) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_ClkInitTypeDef clk = {0};
  clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                  RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clk.APB1CLKDivider = RCC_HCLK_DIV4;
  clk.APB2CLKDivider = RCC_HCLK_DIV2;
  if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

static void board_io_init(void)
{
  BSP_LED_Init(LED4);
  BSP_LED_Init(LED3);
  BSP_LED_Init(LED5);
  BSP_LED_Init(LED6);
  BSP_PB_Init(BUTTON_KEY, BUTTON_MODE_GPIO);
}

static void toggle_playback(void)
{
  const bool playing = !audio_is_playing();

  audio_set_playing(playing);
  usb_cmd_notify_play(playing);
}

/* Short press: play/stop. Long press: looper record, fired while still held. */
static void poll_button(uint32_t now)
{
  static bool raw;
  static bool stable;
  static bool long_fired;
  static uint32_t changed_at;

  const bool pressed = BSP_PB_GetState(BUTTON_KEY) != 0u;
  if (pressed != raw)
  {
    raw = pressed;
    changed_at = now;
    return;
  }

  const uint32_t held = now - changed_at;
  if (raw != stable && held >= BUTTON_DEBOUNCE_MS)
  {
    stable = raw;
    if (!stable)
    {
      if (!long_fired)
      {
        toggle_playback();
      }
      long_fired = false;
    }
  }
  else if (stable && !long_fired && held >= BUTTON_LONG_MS)
  {
    long_fired = true;
    usb_cmd_looper_rec();
  }
}

static void set_led(Led_TypeDef led, bool on)
{
  if (on)
  {
    BSP_LED_On(led);
  }
  else
  {
    BSP_LED_Off(led);
  }
}

static void update_leds(uint32_t now)
{
  const bool usb_blink_on = (now / USB_BLINK_HALF_MS) % 2u == 0u;
  const bool rec_blink_on = (now / REC_BLINK_HALF_MS) % 2u == 0u;

  set_led(LED4, usb_cmd_looper_recording() ? rec_blink_on : audio_is_playing());
  set_led(LED3, fx_get_global(FXP_BYPASS) >= 0.5f);
  set_led(LED5, audio_clip_active());
  set_led(LED6, CDC_IsConfigured() || usb_blink_on);
}

void Error_Handler(void)
{
  __disable_irq();
  BSP_LED_Init(LED5);
  for (;;)
  {
    BSP_LED_Toggle(LED5);
    for (volatile uint32_t i = 0; i < ERROR_BLINK_LOOPS; i++)
    {
    }
  }
}
