/*
 * audio_out.c - I2S3 + circular DMA + CS43L22 audio output (see audio_out.h).
 *
 * The BSP audio driver is deliberately not used: it runs the DMA in normal
 * mode and drags in the PDM library. Codec control still goes through the BSP
 * cs43l22 component and the AUDIO_IO_* functions of stm32f4_discovery.c.
 */
#include "audio_out.h"

#include <string.h>

#include "cs43l22.h"
#include "fx_engine.h"
#include "main.h"
#include "sample_data.h"
#include "scope.h"
#include "stm32f4_discovery.h"

#define SAMPLE_RATE_HZ     48000u
#define CODEC_VOLUME       75u
#define DMA_IRQ_PRIORITY   6u

/* PLLI2S for 48 kHz from the 1 MHz VCO input: 1 MHz * 258 / 3 = 86 MHz. */
#define PLLI2S_N_48K       258u
#define PLLI2S_R_48K       3u

#define HALF_SAMPLES       (AUDIO_BLOCK_FRAMES * 2u)  /* interleaved L/R */
#define BLOCK_CYCLES       ((SYSCLK_HZ / SAMPLE_RATE_HZ) * AUDIO_BLOCK_FRAMES)

#define FULL_SCALE         32768
#define CLIP_THRESHOLD     ((int32_t)(0.99f * FULL_SCALE))
#define CLIP_HOLD_MS       200u

static I2S_HandleTypeDef s_hi2s3;
static DMA_HandleTypeDef s_hdma_tx;

static int16_t s_dma_buf[2][HALF_SAMPLES];
static int16_t s_input[AUDIO_BLOCK_FRAMES];

static volatile bool s_playing;
static volatile bool s_muted;
static volatile uint32_t s_position;
static volatile uint32_t s_last_cycles;
static volatile int32_t s_peak;
static volatile uint32_t s_clip_tick;
static volatile bool s_clip_seen;

void HAL_I2S_MspInit(I2S_HandleTypeDef *hi2s)
{
  if (hi2s->Instance != SPI3)
  {
    return;
  }

  __HAL_RCC_SPI3_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_DMA1_CLK_ENABLE();

  GPIO_InitTypeDef gpio = {0};
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  gpio.Alternate = GPIO_AF6_SPI3;

  gpio.Pin = GPIO_PIN_4; /* WS */
  HAL_GPIO_Init(GPIOA, &gpio);
  gpio.Pin = GPIO_PIN_7 | GPIO_PIN_10 | GPIO_PIN_12; /* MCK, SCK, SD */
  HAL_GPIO_Init(GPIOC, &gpio);

  s_hdma_tx.Instance = DMA1_Stream7;
  s_hdma_tx.Init.Channel = DMA_CHANNEL_0;
  s_hdma_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;
  s_hdma_tx.Init.PeriphInc = DMA_PINC_DISABLE;
  s_hdma_tx.Init.MemInc = DMA_MINC_ENABLE;
  s_hdma_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
  s_hdma_tx.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
  s_hdma_tx.Init.Mode = DMA_CIRCULAR;
  s_hdma_tx.Init.Priority = DMA_PRIORITY_HIGH;
  s_hdma_tx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
  if (HAL_DMA_Init(&s_hdma_tx) != HAL_OK)
  {
    Error_Handler();
  }
  __HAL_LINKDMA(hi2s, hdmatx, s_hdma_tx);

  HAL_NVIC_SetPriority(DMA1_Stream7_IRQn, DMA_IRQ_PRIORITY, 0);
  HAL_NVIC_EnableIRQ(DMA1_Stream7_IRQn);
}

void audio_out_dma_irq(void)
{
  HAL_DMA_IRQHandler(s_hi2s3.hdmatx);
}

static void enable_cycle_counter(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static void configure_i2s_clock(void)
{
  RCC_PeriphCLKInitTypeDef clk = {0};

  clk.PeriphClockSelection = RCC_PERIPHCLK_I2S;
  clk.PLLI2S.PLLI2SN = PLLI2S_N_48K;
  clk.PLLI2S.PLLI2SR = PLLI2S_R_48K;
  if (HAL_RCCEx_PeriphCLKConfig(&clk) != HAL_OK)
  {
    Error_Handler();
  }
}

static void configure_i2s(void)
{
  s_hi2s3.Instance = SPI3;
  s_hi2s3.Init.Mode = I2S_MODE_MASTER_TX;
  s_hi2s3.Init.Standard = I2S_STANDARD_PHILIPS;
  s_hi2s3.Init.DataFormat = I2S_DATAFORMAT_16B;
  s_hi2s3.Init.MCLKOutput = I2S_MCLKOUTPUT_ENABLE;
  s_hi2s3.Init.AudioFreq = SAMPLE_RATE_HZ;
  s_hi2s3.Init.CPOL = I2S_CPOL_LOW;
  s_hi2s3.Init.ClockSource = I2S_CLOCK_PLL;
  if (HAL_I2S_Init(&s_hi2s3) != HAL_OK)
  {
    Error_Handler();
  }
}

bool audio_out_init(void)
{
  enable_cycle_counter();
  configure_i2s_clock();
  configure_i2s();

  if ((cs43l22_drv.ReadID(AUDIO_I2C_ADDRESS) & CS43L22_ID_MASK) != CS43L22_ID)
  {
    return false;
  }
  return cs43l22_drv.Init(AUDIO_I2C_ADDRESS, OUTPUT_DEVICE_AUTO, CODEC_VOLUME, SAMPLE_RATE_HZ) == 0u;
}

/* The mono source is looped from flash; silence while stopped. */
static void fill_input(int16_t *in)
{
  if (!s_playing || g_sample_len == 0u)
  {
    memset(in, 0, AUDIO_BLOCK_FRAMES * sizeof(in[0]));
    return;
  }

  uint32_t pos = s_position;
  for (uint32_t i = 0; i < AUDIO_BLOCK_FRAMES; i++)
  {
    in[i] = g_sample_data[pos];
    if (++pos >= g_sample_len)
    {
      pos = 0;
    }
  }
  s_position = pos;
}

static int32_t block_peak(const int16_t *out)
{
  int32_t peak = 0;

  for (uint32_t i = 0; i < HALF_SAMPLES; i++)
  {
    const int32_t v = out[i];
    const int32_t mag = v < 0 ? -v : v;
    if (mag > peak)
    {
      peak = mag;
    }
  }
  return peak;
}

static void track_peak(const int16_t *out)
{
  const int32_t peak = block_peak(out);

  if (peak > s_peak)
  {
    s_peak = peak;
  }
  if (peak >= CLIP_THRESHOLD)
  {
    s_clip_tick = HAL_GetTick();
    s_clip_seen = true;
  }
}

static void render_half(int16_t *out)
{
  if (s_muted)
  {
    memset(out, 0, HALF_SAMPLES * sizeof(out[0]));
    return;
  }

  fill_input(s_input);

  const uint32_t t0 = DWT->CYCCNT;
  fx_process(s_input, out, AUDIO_BLOCK_FRAMES);
  s_last_cycles = DWT->CYCCNT - t0;

  scope_feed(s_input, out, AUDIO_BLOCK_FRAMES);

  track_peak(out);
}

/* The DMA is reading the other half, so the completed one is free. */
void HAL_I2S_TxHalfCpltCallback(I2S_HandleTypeDef *hi2s)
{
  (void)hi2s;
  render_half(s_dma_buf[0]);
}

void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef *hi2s)
{
  (void)hi2s;
  render_half(s_dma_buf[1]);
}

void audio_out_start(void)
{
  render_half(s_dma_buf[0]);
  render_half(s_dma_buf[1]);

  if (cs43l22_drv.Play(AUDIO_I2C_ADDRESS, NULL, 0) != 0u ||
      HAL_I2S_Transmit_DMA(&s_hi2s3, (uint16_t *)s_dma_buf, 2u * HALF_SAMPLES) != HAL_OK)
  {
    Error_Handler();
  }
}

void audio_set_playing(bool playing)
{
  if (playing)
  {
    s_position = 0;
  }
  s_playing = playing;
}

bool audio_is_playing(void)
{
  return s_playing;
}

void audio_set_muted(bool muted)
{
  s_muted = muted;
  if (muted)
  {
    memset(s_dma_buf, 0, sizeof(s_dma_buf));
  }
}

uint32_t audio_get_cpu_percent(void)
{
  return (s_last_cycles * 100u + BLOCK_CYCLES / 2u) / BLOCK_CYCLES;
}

float audio_take_peak(void)
{
  const uint32_t primask = __get_PRIMASK();
  __disable_irq();
  const int32_t peak = s_peak;
  s_peak = 0;
  __set_PRIMASK(primask);

  return (float)peak / (float)FULL_SCALE;
}

bool audio_clip_active(void)
{
  return s_clip_seen && (HAL_GetTick() - s_clip_tick) < CLIP_HOLD_MS;
}

uint32_t audio_get_position(void)
{
  return s_position;
}
