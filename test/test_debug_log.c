/**
 * @file test_debug_log.c
 * @brief Unit tests for the in-memory debug log (debug_log.c): levels,
 * one-line text, the two rings (debug lines can't push the startup out),
 * merging back into order, and appending to the *debug-log* buffer.
 */

#include "test.h"
#include "../include/buffer.h"
#include "../include/debug_log.h"
#include "../include/galloc.h"

typedef struct {
  int count;
  char lines[8][DEBUG_LOG_TEXT_MAX + 64];
  unsigned long seqs[8];
  LogLevel levels[8];
} Collected;

static int collect(const LogEntry *entry, void *data) {
  Collected *c = data;
  if (c->count < 8) {
    snprintf(c->lines[c->count], sizeof(c->lines[0]), "%s", entry->text);
    c->seqs[c->count] = entry->seq;
    c->levels[c->count] = entry->level;
  }
  c->count++;
  return 0;
}

static Collected collectAll(const unsigned long after, const LogLevel level) {
  Collected c;
  memset(&c, 0, sizeof(c));
  logForEach(after, level, collect, &c);
  return c;
}

static void testLevelNames(void) {
  CHECK_STR(logLevelName(LOG_ERROR), "error");
  CHECK_STR(logLevelName(LOG_DEBUG), "debug");
  CHECK_INT(logLevelFromName("warn"), LOG_WARN);
  CHECK_INT(logLevelFromName("warning"), LOG_WARN);
  CHECK_INT(logLevelFromName("info"), LOG_INFO);
  CHECK_INT(logLevelFromName("loud"), -1);
  CHECK_INT(logLevelFromName(NULL), -1);
}

static void testOrderAndFilter(void) {
  Collected c;
  const unsigned long before = logLastSeq();
  logClear();
  logInfo("narval %s starting", "4.1");
  appDebug("[lsp →] {\"id\": %d}", 1);
  logError("[config] init.lisp:%d: unbound symbol", 12);
  logWarn("[lsp] %s exited", "clangd");

  CHECK_INT(logLastSeq(), before + 4);
  // Everything, in the order it was logged.
  c = collectAll(0, LOG_DEBUG);
  CHECK_INT(c.count, 4);
  CHECK_STR(c.lines[0], "narval 4.1 starting");
  CHECK_STR(c.lines[1], "[lsp →] {\"id\": 1}");
  CHECK_STR(c.lines[2], "[config] init.lisp:12: unbound symbol");
  CHECK_STR(c.lines[3], "[lsp] clangd exited");
  CHECK(c.seqs[0] < c.seqs[1] && c.seqs[1] < c.seqs[2] && c.seqs[2] < c.seqs[3]);

  // Warnings and errors only.
  c = collectAll(0, LOG_WARN);
  CHECK_INT(c.count, 2);
  CHECK_INT(c.levels[0], LOG_ERROR);
  CHECK_INT(c.levels[1], LOG_WARN);

  // Only what came after a seq.
  c = collectAll(before + 2, LOG_DEBUG);
  CHECK_INT(c.count, 2);
  CHECK_STR(c.lines[0], "[config] init.lisp:12: unbound symbol");
  c = collectAll(logLastSeq(), LOG_DEBUG);
  CHECK_INT(c.count, 0);
}

static void testOneLine(void) {
  Collected c;
  char longer[DEBUG_LOG_TEXT_MAX + 100];
  logClear();
  logInfo("first\nsecond\tthird\r\n\n");
  c = collectAll(0, LOG_DEBUG);
  CHECK_INT(c.count, 1);
  CHECK_STR(c.lines[0], "first second third");

  // Cut, on a character boundary, with an ellipsis.
  memset(longer, 'a', sizeof(longer));
  memcpy(longer + DEBUG_LOG_TEXT_MAX - 1, "é", 2); // straddles the cut
  longer[sizeof(longer) - 1] = '\0';
  logClear();
  logInfo("%s", longer);
  c = collectAll(0, LOG_DEBUG);
  CHECK_INT((long)strlen(c.lines[0]), DEBUG_LOG_TEXT_MAX - 1 + (long)strlen("…"));
  CHECK(strcmp(c.lines[0] + DEBUG_LOG_TEXT_MAX - 1, "…") == 0);
}

typedef struct {
  int count;
  char first[64];
  int infos;
} Counted;

static int count(const LogEntry *entry, void *data) {
  Counted *c = data;
  if (c->count == 0)
    snprintf(c->first, sizeof(c->first), "%s", entry->text);
  c->count++;
  if (entry->level == LOG_INFO)
    c->infos++;
  return 0;
}

static void testDebugCantPushStartupOut(void) {
  Counted c;
  int i;
  logClear();
  logInfo("narval starting");
  for (i = 0; i < DEBUG_LOG_KEEP + 500; i++)
    appDebug("[lsp ←] message %d", i);

  memset(&c, 0, sizeof(c));
  logForEach(0, LOG_DEBUG, count, &c);
  // The info line is still there, and first; only the oldest debug lines went.
  CHECK_INT(c.count, DEBUG_LOG_KEEP + 1);
  CHECK_INT(c.infos, 1);
  CHECK_STR(c.first, "narval starting");

  memset(&c, 0, sizeof(c));
  logForEach(0, LOG_INFO, count, &c);
  CHECK_INT(c.count, 1);
}

static int stopAtTwo(const LogEntry *entry, void *data) {
  int *n = data;
  (void)entry;
  return ++*n == 2;
}

static void testStopEarly(void) {
  int n = 0;
  logClear();
  logInfo("a");
  logInfo("b");
  logInfo("c");
  logForEach(0, LOG_DEBUG, stopAtTwo, &n);
  CHECK_INT(n, 2);
}

static void testFormat(void) {
  LogEntry entry;
  char out[128];
  entry.seq = 1;
  entry.ms = 1234;
  entry.level = LOG_WARN;
  entry.text = "[lsp] clangd exited";
  logFormatEntry(&entry, out, sizeof(out));
  CHECK_STR(out, "[    1.234] warn  [lsp] clangd exited");
}

static void testAppendToBuffer(void) {
  Buffer *buf = testBufferFromText("");
  unsigned long seq;
  char *text;
  // testBufferFromText("") leaves one empty row: start from none, as the
  // *debug-log* buffer does.
  bufferFreeRow(&buf->row[0]);
  buf->numrows = 0;

  logClear();
  logInfo("one");
  appDebug("two");
  seq = debugLogAppendTo(buf, 0, LOG_INFO);
  CHECK_INT(buf->numrows, 1);
  CHECK_INT((long)seq, (long)logLastSeq());

  // Only the new lines are appended.
  logError("three");
  seq = debugLogAppendTo(buf, seq, LOG_INFO);
  CHECK_INT(buf->numrows, 2);
  text = testBufferText(buf);
  CHECK(text && strstr(text, "info  one\n") != NULL);
  CHECK(text && strstr(text, "error three") != NULL);
  CHECK(text && strstr(text, "two") == NULL);
  gfree(text);

  // Nothing new: nothing appended, same seq back.
  CHECK_INT((long)debugLogAppendTo(buf, seq, LOG_DEBUG), (long)seq);
  CHECK_INT(buf->numrows, 2);
}

static void testFreedDropsLines(void) {
  Collected c;
  debugLogFree();
  logError("after the end");
  c = collectAll(0, LOG_DEBUG);
  CHECK_INT(c.count, 0);
}

int main(void) {
  testInit();
  testLevelNames();
  testOrderAndFilter();
  testOneLine();
  testDebugCantPushStartupOut();
  testStopEarly();
  testFormat();
  testAppendToBuffer();
  testFreedDropsLines();
  return testDone();
}
