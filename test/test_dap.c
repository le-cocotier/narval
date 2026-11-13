/**
 * @file test_dap.c
 * @brief Unit tests for the debugger's output pane (dap.c): what the
 * adapter's output events become, line by line.
 */

#include "test.h"
#include "../include/dap.h"
#include "../include/galloc.h"
#include "../include/os.h"
#include "../include/split_screen.h"
#include "../include/utils.h"

#include <stdio.h>

// Feeds one output event, framed as the adapter would send it.
static void output(const char *body) {
  char *json = utilFormat("{\"seq\":1,\"type\":\"event\",\"event\":\"output\",\"body\":%s}", body);
  char *msg = utilFormat("Content-Length: %d\r\n\r\n%s", (int)strlen(json), json);
  dapFeed(msg, strlen(msg));
  gfree(msg);
  gfree(json);
}

static const char *line(const int i) {
  int count;
  const char *const *lines = dapOutputTail(&count);
  return i < count ? lines[i] : "(none)";
}

static int lineCount(void) {
  int count;
  dapOutputTail(&count);
  return count;
}

static void testLinesAcrossEvents(void) {
  int base = lineCount();
  output("{\"category\":\"stdout\",\"output\":\"a\"}");
  output("{\"category\":\"stdout\",\"output\":\"b\\r\\nc\\n\"}");
  CHECK_INT(lineCount(), base + 2);
  CHECK_STR(line(base), "ab");
  CHECK_STR(line(base + 1), "c");
}

// debugpy sends a logpoint on "stdout" with a source, from another process
// than the program's writes: it can come between the pieces of a print().
static void testLogpointInsideAProgramLine(void) {
  int base = lineCount();
  output("{\"category\":\"stdout\",\"output\":\"total\"}");
  output("{\"category\":\"stdout\",\"output\":\"i is 3\\n\",\"source\":{}}");
  output("{\"category\":\"stdout\",\"output\":\" 10\\n\"}");
  CHECK_INT(lineCount(), base + 2);
  CHECK_STR(line(base), "i is 3");
  CHECK_STR(line(base + 1), "total 10");
}

// A line the program's stderr left open isn't joined by its stdout, nor
// by a console message.
static void testStreamsKeepTheirOwnLines(void) {
  int base = lineCount();
  output("{\"category\":\"stderr\",\"output\":\"warn\"}");
  output("{\"category\":\"stdout\",\"output\":\"out\\n\"}");
  output("{\"category\":\"console\",\"output\":\"note\\n\"}");
  output("{\"category\":\"stderr\",\"output\":\"ing\\n\"}");
  CHECK_INT(lineCount(), base + 3);
  CHECK_STR(line(base), "out");
  CHECK_STR(line(base + 1), "note");
  CHECK_STR(line(base + 2), "warning");
}

static void testTelemetryIsDropped(void) {
  int base = lineCount();
  output("{\"category\":\"telemetry\",\"output\":\"ptvsd\\n\"}");
  CHECK_INT(lineCount(), base);
}

int main(void) {
  // An "adapter" that reads its stdin and says nothing: the events come
  // from dapFeed(). It's a POSIX shell command.
  const char *const adapter[] = {"sh", "-c", "exec cat >/dev/null", NULL};
  testInit();
  splitScreenInit(); // dapStart() opens the debug panel beside the code
  if (strcmp(os_name(), "windows") != 0) {
    CHECK_INT(dapRegisterAdapter("t", adapter, NULL), 0);
    CHECK_INT(dapStart("t", "{}"), 0);
    testLinesAcrossEvents();
    testLogpointInsideAProgramLine();
    testStreamsKeepTheirOwnLines();
    testTelemetryIsDropped();
  }
  dapFree();
  return testDone();
}
