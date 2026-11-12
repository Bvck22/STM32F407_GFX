/*
 * usb_cmd.c - ASCII command parser for the CDC link (see docs/protocol.md v2).
 */
#include "usb_cmd.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "audio_out.h"
#include "fx_engine.h"
#include "preset_store.h"
#include "sample_data.h"
#include "scope.h"
#include "stm32f4xx_hal.h"
#include "usbd_cdc_if.h"

#define MAX_LINE_CHARS    63u
#define MAX_TOKENS        4
#define REPLY_BUF_SIZE    544u  /* longest line: SC with 512 hex digits */
#define LOOP_POLL_MS      50u
#define STATE_UNKNOWN     0xFFu

typedef struct
{
  const char *name;
  int max_tokens;               /* including the keyword; the last token takes the rest of the line */
  void (*run)(int argc, char **argv);
} command_t;

/* A long reply produced one line per step so it never overruns the TX queue. */
typedef enum
{
  JOB_NONE = 0,
  JOB_ALGOS,
  JOB_STATE,
  JOB_SLOT_VALUES,
  JOB_PLIST,
  JOB_SCOPE_DATA,
} job_kind_t;

typedef struct
{
  job_kind_t kind;
  uint32_t phase;
  uint32_t i;
  uint32_t j;
} job_t;

static const char *const k_loop_state_names[] = { "empty", "recording", "playing", "overdub", "stopped" };
static const char *const k_category_names[] = FX_CATEGORY_NAMES;

static char s_line[MAX_LINE_CHARS + 1u];
static uint32_t s_line_len;
static bool s_line_overflow;

static job_t s_job;
static int s_pending_play = -1;
static uint8_t s_loop_seen[FX_SLOTS];
static uint32_t s_loop_polled_at;

/* ---- output -------------------------------------------------------------- */

static void reply(const char *fmt, ...)
{
  char buf[REPLY_BUF_SIZE];
  va_list args;

  va_start(args, fmt);
  int n = vsnprintf(buf, sizeof(buf) - 1u, fmt, args);
  va_end(args);

  if (n < 0)
  {
    return;
  }
  if ((uint32_t)n > sizeof(buf) - 2u)
  {
    n = (int)sizeof(buf) - 2;
  }
  buf[n++] = '\n';
  CDC_Write((const uint8_t *)buf, (uint32_t)n);
}

static const char *or_dash(const char *text)
{
  return (text == NULL || text[0] == '\0') ? "-" : text;
}

static void reply_result(fx_result_t result)
{
  switch (result)
  {
    case FX_ERR_MEMORY:
      reply("ERR memory");
      break;
    case FX_ERR_CPU:
      reply("ERR cpu");
      break;
    case FX_ERR_NO_CMD:
      reply("ERR no looper");
      break;
    default:
      reply("ERR range");
      break;
  }
}

/* ---- argument parsing ---------------------------------------------------- */

static bool parse_float(const char *text, float *value)
{
  char *end;
  const float v = strtof(text, &end);

  if (end == text || *end != '\0' || !isfinite(v))
  {
    return false;
  }
  *value = v;
  return true;
}

static bool parse_uint(const char *text, uint32_t *value)
{
  char *end;

  if (*text < '0' || *text > '9')
  {
    return false;
  }
  const unsigned long v = strtoul(text, &end, 10);
  if (*end != '\0' || v > 0xFFFFu)
  {
    return false;
  }
  *value = (uint32_t)v;
  return true;
}

/* Parses argv[first] and argv[first + 1] as unsigned integers. */
static bool parse_two_uints(int argc, char **argv, int first, uint32_t *a, uint32_t *b)
{
  return argc == first + 2 && parse_uint(argv[first], a) && parse_uint(argv[first + 1], b);
}

/* ---- looper event tracking ----------------------------------------------- */

static uint8_t slot_loop_state(uint32_t slot)
{
  fx_status_t st;

  if (fx_slot_status(slot, &st) != FX_OK || st.state >= sizeof(k_loop_state_names) / sizeof(k_loop_state_names[0]))
  {
    return STATE_UNKNOWN;
  }
  return (uint8_t)st.state;
}

static bool slot_is_looper(uint32_t slot)
{
  return fx_algo_category(fx_slot_algo(slot)) == FX_CAT_LOOPER;
}

static int first_looper_slot(void)
{
  for (uint32_t i = 0; i < FX_SLOTS; i++)
  {
    if (slot_is_looper(i))
    {
      return (int)i;
    }
  }
  return -1;
}

/* Samples every slot; optionally reports changes nobody asked for. */
static void scan_loops(bool emit_events)
{
  for (uint32_t i = 0; i < FX_SLOTS; i++)
  {
    const uint8_t state = slot_loop_state(i);
    if (emit_events && state != STATE_UNKNOWN && s_loop_seen[i] != STATE_UNKNOWN && state != s_loop_seen[i])
    {
      reply("EVT LOOP %" PRIu32 " %s", i, k_loop_state_names[state]);
    }
    s_loop_seen[i] = state;
  }
}

bool usb_cmd_looper_recording(void)
{
  for (uint32_t i = 0; i < FX_SLOTS; i++)
  {
    if (s_loop_seen[i] == FX_LOOP_RECORDING)
    {
      return true;
    }
  }
  return false;
}

bool usb_cmd_looper_rec(void)
{
  const int slot = first_looper_slot();

  if (slot < 0 || fx_slot_command((uint32_t)slot, FX_CMD_REC) != FX_OK)
  {
    return false;
  }
  const uint8_t state = slot_loop_state((uint32_t)slot);
  if (state != STATE_UNKNOWN)
  {
    reply("EVT LOOP %d %s", slot, k_loop_state_names[state]);
  }
  s_loop_seen[slot] = state;
  return true;
}

/* ---- streamed replies ---------------------------------------------------- */

static void print_value_line(uint32_t slot, uint32_t idx)
{
  reply("V %" PRIu32 " %" PRIu32 " %g", slot, idx, (double)fx_slot_get_param(slot, idx));
}

/* Emits the next "V <slot> <idx> <value>" line, scanning slots from first_slot
 * to last_slot. Returns false when no lines are left. */
static bool step_values(job_t *job, uint32_t last_slot)
{
  while (job->i <= last_slot)
  {
    const uint32_t count = fx_algo_param_count(fx_slot_algo(job->i));
    if (job->j < count)
    {
      print_value_line(job->i, job->j++);
      return true;
    }
    job->i++;
    job->j = 0;
  }
  return false;
}

static bool step_algos(job_t *job)
{
  if (job->i >= FX_ALGO_COUNT)
  {
    reply("END");
    return false;
  }

  const fx_algo_id_t id = (fx_algo_id_t)job->i;
  const uint32_t count = fx_algo_param_count(id);
  if (job->j == 0u)
  {
    reply("A %u %s %" PRIu32 " %s", (unsigned)id, k_category_names[fx_algo_category(id)],
          count, fx_algo_name(id));
    job->j = 1u;
  }
  else
  {
    const uint32_t idx = job->j - 1u;
    const fx_param_desc_t *p = &fx_algo_params(id)[idx];
    reply("AP %u %" PRIu32 " %s %g %g %g %g %s %s", (unsigned)id, idx, p->key,
          (double)p->min, (double)p->max, (double)p->step, (double)p->def,
          or_dash(p->unit), or_dash(p->labels));
    job->j++;
  }
  if (job->j > count)
  {
    job->i++;
    job->j = 0;
  }
  return true;
}

static bool step_state(job_t *job)
{
  switch (job->phase)
  {
    case 0:
    {
      const fx_param_info_t *g = &g_fx_param_info[job->i];
      reply("G %s %g %g %g %g %g", g->name, (double)g->min, (double)g->max, (double)g->step,
            (double)g->def, (double)fx_get_global((fx_param_id_t)job->i));
      if (++job->i >= FXP_COUNT)
      {
        job->phase = 1;
        job->i = 0;
      }
      return true;
    }
    case 1:
      reply("SLOT %" PRIu32 " %u %d", job->i, (unsigned)fx_slot_algo(job->i), fx_slot_enabled(job->i) ? 1 : 0);
      if (++job->i >= FX_SLOTS)
      {
        job->phase = 2;
        job->i = 0;
        job->j = 0;
      }
      return true;
    case 2:
      if (step_values(job, FX_SLOTS - 1u))
      {
        return true;
      }
      job->phase = 3;
      return step_state(job);
    case 3:
      if (preset_current() >= 0)
      {
        reply("PRESET %d %s", preset_current(), preset_name((uint32_t)preset_current()));
      }
      else
      {
        reply("PRESET -1 -");
      }
      job->phase = 4;
      return true;
    default:
      reply("END");
      return false;
  }
}

static bool step_slot_values(job_t *job)
{
  if (step_values(job, job->i))
  {
    return true;
  }
  reply("END");
  return false;
}

static bool step_plist(job_t *job)
{
  while (job->i < PRESET_COUNT)
  {
    const uint32_t idx = job->i++;
    if (preset_used(idx))
    {
      reply("PR %" PRIu32 " %s", idx, preset_name(idx));
      return true;
    }
  }
  reply("END");
  return false;
}

/* One "SC <ch> <offset> <hex>" line per step: 3 channels x 2 halves. */
static bool step_scope_data(job_t *job)
{
  static const char *const k_channel_names[SCOPE_CHANNELS] = { "in", "l", "r" };
  static const char k_hex[] = "0123456789ABCDEF";
  enum { HALF_POINTS = SCOPE_POINTS / 2 };

  if (job->i >= SCOPE_CHANNELS * 2u)
  {
    reply("END");
    return false;
  }

  const uint32_t ch = job->i / 2u;
  const uint32_t offset = (job->i % 2u) * HALF_POINTS;
  const uint8_t *bytes = &scope_data((scope_channel_t)ch)[2u * offset];
  char line[REPLY_BUF_SIZE];

  int n = snprintf(line, sizeof(line), "SC %s %" PRIu32 " ", k_channel_names[ch], offset);
  for (uint32_t i = 0; i < 2u * HALF_POINTS; i++)
  {
    line[n++] = k_hex[bytes[i] >> 4];
    line[n++] = k_hex[bytes[i] & 0x0Fu];
  }
  line[n++] = '\n';
  CDC_Write((const uint8_t *)line, (uint32_t)n);
  job->i++;
  return true;
}

static void start_job(job_kind_t kind, uint32_t first_slot)
{
  s_job = (job_t){ .kind = kind, .i = first_slot };
}

/* Emits one line of the active job and clears the job when it is done. */
static void step_job(void)
{
  bool more = false;

  switch (s_job.kind)
  {
    case JOB_ALGOS:
      more = step_algos(&s_job);
      break;
    case JOB_STATE:
      more = step_state(&s_job);
      break;
    case JOB_SLOT_VALUES:
      more = step_slot_values(&s_job);
      break;
    case JOB_PLIST:
      more = step_plist(&s_job);
      break;
    case JOB_SCOPE_DATA:
      more = step_scope_data(&s_job);
      break;
    default:
      break;
  }
  if (!more)
  {
    s_job.kind = JOB_NONE;
  }
}

/* ---- commands ------------------------------------------------------------ */

static void cmd_ping(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  reply("PONG GFX 2.0");
}

static void cmd_algos(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  start_job(JOB_ALGOS, 0);
}

static void cmd_state(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  start_job(JOB_STATE, 0);
}

static void cmd_setg(int argc, char **argv)
{
  const int id = argc == 3 ? fx_param_find(argv[1]) : -1;
  float value;

  if (id < 0)
  {
    reply("ERR range");
  }
  else if (!parse_float(argv[2], &value))
  {
    reply("ERR bad value");
  }
  else
  {
    reply("OK G %s %g", argv[1], (double)fx_set_global((fx_param_id_t)id, value));
  }
}

static void cmd_slot(int argc, char **argv)
{
  uint32_t slot, algo;

  if (!parse_two_uints(argc, argv, 1, &slot, &algo))
  {
    reply("ERR bad value");
    return;
  }
  if (slot >= FX_SLOTS || algo >= FX_ALGO_COUNT)
  {
    reply("ERR range");
    return;
  }

  const fx_result_t result = fx_slot_assign(slot, (fx_algo_id_t)algo);
  if (result != FX_OK)
  {
    reply_result(result);
    return;
  }
  reply("OK SLOT %" PRIu32 " %" PRIu32, slot, algo);
  start_job(JOB_SLOT_VALUES, slot);
}

static void cmd_on(int argc, char **argv)
{
  uint32_t slot, on;

  if (!parse_two_uints(argc, argv, 1, &slot, &on) || on > 1u)
  {
    reply("ERR bad value");
    return;
  }
  const fx_result_t result = fx_slot_enable(slot, on != 0u);
  if (result == FX_OK)
  {
    reply("OK ON %" PRIu32 " %" PRIu32, slot, on);
  }
  else
  {
    reply_result(result);
  }
}

static void cmd_set(int argc, char **argv)
{
  uint32_t slot, idx;
  float value, applied;

  if (argc != 4 || !parse_uint(argv[1], &slot) || !parse_uint(argv[2], &idx) ||
      !parse_float(argv[3], &value))
  {
    reply("ERR bad value");
    return;
  }
  const fx_result_t result = fx_slot_set_param(slot, idx, value, &applied);
  if (result == FX_OK)
  {
    reply("OK V %" PRIu32 " %" PRIu32 " %g", slot, idx, (double)applied);
  }
  else
  {
    reply_result(result);
  }
}

static void cmd_move(int argc, char **argv)
{
  uint32_t from, to;

  if (!parse_two_uints(argc, argv, 1, &from, &to))
  {
    reply("ERR bad value");
    return;
  }
  const fx_result_t result = fx_slot_move(from, to);
  if (result == FX_OK)
  {
    reply("OK MOVE %" PRIu32 " %" PRIu32, from, to);
  }
  else
  {
    reply_result(result);
  }
}

static void cmd_defaults(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  fx_load_defaults();
  reply("OK DEFAULTS");
}

static void cmd_play(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  audio_set_playing(true);
  reply("OK PLAY");
}

static void cmd_stop(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  audio_set_playing(false);
  reply("OK STOP");
}

static bool parse_loop_command(const char *text, fx_cmd_t *cmd)
{
  static const char *const names[FX_CMD_COUNT] = { "REC", "DUB", "PLAY", "STOP", "CLEAR" };

  for (int i = 0; i < FX_CMD_COUNT; i++)
  {
    if (strcasecmp(text, names[i]) == 0)
    {
      *cmd = (fx_cmd_t)i;
      return true;
    }
  }
  return false;
}

static void cmd_loop(int argc, char **argv)
{
  uint32_t slot;
  fx_cmd_t cmd;

  if (argc != 3 || !parse_uint(argv[1], &slot) || !parse_loop_command(argv[2], &cmd))
  {
    reply("ERR bad value");
    return;
  }
  if (slot >= FX_SLOTS || !slot_is_looper(slot))
  {
    reply("ERR no looper");
    return;
  }

  const fx_result_t result = fx_slot_command(slot, cmd);
  const uint8_t state = slot_loop_state(slot);
  if (result != FX_OK || state == STATE_UNKNOWN)
  {
    reply_result(result == FX_OK ? FX_ERR_NO_CMD : result);
    return;
  }
  reply("OK LOOP %" PRIu32 " %s", slot, k_loop_state_names[state]);
}

static void cmd_plist(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  start_job(JOB_PLIST, 0);
}

static void cmd_psave(int argc, char **argv)
{
  uint32_t index;

  if (argc != 3 || !parse_uint(argv[1], &index) || strlen(argv[2]) > PRESET_NAME_MAX)
  {
    reply("ERR bad value");
  }
  else if (index >= PRESET_COUNT)
  {
    reply("ERR range");
  }
  else if (preset_save(index, argv[2]))
  {
    reply("OK PSAVE %" PRIu32, index);
  }
  else
  {
    reply("ERR flash");
  }
}

static void cmd_pload(int argc, char **argv)
{
  uint32_t index;

  if (argc != 2 || !parse_uint(argv[1], &index))
  {
    reply("ERR bad value");
    return;
  }
  const fx_result_t result = preset_load(index);
  if (result == FX_OK)
  {
    reply("OK PLOAD %" PRIu32, index);
  }
  else if (result == FX_ERR_RANGE)
  {
    reply("ERR no preset");
  }
  else
  {
    reply_result(result);
  }
}

static void cmd_pdel(int argc, char **argv)
{
  uint32_t index;

  if (argc != 2 || !parse_uint(argv[1], &index))
  {
    reply("ERR bad value");
  }
  else if (index >= PRESET_COUNT)
  {
    reply("ERR range");
  }
  else if (preset_delete(index))
  {
    reply("OK PDEL %" PRIu32, index);
  }
  else
  {
    reply("ERR flash");
  }
}

static void format_loop_status(char *out, size_t size)
{
  fx_status_t st;
  const int slot = first_looper_slot();

  if (slot < 0 || fx_slot_status((uint32_t)slot, &st) != FX_OK ||
      st.state >= sizeof(k_loop_state_names) / sizeof(k_loop_state_names[0]))
  {
    snprintf(out, size, "-");
    return;
  }
  snprintf(out, size, "%d:%s:%" PRIu32 ":%" PRIu32 ":%" PRIu32, slot, k_loop_state_names[st.state],
           st.pos, st.len, st.max_len);
}

static void cmd_status(int argc, char **argv)
{
  char loop[64];

  (void)argc;
  (void)argv;
  format_loop_status(loop, sizeof(loop));
  reply("STAT play=%d bypass=%d cpu=%" PRIu32 " peak=%.3f pos=%" PRIu32 " len=%" PRIu32
        " mem=%" PRIu32 "/%" PRIu32 " cost=%" PRIu32 "/%" PRIu32 " loop=%s",
        audio_is_playing() ? 1 : 0, fx_get_global(FXP_BYPASS) >= 0.5f ? 1 : 0,
        audio_get_cpu_percent(), (double)audio_take_peak(), audio_get_position(), g_sample_len,
        fx_mem_used(), fx_mem_total(), fx_cost_used(), fx_cost_budget(), loop);
}

static void cmd_scope(int argc, char **argv)
{
  uint32_t window_ms, trigger;

  if (!parse_two_uints(argc, argv, 1, &window_ms, &trigger) || trigger > 1u)
  {
    reply("ERR bad value");
    return;
  }
  reply("OK SCOPE %" PRIu32 " %u", scope_arm(window_ms, trigger != 0u), (unsigned)SCOPE_POINTS);
}

static void cmd_scopedata(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  const scope_state_t state = scope_state();

  if (state == SCOPE_IDLE)
  {
    reply("SD idle");
  }
  else if (state != SCOPE_READY)
  {
    reply("SD busy");
  }
  else
  {
    reply("SD %" PRIu32 " %u %d", scope_decim(), (unsigned)SCOPE_POINTS, scope_triggered() ? 1 : 0);
    start_job(JOB_SCOPE_DATA, 0);
  }
}

static const command_t k_commands[] = {
  { "PING",     1, cmd_ping },
  { "ALGOS",    1, cmd_algos },
  { "STATE",    1, cmd_state },
  { "SETG",     3, cmd_setg },
  { "SLOT",     3, cmd_slot },
  { "ON",       3, cmd_on },
  { "SET",      4, cmd_set },
  { "MOVE",     3, cmd_move },
  { "DEFAULTS", 1, cmd_defaults },
  { "PLAY",     1, cmd_play },
  { "STOP",     1, cmd_stop },
  { "LOOP",     3, cmd_loop },
  { "PLIST",    1, cmd_plist },
  { "PSAVE",    3, cmd_psave },
  { "PLOAD",    2, cmd_pload },
  { "PDEL",     2, cmd_pdel },
  { "STATUS",   1, cmd_status },
  { "SCOPE",    3, cmd_scope },
  { "SCOPEDATA", 1, cmd_scopedata },
};

/* ---- line handling ------------------------------------------------------- */

static bool is_blank(char c)
{
  return c == ' ' || c == '\t';
}

/* Splits the arguments of a line into at most max tokens in place; the last
 * token keeps the rest of the line (spaces included, trailing blanks cut). */
static int tokenize(char *text, char **tokens, int max)
{
  int count = 0;

  while (count < max)
  {
    while (is_blank(*text))
    {
      *text++ = '\0';
    }
    if (*text == '\0')
    {
      break;
    }
    tokens[count++] = text;
    if (count == max)
    {
      char *end = text + strlen(text);
      while (end > text && is_blank(end[-1]))
      {
        *--end = '\0';
      }
      break;
    }
    while (*text != '\0' && !is_blank(*text))
    {
      text++;
    }
  }
  return count;
}

static void execute_line(char *line)
{
  char *argv[MAX_TOKENS];

  while (is_blank(*line))
  {
    line++;
  }
  if (*line == '\0')
  {
    return;
  }

  char *args = line;
  while (*args != '\0' && !is_blank(*args))
  {
    args++;
  }
  if (*args != '\0')
  {
    *args++ = '\0';
  }

  for (size_t i = 0; i < sizeof(k_commands) / sizeof(k_commands[0]); i++)
  {
    if (strcasecmp(line, k_commands[i].name) == 0)
    {
      argv[0] = line;
      const int argc = 1 + tokenize(args, &argv[1], k_commands[i].max_tokens - 1);
      k_commands[i].run(argc, argv);
      scan_loops(false);
      return;
    }
  }
  reply("ERR unknown command");
}

static void end_of_line(void)
{
  if (s_line_overflow)
  {
    reply("ERR line too long");
  }
  else
  {
    s_line[s_line_len] = '\0';
    execute_line(s_line);
  }
  s_line_len = 0;
  s_line_overflow = false;
}

static void feed_byte(uint8_t byte)
{
  if (byte == '\r')
  {
    return;
  }
  if (byte == '\n')
  {
    end_of_line();
  }
  else if (s_line_len < MAX_LINE_CHARS)
  {
    s_line[s_line_len++] = (char)byte;
  }
  else
  {
    s_line_overflow = true;
  }
}

/* Room for the longest single reply; commands wait for it. */
static bool tx_has_room(void)
{
  return !CDC_IsConfigured() || CDC_TxFree() >= REPLY_BUF_SIZE;
}

static void run_job(void)
{
  while (s_job.kind != JOB_NONE && tx_has_room())
  {
    if (!CDC_IsConfigured())
    {
      s_job.kind = JOB_NONE;
      return;
    }
    step_job();
  }
}

static void emit_events(void)
{
  if (s_pending_play >= 0)
  {
    reply("EVT PLAY %d", s_pending_play);
    s_pending_play = -1;
  }

  const uint32_t now = HAL_GetTick();
  if (now - s_loop_polled_at >= LOOP_POLL_MS)
  {
    s_loop_polled_at = now;
    scan_loops(true);
  }
}

void usb_cmd_poll(void)
{
  uint8_t byte;

  run_job();
  while (s_job.kind == JOB_NONE && tx_has_room() && CDC_Read(&byte, 1) > 0u)
  {
    feed_byte(byte);
    run_job();
  }
  if (s_job.kind == JOB_NONE && tx_has_room())
  {
    emit_events();
  }
  CDC_Poll();
}

void usb_cmd_notify_play(bool playing)
{
  s_pending_play = playing ? 1 : 0;
}
