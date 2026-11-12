/*
 * audio_out.h - 48 kHz / 16-bit stereo output to the on-board CS43L22 codec.
 *
 * I2S3 is fed by a circular DMA over a two-half ping-pong buffer. The DMA
 * half/full-transfer interrupts render the free half through fx_process().
 */
#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

#include <stdbool.h>
#include <stdint.h>

#define AUDIO_BLOCK_FRAMES 256u

/* Configures PLLI2S, I2S3, DMA and the codec. Returns false if the codec does
 * not answer on I2C. Call fx_init() before audio_out_start(). */
bool audio_out_init(void);

/* Pre-renders both halves, powers the codec and starts the circular DMA. */
void audio_out_start(void);

/* Every start rewinds the sample to its beginning. Stopping only mutes
 * the source, so delay/reverb tails keep ringing out. */
void audio_set_playing(bool playing);
bool audio_is_playing(void);

/* While muted the output is silence and the DSP is not run (used around flash
 * erase, which stalls the CPU). */
void audio_set_muted(bool muted);

/* DSP time of the last block relative to the block period, in percent. */
uint32_t audio_get_cpu_percent(void);

/* Highest absolute output sample since the previous call, 0..1; resets. */
float audio_take_peak(void);

/* True for a short while after the output reached full scale. */
bool audio_clip_active(void);

/* Index of the next sample to be played within the looped sample. */
uint32_t audio_get_position(void);

/* DMA1 Stream7 interrupt body (called from stm32f4xx_it.c). */
void audio_out_dma_irq(void);

#endif /* AUDIO_OUT_H */
