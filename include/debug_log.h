#ifndef DEBUG_LOG_H_
#define DEBUG_LOG_H_

/**
 * The debug log: everything narval reports about itself (config errors,
 * packages, language servers, debug adapters, the daemon, appDebug()
 * traces), kept in memory from the moment the process starts and shown in
 * the "*debug-log*" buffer (CTRL-x h l, or M-x debug-log). Debug builds
 * (APP_DEBUG) also write every line to ~/.narval/debug.log as before.
 *
 * Two rings, so a chatty language server can't push the startup out:
 * errors, warnings and info lines are kept in one (the last
 * DEBUG_LOG_KEEP), debug lines in the other (the last DEBUG_LOG_KEEP too).
 */

#include <stdarg.h>
#include <stddef.h>

struct buffer_t;

#define DEBUG_LOG_KEEP 2000
// A longer line (a language server's whole JSON reply) is cut to this
// many bytes in memory; debug.log still gets all of it.
#define DEBUG_LOG_TEXT_MAX 400
#define DEBUG_LOG_BUFFER_NAME "*debug-log*"
#define DEBUG_LOG_PREFIX_NAME "debug-log"

typedef enum {
  LOG_ERROR = 0,
  LOG_WARN = 1,
  LOG_INFO = 2,
  LOG_DEBUG = 3
} LogLevel;

typedef struct {
  unsigned long seq; // 1, 2, 3... in the order lines were logged
  long long ms;      // since the process started
  LogLevel level;
  char *text;        // one line, no control characters
} LogEntry;

// Logs one line (printf-style) at `level`. Safe at any time, before the
// editor is set up and after debugLogFree() (then it's dropped).
void logMessage(LogLevel level, const char *fmt, ...);
void logMessageV(LogLevel level, const char *fmt, va_list ap);

// Shorthands.
#define logError(...) logMessage(LOG_ERROR, __VA_ARGS__)
#define logWarn(...) logMessage(LOG_WARN, __VA_ARGS__)
#define logInfo(...) logMessage(LOG_INFO, __VA_ARGS__)

// The traces sprinkled through the code: a LOG_DEBUG line. The name
// predates the in-memory log.
void appDebug(const char *fmt, ...);
// Only to ~/.narval/debug.log (APP_DEBUG builds), never kept in memory:
// for what happens on every key press.
void appTrace(const char *fmt, ...);

// Path of the debug log file (opened by this call if not yet), or NULL
// when built without APP_DEBUG. Lets another subsystem (a spawned
// language server, a debug adapter) send its own stderr there instead of
// onto the terminal narval is drawing on.
const char *appDebugLogPath(void);
// Closes the debug log file at exit.
void appDebugClose(void);

// "error", "warn", "info", "debug"; -1 from the name when it's none.
const char *logLevelName(LogLevel level);
int logLevelFromName(const char *name);

// When the process started, in os_monotonic_ms() time: the first call
// (main() makes it first thing) fixes it.
long long logStartMs(void);
// The seq of the last line logged, 0 before any.
unsigned long logLastSeq(void);
// Every kept line after `after_seq` at `max_level` or more important, oldest
// first. Stops early when `fn` returns non-zero.
void logForEach(unsigned long after_seq, LogLevel max_level,
                int (*fn)(const LogEntry *entry, void *data), void *data);
// One line as the buffer shows it: "[   1.234] warn  text".
void logFormatEntry(const LogEntry *entry, char *out, size_t size);
// Forgets every kept line (seq keeps counting).
void logClear(void);
// Frees the kept lines; later lines are dropped.
void debugLogFree(void);

// ─── The *debug-log* buffer ─────────────────────────────────────────────────

// Opens (or comes back to) *debug-log* in the active pane, following the
// end, showing `max_level` and more important lines.
void debugLogOpen(LogLevel max_level);
// Called every tick: appends what was logged since to *debug-log* if it's
// open, following the end while the cursor is on the last line.
void debugLogTick(void);
// Shows the next level of detail in *debug-log* (debug -> info -> warn ->
// error -> debug).
void debugLogCycleLevel(void);
// Forgets every kept line and empties *debug-log*.
void debugLogClearBuffer(void);
// The level *debug-log* currently shows.
LogLevel debugLogShownLevel(void);

// Appends to `buf` every line after `after_seq` at `max_level` or more
// important. Returns the seq of the last line looked at (after_seq when
// nothing new), to pass back next time.
unsigned long debugLogAppendTo(struct buffer_t *buf, unsigned long after_seq, LogLevel max_level);

#endif // DEBUG_LOG_H_
