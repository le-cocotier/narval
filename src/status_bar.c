/**
 * @file status_bar.c
 * @brief The status bar, the second-to-last screen row, built from
 * segments that init.lisp can reorder, drop, or add to.
 */

#include "../include/status_bar.h"
#include "../include/append_buffer.h"
#include "../include/buffer.h"
#include "../include/builtins.h"
#include "../include/color.h"
#include "../include/dap.h"
#include "../include/editor_op.h"
#include "../include/galloc.h"
#include "../include/lsp_ui.h"
#include "../include/macro.h"
#include "../include/os.h"
#include "../include/project.h"
#include "../include/split_screen.h"
#include "../include/utf8.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

extern EditorConfig E;

//  ▌ 1:main.c* ▐ develop* E2 W1 ● REC  point › main   2:buffer.c 3:*search*   c  Ln 42/310, Col 7  13%
//  └ buffer ──┘ └ git, diagnostics, macro ┘ └ breadcrumbs ┘ └ buffers ───┘   └ kind, position, percent ┘
//
// (status-bar-segments left right) picks which segments show and in what
// order; (status-bar-add-segment "name" fn) adds one computed in Lisp.
// The "buffer" segment always comes first and the right side always ends
// the row, so neither can be pushed off the screen: when the row is too
// narrow, "buffers" and then "breadcrumbs" give up their room first, then
// the Lisp segments, then the others from the end of the list, and the
// right side falls back to "line:col".

// ─── segments ───────────────────────────────────────────────────────────────

typedef enum {
  SEG_BUFFER,
  SEG_GIT,
  SEG_DIAGNOSTICS,
  SEG_MACRO,
  SEG_DEBUG,
  SEG_MODE,
  SEG_PANES,
  SEG_BREADCRUMBS,
  SEG_BUFFERS,
  SEG_KIND,
  SEG_POSITION,
  SEG_PERCENT,
  SEG_CLOCK,
  SEG_PROJECT,
  SEG_BUILTIN_COUNT,
  SEG_CUSTOM = 100 // + index into s_custom
} SegmentId;

static const char *const s_names[SEG_BUILTIN_COUNT] = {
    "buffer", "git",     "diagnostics", "macro",   "debug", "mode",  "panes",
    "breadcrumbs", "buffers", "kind", "position", "percent", "clock", "project",
};

#define STATUS_MAX_SEGMENTS 24
#define STATUS_MAX_CUSTOM 16
#define STATUS_CUSTOM_REFRESH_MS 500

// The layout a config starts from (statusBarReset() goes back to it).
static const int s_default_left[] = {SEG_BUFFER, SEG_GIT,   SEG_DIAGNOSTICS, SEG_MACRO,
                                     SEG_DEBUG,  SEG_MODE,  SEG_PANES,       SEG_BREADCRUMBS,
                                     SEG_BUFFERS};
static const int s_default_right[] = {SEG_KIND, SEG_POSITION, SEG_PERCENT};

static int s_left[STATUS_MAX_SEGMENTS] = {SEG_BUFFER, SEG_GIT,   SEG_DIAGNOSTICS, SEG_MACRO,
                                          SEG_DEBUG,  SEG_MODE,  SEG_PANES,       SEG_BREADCRUMBS,
                                          SEG_BUFFERS};
static int s_left_count = 9;
static int s_right[STATUS_MAX_SEGMENTS] = {SEG_KIND, SEG_POSITION, SEG_PERCENT};
static int s_right_count = 3;

typedef struct {
  char name[32];
  Lisp fn;          // called with no arguments, returns a string ("" hides it)
  char text[128];   // its last result
  int failed;       // raised an error once: not called again
} CustomSegment;

static CustomSegment s_custom[STATUS_MAX_CUSTOM];
static int s_custom_count = 0;
static long long s_custom_refreshed_ms = 0;

static int segmentByName(const char *name) {
  int i;
  for (i = 0; i < SEG_BUILTIN_COUNT; i++)
    if (!strcmp(s_names[i], name)) return i;
  for (i = 0; i < s_custom_count; i++)
    if (!strcmp(s_custom[i].name, name)) return SEG_CUSTOM + i;
  return -1;
}

void statusBarReset(void) {
  s_left_count = (int)(sizeof(s_default_left) / sizeof(s_default_left[0]));
  memcpy(s_left, s_default_left, sizeof(s_default_left));
  s_right_count = (int)(sizeof(s_default_right) / sizeof(s_default_right[0]));
  memcpy(s_right, s_default_right, sizeof(s_default_right));
  memset(s_custom, 0, sizeof(s_custom));
  s_custom_count = 0;
  s_custom_refreshed_ms = 0;
}

void statusBarVisitLispRoots(LispRootVisitor visit, LispContext ctx) {
  int i;
  for (i = 0; i < s_custom_count; i++)
    visit(&s_custom[i].fn, ctx);
}

// The Lisp segments are called at most every STATUS_CUSTOM_REFRESH_MS,
// not on each ~50 ms redraw tick, and one that raises an error is turned
// off (with a message) rather than failing on every frame.
static void refreshCustomSegments(void) {
  LispError err;
  Lisp result;
  int i;
  long long now = os_monotonic_ms();
  if (now - s_custom_refreshed_ms < STATUS_CUSTOM_REFRESH_MS) return;
  s_custom_refreshed_ms = now;
  for (i = 0; i < s_custom_count; i++) {
    if (s_custom[i].failed) continue;
    err = LISP_ERROR_NONE;
    result = lisp_apply(s_custom[i].fn, lisp_null(), &err, E.ctx);
    if (err != LISP_ERROR_NONE) {
      s_custom[i].failed = 1;
      s_custom[i].text[0] = '\0';
      editorSetStatusMessage("status bar segment \"%s\" raised an error; it's off until the config reloads (CTRL-x c r)",
                             s_custom[i].name);
      continue;
    }
    if (lisp_type(result) == LISP_STRING)
      snprintf(s_custom[i].text, sizeof(s_custom[i].text), "%s", lisp_string(result));
    else
      s_custom[i].text[0] = '\0';
  }
}

// ─── text helpers ───────────────────────────────────────────────────────────

// Terminal columns taken by a UTF-8 string (wide characters count 2).
static int textCols(const char *s) {
  Utf8Char c;
  int cols = 0;
  while (*s) {
    s += utf8CreateChar((const unsigned char *)s, &c);
    cols += utf8CharWidth(&c);
  }
  return cols;
}

// Append at most max_cols columns of s; returns the columns written. A
// string that doesn't fit is cut one column short and ends with "…".
static int appendClipped(struct abuf *ab, const char *s, int max_cols) {
  Utf8Char c;
  const char *p = s;
  int cols = 0;
  int n;
  int w;
  if (max_cols <= 0) return 0;
  if (textCols(s) <= max_cols) {
    abAppend(ab, s, (int)strlen(s));
    return textCols(s);
  }
  while (*p) {
    n = utf8CreateChar((const unsigned char *)p, &c);
    w = utf8CharWidth(&c);
    if (cols + w > max_cols - 1) break;
    abAppend(ab, p, n);
    cols += w;
    p += n;
  }
  abAppend(ab, "\xe2\x80\xa6", 3); // …
  return cols + 1;
}

static void appendStr(struct abuf *ab, const char *s) { abAppend(ab, s, (int)strlen(s)); }

// "3:name*" for one buffer.
static void bufferLabel(const Buffer *b, char *out, size_t size) {
  char *name = b->filename ? (char *)utf8ToString(b->filename) : NULL;
  snprintf(out, size, "%d:%s%s", b->buffer_id, name ? name : "[No Name]", b->dirty ? "*" : "");
  gfree(name);
}

// What the buffer holds: the syntax override, the file's extension, or a
// word for the non-file buffer kinds; then CRLF and RO when they apply.
static void bufferKind(const Buffer *b, char *out, size_t size) {
  out[0] = '\0';
  if (b->type == TERMINAL_BUFF) {
    snprintf(out, size, "terminal");
  } else if (b->syntax[0]) {
    snprintf(out, size, "%s", b->syntax);
  } else if (b->filename) {
    utf8FilenameExtension(b->filename, out, size);
  }
  if (b->crlf && strlen(out) + 6 < size) strcat(out, out[0] ? " CRLF" : "CRLF");
  if (b->state == READ_ONLY && strlen(out) + 4 < size) strcat(out, out[0] ? " RO" : "RO");
}

// ─── one segment's text ─────────────────────────────────────────────────────

typedef struct {
  int id;
  char text[512];
  const char *color; // fg over the bar, NULL for the bar's own
  int dim;
  int priority; // dropped lowest first when the row is too narrow
  int fill;     // takes what's left, clipped (buffers, breadcrumbs)
  int cols;
} Piece;

static void segmentText(const int id, const Buffer *buf, Piece *p) {
  ScreenLayout *layout = splitScreenGetLayout();
  char label[300];
  int off;
  int errors;
  int warnings;
  int i;
  const Buffer *b;
  time_t now;
  struct tm *tm;

  p->id = id;
  p->text[0] = '\0';
  p->color = NULL;
  p->dim = 0;
  p->fill = 0;
  p->priority = 3;

  if (id >= SEG_CUSTOM) {
    if (s_custom[id - SEG_CUSTOM].text[0])
      snprintf(p->text, sizeof(p->text), " %s", s_custom[id - SEG_CUSTOM].text);
    p->priority = 2;
    return;
  }

  switch (id) {
  case SEG_BUFFER: // drawn on its own, in the accent block
    break;
  case SEG_GIT:
    if (buf && buf->git_available)
      snprintf(p->text, sizeof(p->text), " %s%s", buf->git_branch, buf->git_dirty ? "*" : "");
    break;
  case SEG_DIAGNOSTICS:
    if (!buf) break;
    errors = warnings = 0;
    for (i = 0; i < E.lsp_diagnostics.count; ++i) {
      if (E.lsp_diagnostics.entries[i].buffer_id != buf->buffer_id) continue;
      if (E.lsp_diagnostics.entries[i].severity == DIAG_ERROR) errors++;
      else if (E.lsp_diagnostics.entries[i].severity == DIAG_WARNING) warnings++;
    }
    // Two colors in one segment: the errors here, the warnings in a
    // second piece right after (see statusBarPieces()).
    if (errors) {
      snprintf(p->text, sizeof(p->text), " E%d", errors);
      p->color = E.theme.COLOR_REMOVED;
    } else if (warnings) {
      snprintf(p->text, sizeof(p->text), " W%d", warnings);
      p->color = E.theme.COLOR_NUMBER;
    }
    break;
  case SEG_MACRO:
    if (macroIsRecording()) {
      snprintf(p->text, sizeof(p->text), " \xe2\x97\x8f REC"); // ●
      p->color = E.theme.COLOR_REMOVED;
    }
    break;
  case SEG_DEBUG:
    if (dapState() == DAP_RUNNING || dapState() == DAP_STARTING)
      snprintf(p->text, sizeof(p->text), " [debug: running]");
    else if (dapState() == DAP_STOPPED)
      snprintf(p->text, sizeof(p->text), " [debug: paused]");
    break;
  case SEG_MODE:
    // Sticky prefixes are modes (terminal, explorer, ...); a one-shot
    // prefix like CTRL-x is already shown by which-key.
    if (E.prefix_state > 0 && E.prefix_state < E.number_of_prefix &&
        E.prefix[E.prefix_state].sticky)
      snprintf(p->text, sizeof(p->text), " [%s]", E.prefix[E.prefix_state].prefix_name);
    break;
  case SEG_PANES:
    if (layout->num_panes > 1)
      snprintf(p->text, sizeof(p->text), " [%d panes]", layout->num_panes);
    break;
  case SEG_PROJECT:
    if (projectCurrentRoot()) {
      const char *root = projectCurrentRoot();
      const char *slash = strrchr(root, '/');
      snprintf(p->text, sizeof(p->text), " %s", slash ? slash + 1 : root);
    }
    break;
  case SEG_BREADCRUMBS:
    p->fill = 1;
    p->priority = 1;
    if (E.constantes.BREADCRUMBS && buf && buf->type == FILE_BUFF) {
      p->text[0] = p->text[1] = ' ';
      if (lspUiBreadcrumbs(buf, buf->y, p->text + 2, sizeof(p->text) - 2) <= 0)
        p->text[0] = '\0';
    }
    break;
  case SEG_BUFFERS:
    p->fill = 1;
    p->priority = 0;
    p->dim = 1;
    if (!E.buffers || E.number_of_buffer < 2) break;
    off = 0;
    b = E.buffers;
    do {
      if (!buf || b->buffer_id != buf->buffer_id) {
        bufferLabel(b, label, sizeof(label));
        if (off < (int)sizeof(p->text))
          off += snprintf(p->text + off, sizeof(p->text) - off, "%s%s", off ? " " : "  ", label);
      }
      b = b->next;
    } while (b != E.buffers);
    break;
  case SEG_KIND:
    if (buf) {
      bufferKind(buf, label, sizeof(label));
      if (label[0]) snprintf(p->text, sizeof(p->text), "  %s", label);
    }
    break;
  case SEG_POSITION:
    if (buf && buf->type != TERMINAL_BUFF)
      snprintf(p->text, sizeof(p->text), "  Ln %d/%d, Col %d", buf->y + 1,
               buf->numrows ? buf->numrows : 1, buf->x + 1);
    break;
  case SEG_PERCENT:
    if (buf && buf->type != TERMINAL_BUFF) {
      i = buf->numrows > 1 ? (int)((long)buf->y * 100 / (buf->numrows - 1)) : 100;
      snprintf(p->text, sizeof(p->text), "  %3d%%", i > 100 ? 100 : i);
    }
    break;
  case SEG_CLOCK:
    now = time(NULL);
    tm = localtime(&now);
    if (tm) strftime(p->text, sizeof(p->text), "  %H:%M", tm);
    break;
  default:
    break;
  }
}

// ─── drawing ────────────────────────────────────────────────────────────────

static void appendBarColors(struct abuf *ab) {
  appendStr(ab, E.theme.STATUS_BAR_BG);
  appendStr(ab, E.theme.STATUS_BAR_FG);
}

/**
 * @brief Renders the status bar, the second-to-last screen row
 * @details The active buffer in the accent colors, then the left segments,
 * then the right ones flush right (status-bar-segments). Colors come from
 * the "status-bar*" theme roles.
 * @param ab Pointer to append buffer structure for accumulating output
 */
void editorDrawStatusBar(struct abuf *ab) {
  EditorPane *active = splitScreenGetActivePane();
  Buffer *buf = bufferFindById(active->buffer_id);
  const int width = E.screencols;
  static Piece left[STATUS_MAX_SEGMENTS + 1];
  int left_count = 0;
  Piece right_piece;
  char bar_pos[32];
  char name[304] = "";
  char label[300];
  char right[512] = "";
  char right_short[48] = "";
  int has_buffer = 0;
  int name_cols;
  int right_cols;
  int fixed;
  int room;
  int used = 0;
  int drop;
  int i;
  int j;
  int warnings;

  snprintf(bar_pos, sizeof(bar_pos), "\x1b[%d;1H", E.screenrows + 1);
  appendStr(ab, bar_pos);

  if (s_custom_count) refreshCustomSegments();

  for (i = 0; i < s_left_count; i++) {
    if (s_left[i] == SEG_BUFFER) {
      has_buffer = 1;
      continue;
    }
    segmentText(s_left[i], buf, &left[left_count]);
    if (left[left_count].text[0]) left_count++;
    // Warnings go in a piece of their own right after the errors, in
    // their own color.
    if (s_left[i] == SEG_DIAGNOSTICS && buf && left_count < STATUS_MAX_SEGMENTS) {
      warnings = 0;
      for (j = 0; j < E.lsp_diagnostics.count; ++j)
        if (E.lsp_diagnostics.entries[j].buffer_id == buf->buffer_id &&
            E.lsp_diagnostics.entries[j].severity == DIAG_WARNING)
          warnings++;
      if (warnings && left_count > 0 && left[left_count - 1].id == SEG_DIAGNOSTICS &&
          left[left_count - 1].color == E.theme.COLOR_REMOVED) {
        left[left_count] = left[left_count - 1];
        snprintf(left[left_count].text, sizeof(left[left_count].text), " W%d", warnings);
        left[left_count].color = E.theme.COLOR_NUMBER;
        left_count++;
      }
    }
  }

  for (i = 0; i < s_right_count; i++) {
    segmentText(s_right[i], buf, &right_piece);
    if (strlen(right) + strlen(right_piece.text) < sizeof(right))
      strcat(right, right_piece.text);
  }
  if (right[0]) strcat(right, " ");
  if (buf && buf->type != TERMINAL_BUFF)
    snprintf(right_short, sizeof(right_short), " %d:%d ", buf->y + 1, buf->x + 1);

  if (has_buffer && buf) {
    bufferLabel(buf, label, sizeof(label));
    snprintf(name, sizeof(name), " %s ", label);
  }
  name_cols = textCols(name);
  right_cols = textCols(right);
  if (name_cols + right_cols > width) {
    snprintf(right, sizeof(right), "%s", right_short);
    right_cols = textCols(right);
  }
  if (right_cols > width) {
    right[0] = '\0';
    right_cols = 0;
  }

  // Too many fixed pieces for the row: drop the lowest priority ones,
  // from the end of the list.
  for (i = 0; i < left_count; i++) left[i].cols = textCols(left[i].text);
  for (;;) {
    fixed = name_cols + right_cols;
    for (i = 0; i < left_count; i++)
      if (!left[i].fill) fixed += left[i].cols;
    if (fixed <= width) break;
    drop = -1;
    for (i = 0; i < left_count; i++)
      if (!left[i].fill && (drop < 0 || left[i].priority <= left[drop].priority)) drop = i;
    if (drop < 0) break;
    for (i = drop; i < left_count - 1; i++) left[i] = left[i + 1];
    left_count--;
  }

  // The active buffer, clipped only when nothing else is left.
  if (name[0]) {
    appendStr(ab, E.theme.STATUS_ACCENT_BG);
    appendStr(ab, E.theme.STATUS_ACCENT_FG);
    appendStr(ab, ANSI_BOLD);
    used += appendClipped(ab, name, width - right_cols);
    appendStr(ab, ANSI_RESET);
  }
  appendBarColors(ab);

  // The fill pieces share what the fixed ones leave, in list order.
  room = width - fixed;
  for (i = 0; i < left_count; i++) {
    if (left[i].fill) {
      if (room <= 4) continue;
      if (left[i].dim) appendStr(ab, ANSI_DIM);
      j = appendClipped(ab, left[i].text, room);
      room -= j;
      used += j;
      if (left[i].dim) {
        appendStr(ab, ANSI_RESET);
        appendBarColors(ab);
      }
      continue;
    }
    if (used + left[i].cols + right_cols > width) continue;
    if (left[i].color) appendStr(ab, left[i].color);
    appendStr(ab, left[i].text);
    if (left[i].color) appendStr(ab, E.theme.STATUS_BAR_FG);
    used += left[i].cols;
  }

  while (used < width - right_cols) {
    abAppend(ab, " ", 1);
    used++;
  }
  appendStr(ab, right);

  appendStr(ab, ANSI_RESET);
  abAppend(ab, "\r\n", 2);
}

// ─── Lisp ───────────────────────────────────────────────────────────────────

// Fills `out` from a Lisp list of segment names; 0 (and a message) on an
// unknown name or a non-string, leaving `out` alone.
static int readSegmentList(Lisp list, int *out, int *count) {
  int ids[STATUS_MAX_SEGMENTS];
  int n = 0;
  int id;
  Lisp it;
  for (it = list; lisp_is_pair(it); it = lisp_cdr(it)) {
    if (lisp_type(lisp_car(it)) != LISP_STRING) {
      editorSetStatusMessage("status-bar-segments: segment names are strings");
      return 0;
    }
    id = segmentByName(lisp_string(lisp_car(it)));
    if (id < 0) {
      editorSetStatusMessage("status-bar-segments: unknown segment \"%s\"",
                             lisp_string(lisp_car(it)));
      return 0;
    }
    if (n < STATUS_MAX_SEGMENTS) ids[n++] = id;
  }
  memcpy(out, ids, sizeof(int) * (size_t)n);
  *count = n;
  return 1;
}

/**
 * @brief (status-bar-segments left [right]) — which segments the status bar
 * shows, in order: `left` after the start of the row, `right` flush right.
 * @details Built in: "buffer" (the active one, in the accent block),
 * "git", "diagnostics", "macro", "debug", "mode", "panes", "project",
 * "breadcrumbs", "buffers" (the other open ones), "kind", "position",
 * "percent", "clock"; plus any added with status-bar-add-segment.
 * Left out, `right` stays as it is.
 * @return #t, or #f (with a message) on an unknown name
 */
static Lisp l_statusBarSegments(Lisp args, LispError *e, LispContext ctx) {
  int left[STATUS_MAX_SEGMENTS];
  int right[STATUS_MAX_SEGMENTS];
  int left_n = 0;
  int right_n = s_right_count;
  (void)e;
  (void)ctx;
  memcpy(right, s_right, sizeof(right));
  if (!readSegmentList(lisp_car(args), left, &left_n)) return lisp_false();
  if (lisp_is_pair(lisp_cdr(args)) && !readSegmentList(lisp_car(lisp_cdr(args)), right, &right_n))
    return lisp_false();
  memcpy(s_left, left, sizeof(left));
  s_left_count = left_n;
  memcpy(s_right, right, sizeof(right));
  s_right_count = right_n;
  return lisp_true();
}

/**
 * @brief (status-bar-add-segment "name" fn) — a segment computed in Lisp.
 * @details fn is called with no arguments, at most twice a second, and
 * returns the text to show ("" or a non-string hides the segment). Adding
 * a name again replaces its function. The new segment goes at the end of
 * the left side, before "breadcrumbs" and "buffers"; list it in
 * status-bar-segments to put it elsewhere.
 * @return #t, or #f if the name is a built-in one or too many were added
 */
static Lisp l_statusBarAddSegment(Lisp args, LispError *e, LispContext ctx) {
  const char *name;
  int id;
  int i;
  int at;
  (void)e;
  (void)ctx;
  if (lisp_type(lisp_car(args)) != LISP_STRING) return lisp_false();
  name = lisp_string(lisp_car(args));
  id = segmentByName(name);
  if (id >= 0 && id < SEG_CUSTOM) {
    editorSetStatusMessage("status-bar-add-segment: \"%s\" is a built-in segment", name);
    return lisp_false();
  }
  if (id >= SEG_CUSTOM) {
    s_custom[id - SEG_CUSTOM].fn = lisp_car(lisp_cdr(args));
    s_custom[id - SEG_CUSTOM].failed = 0;
    return lisp_true();
  }
  if (s_custom_count >= STATUS_MAX_CUSTOM) return lisp_false();
  snprintf(s_custom[s_custom_count].name, sizeof(s_custom[s_custom_count].name), "%s", name);
  s_custom[s_custom_count].fn = lisp_car(lisp_cdr(args));
  s_custom[s_custom_count].text[0] = '\0';
  s_custom[s_custom_count].failed = 0;
  id = SEG_CUSTOM + s_custom_count++;
  s_custom_refreshed_ms = 0;

  if (s_left_count < STATUS_MAX_SEGMENTS) {
    at = s_left_count;
    for (i = 0; i < s_left_count; i++)
      if (s_left[i] == SEG_BREADCRUMBS || s_left[i] == SEG_BUFFERS) {
        at = i;
        break;
      }
    for (i = s_left_count; i > at; i--) s_left[i] = s_left[i - 1];
    s_left[at] = id;
    s_left_count++;
  }
  return lisp_true();
}

void statusBarRegisterBuiltins(void) {
  registerBuiltin("status-bar-segments", l_statusBarSegments);
  registerBuiltin("status-bar-add-segment", l_statusBarAddSegment);
}
