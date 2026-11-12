/*
 * fx_algo_table.h - Algorithm catalogue (single source of truth).
 *
 * Read by the firmware (registry, ALGOS command, presets) and parsed with a
 * regex by tools/gfx_link.py (simulator). Keep the one-entry-per-line layout.
 *
 * IDs are stored in flash presets: never renumber, only append.
 *
 * A(id, enum_suffix, "Display Name", category)
 * P(key, min, max, def, step, "unit", "labels")
 *   unit:   "" when unitless.
 *   labels: "" or '|' separated names for an integer selector (index = value - min).
 * Max FX_ALGO_MAX_PARAMS (8) params per algorithm. Keys: lowercase, no spaces.
 */
#ifndef FX_ALGO_TABLE_H
#define FX_ALGO_TABLE_H

#define FX_ALGO_MAX_PARAMS 8

typedef enum
{
  FX_CAT_NONE = 0,
  FX_CAT_DELAY,
  FX_CAT_MOD,
  FX_CAT_PITCH,
  FX_CAT_REVERB,
  FX_CAT_DRIVE,
  FX_CAT_LOOPER,
  FX_CAT_COUNT
} fx_category_t;

/* Protocol / GUI names, indexed by fx_category_t. */
#define FX_CATEGORY_NAMES { "None", "Delay", "Modulation", "Pitch", "Reverb", "Drive", "Looper" }

#define FX_ALGO_LIST(A) \
  A(0,  NONE,        "Empty",          FX_CAT_NONE)   \
  A(1,  DIGITAL_DLY, "Digital Delay",  FX_CAT_DELAY)  \
  A(2,  TAPE_ECHO,   "Tape Echo",      FX_CAT_DELAY)  \
  A(3,  MOD_DLY,     "Mod Delay",      FX_CAT_DELAY)  \
  A(4,  PINGPONG,    "Ping-Pong",      FX_CAT_DELAY)  \
  A(5,  REVERSE_DLY, "Reverse Delay",  FX_CAT_DELAY)  \
  A(6,  CHORUS,      "Chorus",         FX_CAT_MOD)    \
  A(7,  VIBRATO,     "Vibrato",        FX_CAT_MOD)    \
  A(8,  FLANGER,     "Flanger",        FX_CAT_MOD)    \
  A(9,  PHASER,      "Phaser",         FX_CAT_MOD)    \
  A(10, TREMOLO,     "Tremolo/Pan",    FX_CAT_MOD)    \
  A(11, ROTARY,      "Rotary",         FX_CAT_MOD)    \
  A(12, RINGMOD,     "Ring Mod",       FX_CAT_MOD)    \
  A(13, OCTAVER,     "Octaver",        FX_CAT_PITCH)  \
  A(14, HARMONIZER,  "Harmonizer",     FX_CAT_PITCH)  \
  A(15, DETUNE,      "Detune",         FX_CAT_PITCH)  \
  A(16, ROOM,        "Room",           FX_CAT_REVERB) \
  A(17, HALL,        "Hall",           FX_CAT_REVERB) \
  A(18, PLATE,       "Plate",          FX_CAT_REVERB) \
  A(19, SPRING,      "Spring",         FX_CAT_REVERB) \
  A(20, SHIMMER,     "Shimmer",        FX_CAT_REVERB) \
  A(21, OVERDRIVE,   "Overdrive",      FX_CAT_DRIVE)  \
  A(22, FUZZ,        "Fuzz",           FX_CAT_DRIVE)  \
  A(23, BITCRUSH,    "Bit Crusher",    FX_CAT_DRIVE)  \
  A(24, COMPRESSOR,  "Compressor",     FX_CAT_DRIVE)  \
  A(25, AUTOWAH,     "Auto-Wah",       FX_CAT_DRIVE)  \
  A(26, EQ,          "EQ",             FX_CAT_DRIVE)  \
  A(27, LOOPER,      "Looper",         FX_CAT_LOOPER)

/* ---- Parameters per algorithm: FX_PARAMS_<enum_suffix>(P) ---------------- */

#define FX_PARAMS_NONE(P)

#define FX_PARAMS_DIGITAL_DLY(P) \
  P(time,     20.0f,  750.0f, 400.0f, 1.0f,  "ms", "") \
  P(feedback, 0.0f,   0.95f,  0.35f,  0.01f, "",   "") \
  P(tone,     0.0f,   1.0f,   0.8f,   0.01f, "",   "") \
  P(mod,      0.0f,   1.0f,   0.0f,   0.01f, "",   "") \
  P(mix,      0.0f,   1.0f,   0.35f,  0.01f, "",   "")

#define FX_PARAMS_TAPE_ECHO(P) \
  P(time,     40.0f,  750.0f, 320.0f, 1.0f,  "ms", "") \
  P(feedback, 0.0f,   0.95f,  0.4f,   0.01f, "",   "") \
  P(wow,      0.0f,   1.0f,   0.3f,   0.01f, "",   "") \
  P(saturate, 0.0f,   1.0f,   0.4f,   0.01f, "",   "") \
  P(age,      0.0f,   1.0f,   0.3f,   0.01f, "",   "") \
  P(mix,      0.0f,   1.0f,   0.35f,  0.01f, "",   "")

#define FX_PARAMS_MOD_DLY(P) \
  P(time,     20.0f,  750.0f, 300.0f, 1.0f,  "ms", "") \
  P(feedback, 0.0f,   0.95f,  0.4f,   0.01f, "",   "") \
  P(rate,     0.1f,   5.0f,   0.6f,   0.05f, "Hz", "") \
  P(depth,    0.0f,   1.0f,   0.5f,   0.01f, "",   "") \
  P(mix,      0.0f,   1.0f,   0.35f,  0.01f, "",   "")

#define FX_PARAMS_PINGPONG(P) \
  P(time,     20.0f,  375.0f, 300.0f, 1.0f,  "ms", "") \
  P(feedback, 0.0f,   0.95f,  0.45f,  0.01f, "",   "") \
  P(tone,     0.0f,   1.0f,   0.8f,   0.01f, "",   "") \
  P(spread,   0.0f,   1.0f,   1.0f,   0.01f, "",   "") \
  P(mix,      0.0f,   1.0f,   0.35f,  0.01f, "",   "")

#define FX_PARAMS_REVERSE_DLY(P) \
  P(time,     100.0f, 750.0f, 500.0f, 1.0f,  "ms", "") \
  P(feedback, 0.0f,   0.9f,   0.3f,   0.01f, "",   "") \
  P(mix,      0.0f,   1.0f,   0.5f,   0.01f, "",   "")

#define FX_PARAMS_CHORUS(P) \
  P(rate,     0.1f,   5.0f,   0.8f,   0.05f, "Hz", "") \
  P(depth,    0.0f,   1.0f,   0.5f,   0.01f, "",   "") \
  P(width,    0.0f,   1.0f,   0.7f,   0.01f, "",   "") \
  P(mix,      0.0f,   1.0f,   0.5f,   0.01f, "",   "")

#define FX_PARAMS_VIBRATO(P) \
  P(rate,     0.5f,   10.0f,  5.0f,   0.1f,  "Hz", "") \
  P(depth,    0.0f,   1.0f,   0.4f,   0.01f, "",   "")

#define FX_PARAMS_FLANGER(P) \
  P(rate,     0.05f,  5.0f,   0.3f,   0.05f, "Hz", "") \
  P(depth,    0.0f,   1.0f,   0.7f,   0.01f, "",   "") \
  P(manual,   0.0f,   1.0f,   0.3f,   0.01f, "",   "") \
  P(feedback, -0.95f, 0.95f,  0.5f,   0.01f, "",   "") \
  P(mix,      0.0f,   1.0f,   0.5f,   0.01f, "",   "")

#define FX_PARAMS_PHASER(P) \
  P(rate,     0.05f,  5.0f,   0.5f,   0.05f, "Hz", "") \
  P(depth,    0.0f,   1.0f,   0.8f,   0.01f, "",   "") \
  P(feedback, 0.0f,   0.9f,   0.5f,   0.01f, "",   "") \
  P(stages,   2.0f,   8.0f,   4.0f,   2.0f,  "",   "") \
  P(mix,      0.0f,   1.0f,   0.5f,   0.01f, "",   "")

#define FX_PARAMS_TREMOLO(P) \
  P(rate,     0.5f,   15.0f,  5.0f,   0.1f,  "Hz", "") \
  P(depth,    0.0f,   1.0f,   0.6f,   0.01f, "",   "") \
  P(shape,    0.0f,   2.0f,   0.0f,   1.0f,  "",   "Sine|Triangle|Square") \
  P(mode,     0.0f,   1.0f,   0.0f,   1.0f,  "",   "Tremolo|Pan")

#define FX_PARAMS_ROTARY(P) \
  P(speed,    0.0f,   1.0f,   0.0f,   1.0f,  "",   "Slow|Fast") \
  P(accel,    0.0f,   1.0f,   0.5f,   0.01f, "",   "") \
  P(drive,    0.0f,   1.0f,   0.2f,   0.01f, "",   "") \
  P(mix,      0.0f,   1.0f,   1.0f,   0.01f, "",   "")

#define FX_PARAMS_RINGMOD(P) \
  P(freq,     20.0f,  2000.0f, 440.0f, 1.0f, "Hz", "") \
  P(lfo_rate, 0.0f,   10.0f,  0.0f,   0.1f,  "Hz", "") \
  P(lfo_depth,0.0f,   1.0f,   0.0f,   0.01f, "",   "") \
  P(mix,      0.0f,   1.0f,   0.5f,   0.01f, "",   "")

#define FX_PARAMS_OCTAVER(P) \
  P(sub1,     0.0f,   1.0f,   0.7f,   0.01f, "",   "") \
  P(sub2,     0.0f,   1.0f,   0.0f,   0.01f, "",   "") \
  P(up,       0.0f,   1.0f,   0.0f,   0.01f, "",   "") \
  P(tone,     0.0f,   1.0f,   0.5f,   0.01f, "",   "") \
  P(dry,      0.0f,   1.0f,   1.0f,   0.01f, "",   "")

#define FX_PARAMS_HARMONIZER(P) \
  P(voice1,   -12.0f, 12.0f,  4.0f,   1.0f,  "st", "") \
  P(voice2,   -12.0f, 12.0f,  7.0f,   1.0f,  "st", "") \
  P(level1,   0.0f,   1.0f,   0.6f,   0.01f, "",   "") \
  P(level2,   0.0f,   1.0f,   0.5f,   0.01f, "",   "") \
  P(dry,      0.0f,   1.0f,   1.0f,   0.01f, "",   "")

#define FX_PARAMS_DETUNE(P) \
  P(cents,    0.0f,   50.0f,  12.0f,  1.0f,  "cents", "") \
  P(delay,    0.0f,   50.0f,  15.0f,  1.0f,  "ms", "") \
  P(mix,      0.0f,   1.0f,   0.5f,   0.01f, "",   "")

#define FX_PARAMS_ROOM(P) \
  P(decay,    0.0f,   1.0f,   0.4f,   0.01f, "",   "") \
  P(damp,     0.0f,   1.0f,   0.5f,   0.01f, "",   "") \
  P(predelay, 0.0f,   100.0f, 10.0f,  1.0f,  "ms", "") \
  P(mix,      0.0f,   1.0f,   0.3f,   0.01f, "",   "")

#define FX_PARAMS_HALL(P) \
  P(decay,    0.0f,   1.0f,   0.7f,   0.01f, "",   "") \
  P(damp,     0.0f,   1.0f,   0.4f,   0.01f, "",   "") \
  P(predelay, 0.0f,   100.0f, 25.0f,  1.0f,  "ms", "") \
  P(width,    0.0f,   1.0f,   1.0f,   0.01f, "",   "") \
  P(mix,      0.0f,   1.0f,   0.3f,   0.01f, "",   "")

#define FX_PARAMS_PLATE(P) \
  P(decay,    0.0f,   1.0f,   0.6f,   0.01f, "",   "") \
  P(damp,     0.0f,   1.0f,   0.3f,   0.01f, "",   "") \
  P(predelay, 0.0f,   100.0f, 0.0f,   1.0f,  "ms", "") \
  P(mix,      0.0f,   1.0f,   0.3f,   0.01f, "",   "")

#define FX_PARAMS_SPRING(P) \
  P(decay,    0.0f,   1.0f,   0.5f,   0.01f, "",   "") \
  P(tone,     0.0f,   1.0f,   0.5f,   0.01f, "",   "") \
  P(drip,     0.0f,   1.0f,   0.5f,   0.01f, "",   "") \
  P(mix,      0.0f,   1.0f,   0.3f,   0.01f, "",   "")

#define FX_PARAMS_SHIMMER(P) \
  P(decay,    0.0f,   1.0f,   0.8f,   0.01f, "",   "") \
  P(shimmer,  0.0f,   1.0f,   0.5f,   0.01f, "",   "") \
  P(interval, 0.0f,   1.0f,   0.0f,   1.0f,  "",   "Octave|Fifth") \
  P(damp,     0.0f,   1.0f,   0.4f,   0.01f, "",   "") \
  P(mix,      0.0f,   1.0f,   0.4f,   0.01f, "",   "")

#define FX_PARAMS_OVERDRIVE(P) \
  P(gain,     0.0f,   1.0f,   0.4f,   0.01f, "",   "") \
  P(tone,     0.0f,   1.0f,   0.5f,   0.01f, "",   "") \
  P(level,    0.0f,   1.0f,   0.5f,   0.01f, "",   "")

#define FX_PARAMS_FUZZ(P) \
  P(gain,     0.0f,   1.0f,   0.6f,   0.01f, "",   "") \
  P(tone,     0.0f,   1.0f,   0.5f,   0.01f, "",   "") \
  P(bias,     0.0f,   1.0f,   0.3f,   0.01f, "",   "") \
  P(level,    0.0f,   1.0f,   0.5f,   0.01f, "",   "")

#define FX_PARAMS_BITCRUSH(P) \
  P(bits,     2.0f,   16.0f,  8.0f,   1.0f,  "bits", "") \
  P(downsample, 1.0f, 32.0f,  4.0f,   1.0f,  "x",  "") \
  P(mix,      0.0f,   1.0f,   1.0f,   0.01f, "",   "")

#define FX_PARAMS_COMPRESSOR(P) \
  P(threshold, -40.0f, 0.0f,  -20.0f, 0.5f,  "dB", "") \
  P(ratio,    1.0f,   20.0f,  4.0f,   0.1f,  ":1", "") \
  P(attack,   1.0f,   100.0f, 10.0f,  1.0f,  "ms", "") \
  P(release,  20.0f,  1000.0f, 150.0f, 1.0f, "ms", "") \
  P(makeup,   0.0f,   24.0f,  6.0f,   0.5f,  "dB", "") \
  P(mix,      0.0f,   1.0f,   1.0f,   0.01f, "",   "")

#define FX_PARAMS_AUTOWAH(P) \
  P(sens,     0.0f,   1.0f,   0.6f,   0.01f, "",   "") \
  P(range,    0.0f,   1.0f,   0.7f,   0.01f, "",   "") \
  P(q,        0.5f,   10.0f,  3.0f,   0.1f,  "",   "") \
  P(mode,     0.0f,   1.0f,   0.0f,   1.0f,  "",   "Envelope|LFO") \
  P(rate,     0.1f,   5.0f,   1.0f,   0.05f, "Hz", "") \
  P(mix,      0.0f,   1.0f,   1.0f,   0.01f, "",   "")

#define FX_PARAMS_EQ(P) \
  P(low,      -12.0f, 12.0f,  0.0f,   0.5f,  "dB", "") \
  P(mid,      -12.0f, 12.0f,  0.0f,   0.5f,  "dB", "") \
  P(mid_freq, 200.0f, 5000.0f, 800.0f, 10.0f, "Hz", "") \
  P(high,     -12.0f, 12.0f,  0.0f,   0.5f,  "dB", "") \
  P(level,    -12.0f, 12.0f,  0.0f,   0.5f,  "dB", "")

#define FX_PARAMS_LOOPER(P) \
  P(level,    0.0f,   1.0f,   1.0f,   0.01f, "",   "") \
  P(dub_fb,   0.0f,   1.0f,   0.9f,   0.01f, "",   "") \
  P(reverse,  0.0f,   1.0f,   0.0f,   1.0f,  "",   "Off|On") \
  P(half,     0.0f,   1.0f,   0.0f,   1.0f,  "",   "Off|On")

#endif /* FX_ALGO_TABLE_H */
