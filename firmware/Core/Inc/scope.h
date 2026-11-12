/*
 * scope.h - Waveform capture for the PC tool.
 *
 * 256 points on three channels (in = mono source before in.level, l, r =
 * final output). Each point holds the min and max of `decim` consecutive
 * samples, stored as G.711 mu-law bytes (min, max per point).
 *
 * State machine: IDLE -> ARMED (waiting for trigger) -> CAPTURING -> READY.
 * scope_feed() runs in the audio ISR and stops writing at READY; every other
 * function runs in the main loop. The data stays valid until the next arm.
 */
#ifndef SCOPE_H
#define SCOPE_H

#include <stdbool.h>
#include <stdint.h>

#define SCOPE_POINTS         256u
#define SCOPE_CHANNELS       3u
#define SCOPE_MIN_WINDOW_MS  5u
#define SCOPE_MAX_WINDOW_MS  5000u

typedef enum
{
  SCOPE_IDLE = 0,
  SCOPE_ARMED,
  SCOPE_CAPTURING,
  SCOPE_READY
} scope_state_t;

typedef enum
{
  SCOPE_CH_IN = 0,
  SCOPE_CH_L,
  SCOPE_CH_R
} scope_channel_t;

/* Audio ISR, once per block after fx_process(). */
void scope_feed(const int16_t *in_mono, const int16_t *out_lr, uint32_t frames);

/* Starts a capture (any previous one is discarded). window_ms is clamped to
 * 5..5000. Returns the decimation (samples per point). */
uint32_t scope_arm(uint32_t window_ms, bool trigger);

scope_state_t scope_state(void);

/* Valid once scope_state() == SCOPE_READY. */
uint32_t scope_decim(void);
bool scope_triggered(void);
/* 2 * SCOPE_POINTS bytes: (min, max) mu-law pairs for one channel. */
const uint8_t *scope_data(scope_channel_t channel);

#endif /* SCOPE_H */
