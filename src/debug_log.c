/**
 * @file debug_log.c
 * @brief The debug log, in memory from the start and in the *debug-log*
 * buffer (see debug_log.h), plus ~/.narval/debug.log in APP_DEBUG builds.
 */

#include "../include/debug_log.h"

#include <stdio.h>
#include <string.h>

#include "../include/buffer.h"
#include "../include/builtins.h"
#include "../include/data.h"
#include "../include/editor_op.h"
#include "../include/galloc.h"
#include "../include/os.h"
#include "../include/split_screen.h"
#include "../include/utf8.h"
#include "../include/utils.h"

extern EditorConfig E;

/***********************************************************************************************************************
 *
 *                                                 ~/.narval/debug.log
 *
 **********************************************************************************************************************/

#ifdef APP_DEBUG
static const char *debugLogPath(void) {
  static char path[OS_PATH_MAX];
  if (!path[0])
    snprintf(path, sizeof(path), "%s/debug.log", utilNarvalDir());
  return path;
}

/**
 * @brief Opens (once) and returns the debug log file.
 * @details Never stderr: this process's stderr is the terminal it draws
 * its UI on. Truncated on open so each run starts its own log, and
 * flushed after every line so a crash or a hang doesn't lose the lines
 * right before it.
 */
static FILE *s_file = NULL;
static int s_file_tried = 0;

static FILE *debugLogFile(void) {
  if (!s_file_tried) {
    s_file_tried = 1;
    s_file = fopen(debugLogPath(), "w");
    if (s_file) {
      setvbuf(s_file, NULL, _IOLBF, 0);
      os_hand_back(debugLogPath());
    }
  }
  return s_file;
}

static void fileWriteV(const char *level, const char *fmt, va_list ap) {
  FILE *f = debugLogFile();
  long long ms;
  if (!f)
    return;
  // Timed like *debug-log*: from the start of the process.
  ms = os_monotonic_ms() - logStartMs();
  fprintf(f, "[%5lld.%03lld] ", ms / 1000, ms % 1000);
  if (level)
    fprintf(f, "%s: ", level);
  vfprintf(f, fmt, ap);
  fputc('\n', f);
  fflush(f);
}
#endif

void appDebugClose(void) {
#ifdef APP_DEBUG
  // Stays closed: lines after this go nowhere on disk.
  if (s_file)
    fclose(s_file);
  s_file = NULL;
  s_file_tried = 1;
#endif
}

const char *appDebugLogPath(void) {
#ifdef APP_DEBUG
  return debugLogFile() ? debugLogPath() : NULL;
#else
  return NULL;
#endif
}

void appTrace(const char *fmt, ...) {
#ifdef APP_DEBUG
  va_list ap;
  va_start(ap, fmt);
  fileWriteV(NULL, fmt, ap);
  va_end(ap);
#endif
  (void)fmt;
}

/***********************************************************************************************************************
 *
 *                                                 In memory
 *
 **********************************************************************************************************************/

typedef struct {
  LogEntry entries[DEBUG_LOG_KEEP];
  int start; // oldest
  int count;
} Ring;

// 0: errors, warnings, info. 1: debug.
static Ring s_rings[2];
static unsigned long s_seq = 0;
static long long s_start_ms = -1;
static int s_closed = 0;

static const char *const s_level_names[] = {"error", "warn", "info", "debug"};

const char *logLevelName(const LogLevel level) {
  if (level < LOG_ERROR || level > LOG_DEBUG)
    return "?";
  return s_level_names[level];
}

int logLevelFromName(const char *name) {
  int i;
  if (!name)
    return -1;
  for (i = 0; i <= LOG_DEBUG; i++)
    if (strcmp(name, s_level_names[i]) == 0)
      return i;
  if (strcmp(name, "warning") == 0)
    return LOG_WARN;
  return -1;
}

long long logStartMs(void) {
  if (s_start_ms < 0)
    s_start_ms = os_monotonic_ms();
  return s_start_ms;
}

unsigned long logLastSeq(void) { return s_seq; }

/**
 * @brief `text` as one line: control characters (a message's own "\n",
 * a tab) as spaces, trailing ones dropped, cut on a character boundary.
 */
static char *oneLine(const char *text) {
  size_t n = strlen(text);
  size_t cut;
  size_t i;
  char *out;
  int cut_short = 0;
  while (n > 0 && ((unsigned char)text[n - 1] < 0x20 || text[n - 1] == ' '))
    n--;
  if (n > DEBUG_LOG_TEXT_MAX) {
    cut = DEBUG_LOG_TEXT_MAX;
    while (cut > 0 && ((unsigned char)text[cut] & 0xC0) == 0x80)
      cut--;
    n = cut;
    cut_short = 1;
  }
  out = galloc(n + (cut_short ? sizeof("…") : 1));
  if (!out)
    return NULL;
  for (i = 0; i < n; i++)
    out[i] = (unsigned char)text[i] < 0x20 || text[i] == 0x7f ? ' ' : text[i];
  out[n] = '\0';
  if (cut_short)
    strcat(out, "…");
  return out;
}

static void keep(const LogLevel level, const char *text) {
  Ring *ring = &s_rings[level == LOG_DEBUG ? 1 : 0];
  LogEntry *slot;
  char *line = oneLine(text);
  if (!line)
    return;
  if (ring->count == DEBUG_LOG_KEEP) {
    slot = &ring->entries[ring->start];
    gfree(slot->text);
    ring->start = (ring->start + 1) % DEBUG_LOG_KEEP;
    ring->count--;
  }
  slot = &ring->entries[(ring->start + ring->count) % DEBUG_LOG_KEEP];
  slot->seq = ++s_seq;
  slot->ms = os_monotonic_ms() - logStartMs();
  slot->level = level;
  slot->text = line;
  ring->count++;
}

void logMessageV(const LogLevel level, const char *fmt, va_list ap) {
  char text[2048];
#ifdef APP_DEBUG
  va_list copy;
  va_copy(copy, ap);
  fileWriteV(logLevelName(level), fmt, copy);
  va_end(copy);
#endif
  if (s_closed)
    return;
  logStartMs();
  vsnprintf(text, sizeof(text), fmt, ap);
  keep(level < LOG_ERROR || level > LOG_DEBUG ? LOG_DEBUG : level, text);
}

void logMessage(const LogLevel level, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  logMessageV(level, fmt, ap);
  va_end(ap);
}

void appDebug(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  logMessageV(LOG_DEBUG, fmt, ap);
  va_end(ap);
}

static const LogEntry *ringAt(const Ring *ring, const int i) {
  return &ring->entries[(ring->start + i) % DEBUG_LOG_KEEP];
}

// The index of the first entry after `seq` (entries are in seq order).
static int ringFirstAfter(const Ring *ring, const unsigned long seq) {
  int lo = 0;
  int hi = ring->count;
  int mid;
  while (lo < hi) {
    mid = (lo + hi) / 2;
    if (ringAt(ring, mid)->seq <= seq)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

void logForEach(const unsigned long after_seq, const LogLevel max_level,
                int (*fn)(const LogEntry *entry, void *data), void *data) {
  const Ring *main = &s_rings[0];
  const Ring *debug = &s_rings[1];
  int i = ringFirstAfter(main, after_seq);
  int j = max_level >= LOG_DEBUG ? ringFirstAfter(debug, after_seq) : debug->count;
  const LogEntry *a;
  const LogEntry *b;
  const LogEntry *next;

  // Merged back into the order they were logged in.
  while (i < main->count || j < debug->count) {
    a = i < main->count ? ringAt(main, i) : NULL;
    b = j < debug->count ? ringAt(debug, j) : NULL;
    if (a && (!b || a->seq < b->seq)) {
      next = a;
      i++;
    } else {
      next = b;
      j++;
    }
    if (next->level > max_level)
      continue;
    if (fn(next, data))
      return;
  }
}

void logFormatEntry(const LogEntry *entry, char *out, const size_t size) {
  snprintf(out, size, "[%5lld.%03lld] %-5s %s", entry->ms / 1000, entry->ms % 1000,
           logLevelName(entry->level), entry->text ? entry->text : "");
}

void logClear(void) {
  int r;
  int i;
  for (r = 0; r < 2; r++) {
    for (i = 0; i < s_rings[r].count; i++)
      gfree(ringAt(&s_rings[r], i)->text);
    s_rings[r].start = 0;
    s_rings[r].count = 0;
  }
}

void debugLogFree(void) {
  logClear();
  s_closed = 1;
}

/***********************************************************************************************************************
 *
 *                                                 The *debug-log* buffer
 *
 **********************************************************************************************************************/

static int s_buffer_id = -1;
static unsigned long s_shown_seq = 0;
static LogLevel s_shown_level = LOG_DEBUG;

LogLevel debugLogShownLevel(void) { return s_shown_level; }

typedef struct {
  Buffer *buf;
  unsigned long last;
} AppendState;

static int appendOne(const LogEntry *entry, void *data) {
  AppendState *state = data;
  char line[DEBUG_LOG_TEXT_MAX + 64];
  Utf8Char *u;
  logFormatEntry(entry, line, sizeof(line));
  u = stringToUtf8((unsigned char *)line);
  if (u) {
    bufferInsertRow(state->buf, state->buf->numrows, u, utf8StringLen(u) - 1);
    gfree(u);
  }
  return 0;
}

unsigned long debugLogAppendTo(Buffer *buf, const unsigned long after_seq,
                               const LogLevel max_level) {
  AppendState state;
  state.buf = buf;
  logForEach(after_seq, max_level, appendOne, &state);
  buf->dirty = 0;
  // Every seq up to the last one has been looked at, shown or not.
  return s_seq > after_seq ? s_seq : after_seq;
}

static Buffer *logBuffer(void) {
  Buffer *buf = bufferFindById(s_buffer_id);
  return buf && buf->type == EXPLORER_BUFF ? buf : NULL;
}

static void clearRows(Buffer *buf) {
  int i;
  for (i = 0; i < buf->numrows; i++)
    bufferFreeRow(&buf->row[i]);
  gfree(buf->row);
  buf->row = NULL;
  buf->numrows = 0;
  buf->x = 0;
  buf->y = 0;
}

static void render(Buffer *buf) {
  clearRows(buf);
  s_shown_seq = debugLogAppendTo(buf, 0, s_shown_level);
  buf->y = buf->numrows > 0 ? buf->numrows - 1 : 0;
}

void debugLogOpen(const LogLevel max_level) {
  Buffer *buf = logBuffer();
  EditorPane *active;
  struct prefix_t prefix;
  Utf8Char *name;

  if (!buf) {
    name = stringToUtf8((unsigned char *)DEBUG_LOG_BUFFER_NAME);
    if (!name)
      return;
    s_buffer_id = bufferCreate(name, READ_ONLY, EXPLORER_BUFF);
    gfree(name);
    buf = logBuffer();
    if (!buf)
      return;
    snprintf(buf->focus_prefix, sizeof(buf->focus_prefix), "%s", DEBUG_LOG_PREFIX_NAME);
  }
  s_shown_level = max_level;
  render(buf);

  active = splitScreenGetActivePane();
  active->buffer_id = s_buffer_id;
  active->shown_buffer_id = -1;
  prefix = find_prefix(DEBUG_LOG_PREFIX_NAME);
  if (prefix.prefix_id != -1)
    E.prefix_state = prefix.prefix_id;
  editorSetStatusMessage("*debug-log*: %s and above. l level, c clear, CTRL-x leave",
                         logLevelName(s_shown_level));
}

void debugLogTick(void) {
  Buffer *buf;
  int at_end;
  if (s_seq == s_shown_seq)
    return;
  buf = logBuffer();
  if (!buf) {
    s_shown_seq = s_seq;
    return;
  }
  at_end = buf->y >= buf->numrows - 1;
  // The kept lines are bounded, the buffer would not be: start again
  // from what's kept once it has grown well past that.
  if (buf->numrows > 2 * DEBUG_LOG_KEEP + DEBUG_LOG_KEEP / 2) {
    render(buf);
    return;
  }
  s_shown_seq = debugLogAppendTo(buf, s_shown_seq, s_shown_level);
  if (at_end)
    buf->y = buf->numrows > 0 ? buf->numrows - 1 : 0;
}

void debugLogCycleLevel(void) {
  Buffer *buf = logBuffer();
  s_shown_level = s_shown_level == LOG_ERROR ? LOG_DEBUG : (LogLevel)(s_shown_level - 1);
  if (buf)
    render(buf);
  editorSetStatusMessage("*debug-log*: %s and above", logLevelName(s_shown_level));
}

void debugLogClearBuffer(void) {
  Buffer *buf = logBuffer();
  logClear();
  if (buf)
    render(buf);
  editorSetStatusMessage("*debug-log* cleared");
}
