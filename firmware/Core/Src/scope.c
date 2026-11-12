/*
 * scope.c - Waveform capture (see scope.h).
 */
#include "scope.h"

#include "stm32f4xx.h"
#include "dsp_util.h"
#include "fx_algo.h"

#define TRIGGER_LEVEL        655  /* 0.02 full scale */
#define TRIGGER_MIN_WAIT_MS  50u
#define NO_CANDIDATE         (-1)

/* Everything below the state flag is owned by the ISR while ARMED/CAPTURING
 * and by the main loop otherwise. */
static volatile scope_state_t s_state = SCOPE_IDLE;

static uint8_t s_data[SCOPE_CHANNELS][2u * SCOPE_POINTS];
static uint32_t s_decim = 1;
static bool s_triggered;

static uint32_t s_wait_samples;     /* ARMED: samples left before free-run */
static bool s_was_below;
static int16_t s_prev_in;
static int32_t s_zero_cross;        /* sample index in the current block, or NO_CANDIDATE */

static uint32_t s_point;            /* CAPTURING: next point to complete */
static uint32_t s_in_point;         /* samples accumulated into the current point */
static int16_t s_min[SCOPE_CHANNELS];
static int16_t s_max[SCOPE_CHANNELS];

uint32_t scope_arm(uint32_t window_ms, bool trigger)
{
  if (window_ms < SCOPE_MIN_WINDOW_MS)
  {
    window_ms = SCOPE_MIN_WINDOW_MS;
  }
  if (window_ms > SCOPE_MAX_WINDOW_MS)
  {
    window_ms = SCOPE_MAX_WINDOW_MS;
  }

  s_state = SCOPE_IDLE;  /* stops the ISR before the shared fields change */
  __DMB();

  const uint32_t decim = (window_ms * (FX_SAMPLE_RATE / 1000u) + SCOPE_POINTS / 2u) / SCOPE_POINTS;
  s_decim = decim > 0u ? decim : 1u;
  s_triggered = false;
  s_point = 0;
  s_in_point = 0;
  s_was_below = false;
  s_prev_in = 0;
  s_zero_cross = NO_CANDIDATE;
  s_wait_samples = (window_ms > TRIGGER_MIN_WAIT_MS ? window_ms : TRIGGER_MIN_WAIT_MS) * (FX_SAMPLE_RATE / 1000u);

  __DMB();
  s_state = trigger ? SCOPE_ARMED : SCOPE_CAPTURING;
  return s_decim;
}

scope_state_t scope_state(void)
{
  return s_state;
}

uint32_t scope_decim(void)
{
  return s_decim;
}

bool scope_triggered(void)
{
  return s_triggered;
}

const uint8_t *scope_data(scope_channel_t channel)
{
  return s_data[channel];
}

/* Rising zero crossing after the signal was below -0.02 FS and then rose
 * above +0.02 FS: returns the index of the crossing, or NO_CANDIDATE. */
static int32_t find_trigger(const int16_t *in, uint32_t from, uint32_t frames)
{
  for (uint32_t i = from; i < frames; i++)
  {
    const int16_t v = in[i];

    if (v < -TRIGGER_LEVEL)
    {
      s_was_below = true;
    }
    if (s_was_below && s_prev_in < 0 && v >= 0)
    {
      s_zero_cross = (int32_t)i;
    }
    else if (v < 0)
    {
      s_zero_cross = NO_CANDIDATE;
    }
    s_prev_in = v;

    if (s_was_below && s_zero_cross != NO_CANDIDATE && v > TRIGGER_LEVEL)
    {
      return s_zero_cross;
    }
  }
  if (s_zero_cross != NO_CANDIDATE)
  {
    s_zero_cross = 0;  /* candidate carries over: next block starts at its first frame */
  }
  return NO_CANDIDATE;
}

static void accumulate(uint32_t channel, int16_t v)
{
  if (s_in_point == 0u || v < s_min[channel])
  {
    s_min[channel] = v;
  }
  if (s_in_point == 0u || v > s_max[channel])
  {
    s_max[channel] = v;
  }
}

static void store_point(void)
{
  for (uint32_t ch = 0; ch < SCOPE_CHANNELS; ch++)
  {
    s_data[ch][2u * s_point] = dsp_ulaw_encode(s_min[ch]);
    s_data[ch][2u * s_point + 1u] = dsp_ulaw_encode(s_max[ch]);
  }
  s_point++;
  s_in_point = 0;
}

/* Consumes frames [from, frames); returns true when the capture completed. */
static bool capture(const int16_t *in, const int16_t *out_lr, uint32_t from, uint32_t frames)
{
  for (uint32_t i = from; i < frames; i++)
  {
    accumulate(SCOPE_CH_IN, in[i]);
    accumulate(SCOPE_CH_L, out_lr[2u * i]);
    accumulate(SCOPE_CH_R, out_lr[2u * i + 1u]);
    s_in_point++;

    if (s_in_point == s_decim)
    {
      store_point();
      if (s_point == SCOPE_POINTS)
      {
        return true;
      }
    }
  }
  return false;
}

/* ARMED: returns the first frame to capture, or `frames` if still waiting. */
static uint32_t wait_for_trigger(const int16_t *in, uint32_t frames)
{
  const int32_t start = find_trigger(in, 0, frames);

  if (start != NO_CANDIDATE)
  {
    s_triggered = true;
    s_state = SCOPE_CAPTURING;
    return (uint32_t)start;
  }
  if (s_wait_samples <= frames)
  {
    s_state = SCOPE_CAPTURING;  /* timed out: free-run, triggered stays 0 */
    return 0;
  }
  s_wait_samples -= frames;
  return frames;
}

void scope_feed(const int16_t *in_mono, const int16_t *out_lr, uint32_t frames)
{
  uint32_t from = 0;

  if (s_state == SCOPE_ARMED)
  {
    from = wait_for_trigger(in_mono, frames);
  }
  if (s_state != SCOPE_CAPTURING)
  {
    return;
  }
  if (capture(in_mono, out_lr, from, frames))
  {
    __DMB();  /* point data must be visible before READY */
    s_state = SCOPE_READY;
  }
}
