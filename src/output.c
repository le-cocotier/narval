/**
 * @file output.c
 * @brief Screen rendering and output module for the Narval text editor
 * @details Handles all screen updates, cursor positioning, status bar
 * rendering, and display synchronization using ANSI escape sequences
 */

#include "../include/output.h"
#include "../include/terminal.h"
#include "../include/galloc.h"
#include "../include/append_buffer.h"
#include "../include/buffer.h"
#include "../include/data.h"
#include "../include/define.h"
#include "../include/editor_op.h"
#include "../include/debug_ui.h"
#include "../include/lsp_ui.h"
#include "../include/buffer_switcher.h"
#include "../include/color.h"
#include "../include/dap.h"
#include "../include/macro.h"
#include "../include/file_finder.h"
#include "../include/list_popup.h"
#include "../include/which_key.h"
#include "../include/screen_diff.h"
#include "../include/split_screen.h"
#include "../include/syntax_highlighter.h"
#include "../include/term_render.h"
#include "../include/utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/***********************************************************************************************************************
 *
 *                                                 extern definitions
 *
 **********************************************************************************************************************/

extern EditorConfig E;

/***********************************************************************************************************************
 *
 *                                                 functions definitions
 *
 **********************************************************************************************************************/

/**
 * @brief Width in columns of the line-number gutter for `buf`, including
 * its trailing space, or 0 when line numbers are off / not applicable.
 * @details Sized to the buffer's actual line count so a 12000-line file
 * gets 5 digits and a short one doesn't waste columns, with a floor of
 * LINE_NUMBER_MIN_WIDTH so the text doesn't visibly shift left/right as a
 * file grows past a power of ten. Only FILE_BUFFs get numbers: a terminal
 * pane's rows are a PTY's screen, not a document, and an explorer sidebar
 * is a menu.
 */
int editorLineNumberWidth(const struct buffer_t *buf) {
  int digits;
  int n;
  if (!buf || !E.constantes.LINE_NUMBERS || buf->type != FILE_BUFF)
    return 0;

  digits = 1;
  for (n = buf->numrows > 0 ? buf->numrows : 1; n >= 10; n /= 10)
    digits++;
  if (digits < LINE_NUMBER_MIN_WIDTH)
    digits = LINE_NUMBER_MIN_WIDTH;
  return digits + 1; // + a space separating numbers from the marker/text
}

/**
 * @brief Total width reserved to the left of a pane's text: line numbers
 * plus the diagnostic/git marker column.
 * @details Single source of truth for the whole gutter. Drawing, scrolling
 * and cursor placement each used to recompute "has_gutter ? GUTTER_WIDTH : 0"
 * independently, so any new gutter element had to be added in lockstep in
 * several files or the cursor would sit a few columns off the text.
 */
int editorGutterWidth(const struct buffer_t *buf) {
  int marker;
  if (!buf)
    return 0;
  marker = (buf->type == FILE_BUFF &&
            (E.constantes.LSP || buf->git_available || debugUiWantsGutter(buf)))
                   ? GUTTER_WIDTH : 0;
  return editorLineNumberWidth(buf) + marker;
}

/**
 * @brief If buf's mark is active and this row overlaps the mark..point
 * region, fills in `from` and `to` with the row's selected character-column
 * range (in the row's own coordinates, not yet adjusted for horizontal
 * scroll) and `extends_past_eol` with whether the selection continues onto
 * the next row (so the line's trailing padding should be highlighted too).
 * @return 1 if this row has a (possibly empty) selection range, 0 if the
 * mark isn't active or this row is outside the region entirely.
 */
static int rowSelectionRange(const struct buffer_t *buf, const int file_row,
                             int *from, int *to, int *extends_past_eol) {
  int sr;
  int sc;
  int er;
  int ec;
  int before;
  if (!buf->mark_active)
    return 0;

  before = (buf->mark_y < buf->y) ||
               (buf->mark_y == buf->y && buf->mark_x <= buf->x);
  if (before) {
    sr = buf->mark_y; sc = buf->mark_x;
    er = buf->y; ec = buf->x;
  } else {
    sr = buf->y; sc = buf->x;
    er = buf->mark_y; ec = buf->mark_x;
  }
  if (file_row < sr || file_row > er || (sr == er && sc == ec))
    return 0;

  *from = (file_row == sr) ? sc : 0;
  *to = (file_row == er) ? ec : buf->row[file_row].size;
  *extends_past_eol = (file_row < er);
  return 1;
}

/**
 * @brief Wraps visible columns [vis_from, vis_to) of an already-rendered
 * row string (which may itself contain syntax-highlighting ANSI SGR codes)
 * in reverse video, to show a selection. Escape sequences are copied
 * through untouched and don't count as columns, so this composes safely
 * with highlight_line()'s own color codes. Caller must gfree() the result.
 * @param rendered The already-rendered row string to wrap in reverse video
 * @param rendered_len Length of `rendered`, in bytes
 * @param vis_from First visible column to start reverse video at
 * @param vis_to -1 means "to the end of the rendered content".
 * @param out_len Out-param: length of the returned string, in bytes
 */
static char *applySelectionHighlight(const char *rendered, int rendered_len,
                                     int vis_from, int vis_to, int *out_len) {
  size_t cap = (size_t) rendered_len + 32;
  char *out = galloc(cap);
  size_t out_pos = 0;
  int col = 0;
  int reversed = 0;
  int i = 0;
  int start;
  unsigned char lead;
  int clen;

  while (i < rendered_len) {
    if (out_pos + 16 > cap) { cap *= 2; out = grealloc(out, cap); }

    if ((unsigned char) rendered[i] == 0x1b) {
      start = i;
      i++;
      if (i < rendered_len && rendered[i] == '[') {
        i++;
        while (i < rendered_len && !(rendered[i] >= '@' && rendered[i] <= '~'))
          i++;
        if (i < rendered_len)
          i++; // consume the final byte
      }
      memcpy(out + out_pos, rendered + start, (size_t)(i - start));
      out_pos += (size_t)(i - start);
      continue;
    }

    if (!reversed && col == vis_from) {
      memcpy(out + out_pos, "\x1b[7m", 4);
      out_pos += 4;
      reversed = 1;
    }
    if (reversed && vis_to >= 0 && col == vis_to) {
      memcpy(out + out_pos, "\x1b[27m", 5);
      out_pos += 5;
      reversed = 0;
    }

    lead = (unsigned char) rendered[i];
    clen = (lead >= 0xF0) ? 4 : (lead >= 0xE0) ? 3 : (lead >= 0xC0) ? 2 : 1;
    if (i + clen > rendered_len) clen = 1;
    memcpy(out + out_pos, rendered + i, (size_t) clen);
    out_pos += (size_t) clen;
    i += clen;
    col++;
  }
  if (reversed) {
    if (out_pos + 5 > cap) { cap += 5; out = grealloc(out, cap); }
    memcpy(out + out_pos, "\x1b[27m", 5);
    out_pos += 5;
  }
  *out_len = (int) out_pos;
  return out;
}

/**
 * @brief The inlay hints of `file_row` that land in the drawn part of it:
 * for each, the rendered-character index it goes before (counted like
 * applySelectionHighlight() does: from the first character drawn, lead
 * padding included) and its label. Hints aren't drawn on a sticky row nor
 * on a row with a selection (whose reverse video they'd break up).
 * @return how many, at most `max`.
 */
static int rowInlayHints(const struct buffer_t *buf, const int file_row, const Utf8Span *span,
                         int *at, const char **labels, const int max) {
  const Row *row = &buf->row[file_row];
  const int visible = span->end - span->start + span->lead_pad;
  int n = 0;
  int i;
  int col;
  if (!E.constantes.INLAY_HINTS || !buf->lsp_hints)
    return 0;
  for (i = 0; i < buf->lsp_hints_count && n < max; i++) {
    if (buf->lsp_hints[i].line < file_row)
      continue;
    if (buf->lsp_hints[i].line > file_row)
      break; // sorted by line
    col = buf->lsp_hints[i].col > row->size ? row->size : buf->lsp_hints[i].col;
    if (col < span->start || col - span->start + span->lead_pad > visible)
      continue;
    at[n] = col - span->start + span->lead_pad;
    labels[n] = buf->lsp_hints[i].label;
    n++;
  }
  return n;
}

/**
 * @brief Copies a rendered row, drawing each label before the character
 * its `at` names (in the comment color, italic), and stops once the row
 * takes `max_cols` columns.
 * @param out_cols Out-param: the columns the result takes
 * @return The new row (galloc'd), *out_len bytes long
 */
static char *spliceInlayHints(const char *rendered, const int rendered_len, const int *at,
                              const char **labels, const int n, const int max_cols,
                              int *out_len, int *out_cols) {
  struct abuf ab = ABUF_INIT;
  const char *last_sgr = NULL; // to restore after each hint
  int last_sgr_len = 0;
  int index = 0;
  int cols = 0;
  int next = 0;
  int i = 0;
  int start;
  int bytes;
  int w;
  char *out;
  Utf8Char c;

  while (cols < max_cols) {
    // Hints anchored here, before this character (or at the row's end).
    while (next < n && at[next] == index && cols < max_cols) {
      bytes = lspUiClipToColumns(labels[next], (int) strlen(labels[next]), max_cols - cols, &w);
      abAppend(&ab, E.theme.COLOR_COMMENT, (int) strlen(E.theme.COLOR_COMMENT));
      abAppend(&ab, "\x1b[3m", 4);
      abAppend(&ab, labels[next], bytes);
      abAppend(&ab, "\x1b[23m", 5);
      if (last_sgr)
        abAppend(&ab, last_sgr, last_sgr_len);
      else
        abAppend(&ab, E.theme.COLOR_DEFAULT, (int) strlen(E.theme.COLOR_DEFAULT));
      cols += w;
      next++;
    }
    if (i >= rendered_len || cols >= max_cols)
      break;

    if ((unsigned char) rendered[i] == 0x1b) {
      start = i++;
      if (i < rendered_len && rendered[i] == '[') {
        i++;
        while (i < rendered_len && !(rendered[i] >= '@' && rendered[i] <= '~'))
          i++;
        if (i < rendered_len)
          i++;
      }
      abAppend(&ab, rendered + start, i - start);
      last_sgr = rendered + start;
      last_sgr_len = i - start;
      continue;
    }

    utf8CreateChar((const unsigned char *) rendered + i, &c);
    bytes = c.length <= rendered_len - i ? c.length : 1;
    w = utf8CharWidth(&c);
    if (cols + w > max_cols)
      break; // a wide character that no longer fits
    abAppend(&ab, rendered + i, bytes);
    i += bytes;
    cols += w;
    index++;
  }

  out = galloc((size_t) ab.len + 1);
  memcpy(out, ab.b, (size_t) ab.len);
  out[ab.len] = '\0';
  *out_len = ab.len;
  *out_cols = cols;
  abFree(&ab);
  return out;
}

/**
 * @brief Renders a single pane with its buffer content
 */
/**
 * @brief Recomputes a pane's view when it has started showing a different
 * buffer.
 * @details Scroll offsets live on the pane, not the buffer, so without this
 * a buffer switched into a pane inherited the previous buffer's scroll
 * position: switching from line 1000 of a file to a short buffer left its
 * first rows scrolled out of sight above the cursor. On a change of buffer
 * the view is placed around the new buffer's own cursor instead: from the
 * top when the cursor fits on the first screen, otherwise centered (or, for
 * a terminal, bottom-aligned like the shell's own screen).
 * A no-op while the pane keeps showing the same buffer, so ordinary
 * scrolling (editorScroll()'s minimal adjustments) is untouched.
 */
static void paneFitShownBuffer(EditorPane *pane, const struct buffer_t *buf) {
  if (pane->shown_buffer_id == pane->buffer_id)
    return;
  pane->shown_buffer_id = pane->buffer_id;

  if (buf->y < pane->height)
    pane->y_offset = 0;                            // fits on the first screen
  else if (buf->type == TERMINAL_BUFF)
    pane->y_offset = buf->y - (pane->height - 1);  // a shell's screen: bottom-aligned
  else
    pane->y_offset = buf->y - pane->height / 2;    // deep in a file: centered
  pane->cursor_y = buf->y - pane->y_offset;
  // Horizontal: start from column 0, editorScroll() walks right to the
  // cursor if it's off-screen.
  pane->x_offset = 0;
  pane->cursor_x = 0;
}

/**
 * @brief The rows sticky scroll pins atop `pane`: the first line of each
 * function / struct / ... the pane's top row is inside of but whose own
 * first line has scrolled out of sight, outermost first. Never so many
 * that they'd cover the cursor, or a third of the pane.
 * @return how many (0 without LSP symbols for the buffer)
 */
int editorPaneStickyRows(const EditorPane *pane, const struct buffer_t *buf, int *rows) {
  const LspSymbol *chain[STICKY_MAX];
  int n;
  int count = 0;
  int i;
  if (!E.constantes.STICKY_SCROLL || buf->type != FILE_BUFF || pane->y_offset <= 0)
    return 0;
  n = lspUiEnclosingSymbols(buf, pane->y_offset, chain, STICKY_MAX);
  for (i = 0; i < n; i++)
    if (chain[i]->line < pane->y_offset && chain[i]->line < buf->numrows)
      rows[count++] = chain[i]->line;
  if (pane->is_active && count > buf->y - pane->y_offset)
    count = buf->y - pane->y_offset;
  if (count > pane->height / 3)
    count = pane->height / 3;
  return count < 0 ? 0 : count;
}

static void editorDrawPane(struct abuf *ab, EditorPane *pane) {
  int file_row;
  char pos_buf[32];
  int pos_len;
  int byte_len_to_print;
  char *highlighted;
  const struct buffer_t *buf;
  int has_gutter;
  int lineno_width;
  int gutter_width;
  int content_width;
  int y;
  int is_cursor_line;
  int i;
  char *lineno_color;
  char num_buf[24];
  int num_len;
  GitLineStatus git_status;
  int sel_from;
  int sel_to;
  int sel_extends;
  int has_selection;
  int vis_from;
  int vis_to;
  const LanguageSyntax *lang;
  int wrapped_len;
  char *wrapped;
  char *line;
  int line_len;
  int filled;
  Utf8Span span;
  const Row *row;
  int sticky_rows[STICKY_MAX];
  int sticky_n;
  int is_sticky;
  int hint_at[64];
  const char *hint_labels[64];
  int hint_n;
  int hinted_len;
  int hinted_cols;
  char *hinted;
  const Diagnostic *diag;
  char *debug_path;

  if (pane == NULL || pane->buffer_id < 0)
    return;

  buf = bufferFindById(pane->buffer_id);
  if (buf == NULL)
    return;
  paneFitShownBuffer(pane, buf);

  // The diagnostic gutter only makes sense for real source files — drawing
  // it for a terminal pane would eat GUTTER_WIDTH columns the PTY was never
  // told to give up, misaligning its own line-wrapping. content_width is
  // what's actually left for text once the gutter (if any) is accounted for;
  // gutter + content_width always adds up to the pane's full on-screen span.
  debug_path = debugUiGutterPath(buf);
  has_gutter = buf->type == FILE_BUFF &&
               (E.constantes.LSP || buf->git_available || debug_path);
  lineno_width = editorLineNumberWidth(buf);
  gutter_width = editorGutterWidth(buf);
  content_width = pane->width - gutter_width;
  if (content_width < 0)
    content_width = 0;

  // Seed the syntax highlighter's cross-row block-comment state for
  // wherever this pane's scroll position actually starts, rather than
  // whatever comment_section was last left at by a previous pane or a
  // previous scroll position — see the comment_state fields on
  // struct buffer_t and commentStateAtRowStart()'s own doc comment.
  syntaxHighlighterSeed(commentStateAtRowStart((struct buffer_t *) buf, pane->y_offset));
  sticky_n = editorPaneStickyRows(pane, buf, sticky_rows);

  // Draw all pane
  for (y = 0; y < pane->height; y++) {
    // Sticky rows stand in for the first rows of the view; each is drawn
    // from its own place in the file, so the block-comment state is
    // re-seeded for it, and again where the ordinary rows resume.
    is_sticky = y < sticky_n;
    file_row = is_sticky ? sticky_rows[y] : y + pane->y_offset;
    if (is_sticky || (sticky_n > 0 && y == sticky_n))
      syntaxHighlighterSeed(commentStateAtRowStart((struct buffer_t *) buf, file_row));
    is_cursor_line = (!is_sticky && pane->is_active && file_row == buf->y);

    // Set cursor at start of pane row
    pos_len = snprintf(pos_buf, sizeof(pos_buf), "\x1b[%d;%dH",
                       pane->origin_y + y + 1, pane->origin_x + 1);
    abAppend(ab, pos_buf, pos_len);

    // Apply background color (6 bytes for RGB format)
    abAppend(ab, E.theme.BACKGROUND_COLOR,
             (int)strlen(E.theme.BACKGROUND_COLOR));
    // ...and the theme's default foreground, so text drawn without its own
    // explicit color (the tilde below, and the whole plain-text branch
    // further down for anything that isn't a .c/.h file — the explorer
    // sidebar included) renders in a deliberate, theme-consistent color
    // instead of whatever was last left active by a previous row (line
    // numbers reset to the terminal's own default and never re-set an
    // explicit fg of their own; a syntax-highlighted C row leaves its
    // last token's color active). Harmless for highlight_line() output:
    // its very first byte is always one of its own color codes, which
    // simply overrides this immediately.
    abAppend(ab, E.theme.COLOR_DEFAULT, (int) strlen(E.theme.COLOR_DEFAULT));

    // pane line is out of buffer height
    if (file_row >= buf->numrows) {
      // Empty line - show tilde, but still reserve the gutter's columns so
      // the tilde lines up with where text starts on real lines above it
      for (i = 0; i < gutter_width; ++i)
        abAppend(ab, " ", 1);
      abAppend(ab, "~", 1);
      for (i = 0; i < content_width - 1; ++i) {
        abAppend(ab, " ", 1);
      }
    } else {
      if (lineno_width) {
        // The cursor's own line is highlighted (bold, undimmed) so the eye
        // can find it without hunting for the terminal cursor itself.
        //
        // Colors come from the theme rather than a hardcoded "\x1b[2;37m"/
        // "\x1b[1;37m" (dim/bold ANSI white): COLOR_COMMENT is already,
        // by every theme's own design, the muted/secondary tone relative
        // to COLOR_DEFAULT, which is exactly the dim/prominent pairing
        // this gutter wants — and unlike literal ANSI white, it can't end
        // up lighter than the background. It did: bold+white is rendered
        // bright/intense by most terminals, which on theme-light's
        // near-white background (250,251,252) made the cursor's own line
        // number — the one meant to stand out — the least visible thing
        // on screen.
        lineno_color = is_cursor_line ? E.theme.COLOR_DEFAULT : E.theme.COLOR_COMMENT;
        num_len = snprintf(num_buf, sizeof(num_buf), "%*d ",
                               lineno_width - 1, file_row + 1);
        if (is_cursor_line)
          abAppend(ab, "\x1b[1m", 4);
        abAppend(ab, lineno_color, (int) strlen(lineno_color));
        abAppend(ab, num_buf, num_len);
        abAppend(ab, "\x1b[0m", 4);
        abAppend(ab, E.theme.BACKGROUND_COLOR, (int) strlen(E.theme.BACKGROUND_COLOR));
        // The \x1b[0m above also clears the default-fg set at the top of
        // this row's render — re-set it before content starts, or every
        // non-.c/.h FILE_BUFF (the common case — .txt, .md, .lisp, ...)
        // renders its text in the terminal's own default color, not the
        // theme's, the moment line numbers are on (the default).
        abAppend(ab, E.theme.COLOR_DEFAULT, (int) strlen(E.theme.COLOR_DEFAULT));
      }
      if (has_gutter && !debugUiDrawGutter(ab, debug_path, file_row)) {
        git_status = (buf->git_lines && file_row < buf->git_lines_count)
                                        ? buf->git_lines[file_row] : GIT_LINE_NONE;
        lspUiDrawGutter(ab, &E.lsp_diagnostics, pane->buffer_id, file_row, git_status);
      }

      // Selection highlight — not applied to TERMINAL_BUFF (a live shell's
      // own escape sequences make wrapping unreliable, and "mark a region
      // in a PTY view" isn't a meaningful operation anyway).
      // x_offset and content_width are screen columns; span is the
      // characters that fit in them (wide characters take two columns).
      row = &buf->row[file_row];
      utf8VisibleSpan(row->characters, row->size, pane->x_offset, content_width, &span);

      sel_from = 0; sel_to = 0; sel_extends = 0;
      has_selection = buf->type != TERMINAL_BUFF && !is_sticky &&
                          rowSelectionRange(buf, file_row, &sel_from, &sel_to, &sel_extends);
      // applySelectionHighlight() counts rendered characters, so these are
      // relative to the first character drawn (after any lead padding).
      vis_from = 0; vis_to = -1;
      if (has_selection) {
        vis_from = sel_from - span.start + span.lead_pad;
        if (vis_from < 0) vis_from = 0;
        if (!sel_extends) {
          vis_to = sel_to - span.start + span.lead_pad;
          if (vis_to < 0) vis_to = 0;
        }
      }

      if (buf->type == TERMINAL_BUFF) {
        // Clipped to the pane, exactly like the file paths below: a
        // terminal row is as wide as the PTY was when the shell wrote it,
        // which stops matching the pane the moment the layout changes.
        highlighted = termRenderRow(&buf->row[file_row], &byte_len_to_print,
                                    pane->x_offset, content_width);
        abAppend(ab, highlighted, byte_len_to_print);
        gfree(highlighted);
      } else {
        hint_n = (has_selection || is_sticky)
                     ? 0
                     : rowInlayHints(buf, file_row, &span, hint_at, hint_labels, 64);
        // A sticky row gets the cursor line's background: it's not where
        // it is in the file, and should look apart from the rows below.
        // The line the debugger stopped on outranks both.
        if (!is_sticky && debug_path && debugUiIsCurrentLine(debug_path, file_row))
            abAppend(ab, E.theme.DEBUG_LINE_BACKGROUND, (int) strlen(E.theme.DEBUG_LINE_BACKGROUND));
        else if (is_cursor_line || is_sticky)
            abAppend(ab, E.theme.LINE_CURSOR_BACKGROUND, (int) strlen(E.theme.LINE_CURSOR_BACKGROUND));
        else
            abAppend(ab, E.theme.BACKGROUND_COLOR, (int) strlen(E.theme.BACKGROUND_COLOR));
        // Extension-based lookup against whatever packages have actually
        // registered via (syntax-define-language ...), rather than a
        // hardcoded "does the filename end in c or h" check — the old
        // check also matched anything merely *ending* in those letters
        // (a file called "search" would have gone through the C
        // tokenizer), and gave C highlighting no matter what packages
        // were loaded. Now a buffer with no registered language for its
        // extension always falls through to plain text, C included.
        lang = syntaxLanguageForBuffer(buf);
        if (lang) {
          byte_len_to_print = 0;
          // Render line with syntax highlighting, constrain to pane width
          highlighted = highlight_line(&buf->row[file_row], &byte_len_to_print,
                                       pane->x_offset, content_width, lang);

          if (has_selection) {
            wrapped_len = 0;
            wrapped = applySelectionHighlight(highlighted, byte_len_to_print,
                                                    vis_from, vis_to, &wrapped_len);
            gfree(highlighted);
            highlighted = wrapped;
            byte_len_to_print = wrapped_len;
          }

          if (hint_n > 0) {
            hinted = spliceInlayHints(highlighted, byte_len_to_print, hint_at, hint_labels,
                                      hint_n, content_width, &hinted_len, &hinted_cols);
            gfree(highlighted);
            highlighted = hinted;
            byte_len_to_print = hinted_len;
            span.columns = hinted_cols;
          }

          // Print only up to pane width
          abAppend(ab, highlighted, byte_len_to_print);
          gfree(highlighted);
        } else {

          // Render basic line: the characters that fit between x_offset
          // and the pane's width, in columns. A row shorter than x_offset
          // gives an empty span.
          wrapped = (char *)utf8ToNString(&row->characters[span.start],
                                          span.end - span.start);
          line_len = (int) strlen(wrapped) + span.lead_pad;
          line = galloc((size_t) line_len + 1);
          memset(line, ' ', (size_t) span.lead_pad);
          memcpy(line + span.lead_pad, wrapped, strlen(wrapped) + 1);
          gfree(wrapped);

          if (has_selection) {
            wrapped_len = 0;
            wrapped = applySelectionHighlight(line, line_len, vis_from, vis_to, &wrapped_len);
            gfree(line);
            line = wrapped;
            line_len = wrapped_len;
          }
          if (hint_n > 0) {
            hinted = spliceInlayHints(line, line_len, hint_at, hint_labels, hint_n,
                                      content_width, &hinted_len, &hinted_cols);
            gfree(line);
            line = hinted;
            line_len = hinted_len;
            span.columns = hinted_cols;
          }
          abAppend(ab, line, line_len);
          gfree(line);
          line = NULL;
        }
      }

      // Fill remaining space with background color to pane width — also in
      // reverse video if the selection continues past this line's content
      // (a whole line selected, including its line break).
      if (has_selection && sel_extends)
        abAppend(ab, "\x1b[7m", 4);
      // Start from how many columns the content actually filled.
      filled = span.columns;

      // After the line's text, when its end is in view (and not under a
      // selection running past it): its struct layout note, if shown, then
      // its diagnostic.
      if (buf->lsp_layout && buf->lsp_layout_seq == buf->edit_seq + 1 && !is_sticky &&
          !(has_selection && sel_extends) && span.end >= row->size) {
        for (i = 0; i < buf->lsp_layout_count; i++)
          if (buf->lsp_layout[i].line == file_row) {
            filled += lspUiDrawEolNote(ab, buf->lsp_layout[i].label, content_width - filled);
            break;
          }
      }
      // While the debugger is paused here: the values of the variables
      // the line names.
      if (debug_path && !is_sticky && !(has_selection && sel_extends) && span.end >= row->size)
        filled += debugUiDrawInlineValues(ab, debug_path, buf, file_row, content_width - filled);
      if (E.constantes.INLINE_DIAGNOSTICS && buf->type == FILE_BUFF && !is_sticky &&
          !(has_selection && sel_extends) && span.end >= row->size) {
        diag = lspUiWorstDiagnostic(&E.lsp_diagnostics, pane->buffer_id, file_row);
        if (diag)
          filled += lspUiDrawInlineDiagnostic(ab, diag, content_width - filled);
      }
      for (i = filled; i < content_width; i++) {
        abAppend(ab, " ", 1);
      }
      if (has_selection && sel_extends)
        abAppend(ab, "\x1b[27m", 5);
    }
  }
  gfree(debug_path);
}

/**
 * @brief Renders all panes based on current split configuration
 */
/**
 * @brief Draws every pane, then the dividers between them.
 * @details Dividers are derived from the panes' own rectangles rather than
 * from a single global split mode, which is what lets the layout nest
 * arbitrarily: splitScreenSplitActive() always leaves one column (or row)
 * unclaimed between the two halves it makes, so any pane whose right edge
 * stops short of the screen has a gap there, and the same for its bottom
 * edge. Drawing a rule down/along each such edge fills exactly those gaps,
 * for any number of panes in any arrangement.
 *
 * The old version only understood a two-pane layout and read the split
 * direction from layout->mode, so it drew a full-height rule at pane 0's
 * width — which also painted over pane 0's last text column, since that
 * layout left no room for the divider to sit in.
 *
 * Each rule is bounded to its own pane's span, not the whole screen, so a
 * divider between two stacked panes on the right doesn't cut across the
 * pane on the left. Neighbouring panes may name the same gap; drawing it
 * twice is harmless.
 */
static void editorDrawAllPanes(struct abuf *ab) {
  const ScreenLayout *layout = splitScreenGetLayout();
  int i;
  const EditorPane *p;
  char pos_buf[32];
  int y;
  int len;
  int x;

  for (i = 0; i < layout->num_panes; i++)
    editorDrawPane(ab, &layout->panes[i]);

  for (i = 0; i < layout->num_panes; i++) {
    p = &layout->panes[i];

    // Vertical rule down the gap on this pane's right edge.
    if (p->origin_x + p->width < E.screencols) {
      for (y = 0; y < p->height; y++) {
        len = snprintf(pos_buf, sizeof(pos_buf), "\x1b[%d;%dH",
                           p->origin_y + y + 1, p->origin_x + p->width + 1);
        abAppend(ab, pos_buf, len);
        // "│" is U+2502, a 3-byte UTF-8 sequence — sizeof(literal)-1 here
        // instead of a hand-counted length avoids a repeat of the bug this
        // line used to have: a hardcoded length of 9 (right for the plain-
        // ASCII "-" rule below, wrong for this one) silently truncated the
        // trailing "\x1b[0m" reset down to a dangling, unterminated "\x1b[",
        // corrupting the escape sequence stream from the very next divider
        // row onward — invisible while every split rendered at whatever
        // size the terminal started at, and only actually reachable once
        // resizing (which redraws the dividers at their new positions) was
        // wired up for real.
        abAppend(ab, "\x1b[1m│\x1b[0m", sizeof("\x1b[1m│\x1b[0m") - 1);
      }
    }

    // Horizontal rule along the gap under this pane.
    if (p->origin_y + p->height < E.screenrows) {
      len = snprintf(pos_buf, sizeof(pos_buf), "\x1b[%d;%dH",
                         p->origin_y + p->height + 1, p->origin_x + 1);
      abAppend(ab, pos_buf, len);
      for (x = 0; x < p->width; x++)
        abAppend(ab, "\x1b[1m-\x1b[0m", sizeof("\x1b[1m-\x1b[0m") - 1);
    }
  }
}

/**
 * @brief Updates scroll offsets to keep cursor visible on screen
 * @details Adjusts E.row_offset and E.col_offset to ensure the cursor remains
 * within the visible viewport. Also updates E.rx (rendered x-coordinate).
 * @note Updates global editor state E
 * @see editorRowCxToRx()
 */
void editorScroll() {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);
  int usable_width;
  int column;

  // bufferFindById() returns NULL for an id that isn't in the list — a
  // pane still pointing at a closed buffer, or at -1 after a failed
  // bufferCreate(). The cursor-tracking loop below dereferences buf on
  // every iteration, so there's nothing to scroll toward without it.
  if (buf == NULL)
    return;
  paneFitShownBuffer(active, buf);

  // Same gutter accounting as editorDrawPane(): with the gutter reserving
  // GUTTER_WIDTH columns, only what's left is actually available before the
  // cursor needs to scroll right, or it would visually run past the pane.
  usable_width = active->width - editorGutterWidth(buf);
  if (usable_width < 1)
    usable_width = 1;

  // The pane scrolls in screen columns, not characters.
  column = buf->y < buf->numrows
               ? utf8ColumnOf(buf->row[buf->y].characters, buf->row[buf->y].size, buf->x)
               : buf->x;
  while (column != active->cursor_x + active->x_offset ||
         buf->y != active->cursor_y + active->y_offset) {
    if (column < active->cursor_x + active->x_offset) {
      // LEFT
      if (active->cursor_x == 0 && active->x_offset) {
        active->x_offset--;
      } else {
        active->cursor_x--;
      }
    } else {
      // RIGHT
      if (column > active->cursor_x + active->x_offset) {
        if (active->cursor_x == usable_width - 1) {
          active->x_offset++;
        } else {
          active->cursor_x++;
        }
      }
    }

    if (buf->y < active->cursor_y + active->y_offset) {
      if (active->cursor_y == 0 && active->y_offset) {
        active->y_offset--;
      } else {
        active->cursor_y--;
      }
    }

    if (buf->y > active->cursor_y + active->y_offset) {
      if (active->cursor_y == active->height - 1) {
        active->y_offset++;
      } else {
        active->cursor_y++;
      }
    }
  }

  // A wide character on the last column would have its right half cut
  // off: scroll one more column so it's drawn whole.
  if (active->cursor_x == usable_width - 1 && usable_width > 1 &&
      buf->y < buf->numrows && buf->x < buf->row[buf->y].size &&
      utf8CharWidth(&buf->row[buf->y].characters[buf->x]) == 2) {
    active->x_offset++;
    active->cursor_x--;
  }
}

// ── Status bar ──────────────────────────────────────────────────────────────
//
//  ▌ 1:main.c* ▐ develop* E2 W1 ● REC  point › main   2:buffer.c 3:*search*   c  Ln 42/310, Col 7  13%
//  └ accent ──┘ └ info ───────────────┘ └ breadcrumbs ┘ └ other buffers ───┘   └ right ──────────────┘
//
// The active buffer always comes first and the cursor position always last,
// so neither can be pushed off the screen by a long list of open buffers:
// when the row is too narrow, the other buffers go first, then the
// breadcrumbs, then the info segment, then the long position form, and the
// active name is clipped last.

// Terminal columns taken by a UTF-8 string (wide characters count 2).
static int statusTextCols(const char *s) {
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
static int statusAppendClipped(struct abuf *ab, const char *s, int max_cols) {
  Utf8Char c;
  const char *p = s;
  int cols = 0;
  int n;
  int w;
  if (max_cols <= 0) return 0;
  if (statusTextCols(s) <= max_cols) {
    abAppend(ab, s, (int)strlen(s));
    return statusTextCols(s);
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

// " 3:name* " for one buffer, into out.
static void statusBufferLabel(const Buffer *b, char *out, size_t size) {
  char *name = b->filename ? (char *)utf8ToString(b->filename) : NULL;
  snprintf(out, size, "%d:%s%s", b->buffer_id, name ? name : "[No Name]",
           b->dirty ? "*" : "");
  gfree(name);
}

// What the buffer holds, for the right side: the syntax override, the
// file's extension, or a word for the non-file buffer kinds.
static void statusBufferKind(const Buffer *b, char *out, size_t size) {
  out[0] = '\0';
  if (b->type == TERMINAL_BUFF) {
    snprintf(out, size, "terminal");
  } else if (b->syntax[0]) {
    snprintf(out, size, "%s", b->syntax);
  } else if (b->filename) {
    utf8FilenameExtension(b->filename, out, size);
  }
  if (b->crlf && strlen(out) + 6 < size) strcat(out, " CRLF");
  if (b->state == READ_ONLY && strlen(out) + 4 < size) strcat(out, " RO");
}

/**
 * @brief Renders the status bar, the second-to-last screen row
 * @details Left: the active buffer in the accent colors, its git branch,
 * LSP error/warning counts, macro recording, debugger state and the active
 * sticky mode. Middle: the breadcrumbs (BREADCRUMBS) and the other open
 * buffers. Right: file kind, line/column and position in the file.
 * Colors come from the "status-bar*" theme roles.
 * @param ab Pointer to append buffer structure for accumulating output
 */
void editorDrawStatusBar(struct abuf *ab) {
  EditorPane *active = splitScreenGetActivePane();
  Buffer *buf = bufferFindById(active->buffer_id);
  ScreenLayout *layout = splitScreenGetLayout();
  const int width = E.screencols;
  char bar_pos[32];
  char name[304] = "";
  char git[80] = "";
  char err[16] = "";
  char warn[16] = "";
  char tail[96] = "";
  char rec[16] = "";
  char crumbs[256];
  int crumbs_len = 0;
  int crumbs_cols = 0;
  char right_long[128] = "";
  char right_short[48] = "";
  char kind[40] = "";
  char label[300];
  const char *right;
  const char *mode = NULL;
  int errors = 0;
  int warnings = 0;
  int name_cols;
  int info_cols;
  int right_cols;
  int used = 0;
  int room;
  int off;
  int i;
  Buffer *b;
  const char *bar = E.theme.STATUS_BAR_BG;
  const char *bar_fg = E.theme.STATUS_BAR_FG;

  // Positioned explicitly: the status bar sits right under the panes
  // (E.screenrows already excludes it and the message bar), whatever was
  // drawn last.
  snprintf(bar_pos, sizeof(bar_pos), "\x1b[%d;1H", E.screenrows + 1);
  abAppend(ab, bar_pos, (int)strlen(bar_pos));

  if (buf) {
    statusBufferLabel(buf, label, sizeof(label));
    snprintf(name, sizeof(name), " %s ", label);

    for (i = 0; i < E.lsp_diagnostics.count; ++i) {
      if (E.lsp_diagnostics.entries[i].buffer_id != buf->buffer_id) continue;
      if (E.lsp_diagnostics.entries[i].severity == DIAG_ERROR) errors++;
      else if (E.lsp_diagnostics.entries[i].severity == DIAG_WARNING) warnings++;
    }

    // Sticky prefixes are modes (terminal, explorer, memory ...); a
    // one-shot prefix like CTRL-x is already shown by which-key.
    if (E.prefix_state > 0 && E.prefix_state < E.number_of_prefix &&
        E.prefix[E.prefix_state].sticky)
      mode = E.prefix[E.prefix_state].prefix_name;

    if (buf->git_available)
      snprintf(git, sizeof(git), " %s%s", buf->git_branch, buf->git_dirty ? "*" : "");
    if (errors) snprintf(err, sizeof(err), " E%d", errors);
    if (warnings) snprintf(warn, sizeof(warn), " W%d", warnings);
    if (macroIsRecording()) snprintf(rec, sizeof(rec), " \xe2\x97\x8f REC"); // ●
    off = 0;
    if (dapState() == DAP_RUNNING || dapState() == DAP_STARTING)
      off += snprintf(tail + off, sizeof(tail) - off, " [debug: running]");
    else if (dapState() == DAP_STOPPED)
      off += snprintf(tail + off, sizeof(tail) - off, " [debug: paused]");
    if (mode && off < (int)sizeof(tail))
      off += snprintf(tail + off, sizeof(tail) - off, " [%s]", mode);
    if (layout->num_panes > 1 && off < (int)sizeof(tail))
      snprintf(tail + off, sizeof(tail) - off, " [%d panes]", layout->num_panes);

    if (E.constantes.BREADCRUMBS && buf->type == FILE_BUFF)
      crumbs_len = lspUiBreadcrumbs(buf, buf->y, crumbs, sizeof(crumbs));

    statusBufferKind(buf, kind, sizeof(kind));
    if (buf->type == TERMINAL_BUFF) {
      snprintf(right_long, sizeof(right_long), " %s ", kind);
      snprintf(right_short, sizeof(right_short), " %s ", kind);
    } else {
      int pct = buf->numrows > 1 ? (int)((long)buf->y * 100 / (buf->numrows - 1)) : 100;
      if (pct > 100) pct = 100;
      snprintf(right_long, sizeof(right_long), "  %s%sLn %d/%d, Col %d  %3d%% ",
               kind, kind[0] ? "  " : "", buf->y + 1, buf->numrows ? buf->numrows : 1,
               buf->x + 1, pct);
      snprintf(right_short, sizeof(right_short), " %d:%d ", buf->y + 1, buf->x + 1);
    }
  }

  name_cols = statusTextCols(name);
  info_cols = statusTextCols(git) + statusTextCols(err) + statusTextCols(warn) +
              statusTextCols(rec) + statusTextCols(tail);
  right = right_long;
  right_cols = statusTextCols(right_long);
  if (name_cols + right_cols > width) {
    right = right_short;
    right_cols = statusTextCols(right_short);
  }
  if (right_cols > width) right_cols = 0, right = "";

  // Accent block: the active buffer, clipped only when nothing else is left.
  abAppend(ab, E.theme.STATUS_ACCENT_BG, (int)strlen(E.theme.STATUS_ACCENT_BG));
  abAppend(ab, E.theme.STATUS_ACCENT_FG, (int)strlen(E.theme.STATUS_ACCENT_FG));
  abAppend(ab, ANSI_BOLD, (int)strlen(ANSI_BOLD));
  used += statusAppendClipped(ab, name, width - right_cols);
  abAppend(ab, ANSI_RESET, (int)strlen(ANSI_RESET));
  abAppend(ab, bar, (int)strlen(bar));
  abAppend(ab, bar_fg, (int)strlen(bar_fg));

  // Info: branch, diagnostic counts (in the diff colors), mode, panes.
  if (info_cols && used + info_cols + right_cols <= width) {
    abAppend(ab, git, (int)strlen(git));
    if (err[0]) {
      abAppend(ab, E.theme.COLOR_REMOVED, (int)strlen(E.theme.COLOR_REMOVED));
      abAppend(ab, err, (int)strlen(err));
    }
    if (warn[0]) {
      abAppend(ab, E.theme.COLOR_NUMBER, (int)strlen(E.theme.COLOR_NUMBER));
      abAppend(ab, warn, (int)strlen(warn));
    }
    if (rec[0]) {
      abAppend(ab, E.theme.COLOR_REMOVED, (int)strlen(E.theme.COLOR_REMOVED));
      abAppend(ab, rec, (int)strlen(rec));
    }
    abAppend(ab, bar_fg, (int)strlen(bar_fg));
    abAppend(ab, tail, (int)strlen(tail));
    used += info_cols;
  }

  // Where the cursor is in the code ("point › main"), up to half the row.
  room = width - used - right_cols - 2;
  if (crumbs_len > 0 && room > 8) {
    crumbs_len = lspUiClipToColumns(crumbs, crumbs_len,
                                    room < width / 2 ? room : width / 2, &crumbs_cols);
    abAppend(ab, "  ", 2);
    abAppend(ab, crumbs, crumbs_len);
    used += 2 + crumbs_cols;
  }

  // The other buffers, dimmed, in whatever room is left.
  room = width - used - right_cols;
  if (room > 4 && E.buffers != NULL && E.number_of_buffer > 1) {
    char list[1024] = "  ";
    int list_off = 2;
    b = E.buffers;
    do {
      if (!buf || b->buffer_id != buf->buffer_id) {
        statusBufferLabel(b, label, sizeof(label));
        if (list_off < (int)sizeof(list))
          list_off += snprintf(list + list_off, sizeof(list) - list_off, "%s ", label);
      }
      b = b->next;
    } while (b != E.buffers);
    if (list_off > 2) {
      abAppend(ab, ANSI_DIM, (int)strlen(ANSI_DIM));
      used += statusAppendClipped(ab, list, room);
      abAppend(ab, ANSI_RESET, (int)strlen(ANSI_RESET));
      abAppend(ab, bar, (int)strlen(bar));
      abAppend(ab, bar_fg, (int)strlen(bar_fg));
    }
  }

  while (used < width - right_cols) {
    abAppend(ab, " ", 1);
    used++;
  }
  abAppend(ab, right, (int)strlen(right));

  abAppend(ab, ANSI_RESET, (int)strlen(ANSI_RESET));
  abAppend(ab, "\r\n", 2);
}

/**
 * @brief Renders the message bar below the status bar
 * @details Displays temporary status messages for a limited time (5 seconds).
 * Only displays message if within time window and within screen width.
 * @param ab Pointer to append buffer structure for accumulating output
 * @note Messages are set by editorSetStatusMessage()
 */
void editorDrawMessageBar(struct abuf *ab) {
  char *status = (char *) utf8ToString(E.status_msg);
  int msg_len = (int)strlen(status);

  // Same reasoning as the status bar: place it, don't inherit a position.
  char pos[32];
  int pos_len = snprintf(pos, sizeof(pos), "\x1b[%d;1H", E.screenrows + 2);
  int bytes;
  int cols;
  abAppend(ab, pos, pos_len);

  abAppend(ab, ERASE_END_LINE, 3);
  if (msg_len && time(NULL) - E.status_msg_time < 5) {
    // Clip to the screen width (in characters, not bytes): this is the
    // terminal's last row, so a longer message — a long path, say — wraps
    // and scrolls the whole screen up a line until the next full redraw.
    bytes = 0; cols = 0;
    while (bytes < msg_len && cols < E.screencols - 1) {
      bytes++;
      while (bytes < msg_len && ((unsigned char) status[bytes] & 0xC0) == 0x80)
        bytes++;
      cols++;
    }
    abAppend(ab, status, bytes);
  }
  gfree(status);
}

/**
 * @brief Performs complete screen refresh and buffer synchronization
 * @details Clears screen, redraws all visible content (rows, status bar,
 * message bar), positions cursor, and writes accumulated buffer to stdout. This
 * is the main rendering function called each frame.
 * @note Updates global editor state E (via editorScroll())
 * @see editorDrawRows()
 * @see editorDrawStatusBar()
 * @see editorDrawMessageBar()
 */

// ── Frame de-duplication ────────────────────────────────────────────────────
//
// editorRunLoop() calls this unconditionally on every ~50ms tick — needed so
// time-driven UI (the which-key delay, the 5s status-message timeout) keeps
// working without a keypress — but that means a totally static screen (user
// reading, not typing) was being sent an identical, full-screen repaint
// roughly 19 times a second, hiding and re-showing the cursor every single
// time. That's what actually produced the flicker: many terminals (tmux
// especially) visibly blink the hardware cursor on every hide/show pair even
// when nothing underneath moved. Caching the last frame written and skipping
// the write() when the new one is byte-for-byte identical (same content,
// same cursor position) fixes it without touching the redraw cadence itself.
// Lives for the whole session; editorFreeScreenCache() releases it at exit.
// In daemon mode each client window has its own: editorSwapScreenCache()
// trades this one for the one a client kept while another was drawn.
static ScreenCache g_prev = {NULL, 0, 0, NULL};

static int screenFrameChanged(const char *buf, int len) {
  if (len != g_prev.len) return 1;
  return len > 0 && memcmp(buf, g_prev.frame, (size_t) len) != 0;
}

static void screenFrameCache(const char *buf, int len) {
  char *grown;
  if (len > g_prev.cap) {
    grown = grealloc(g_prev.frame, (size_t) len);
    if (!grown) return; // best-effort: next frame just always looks "changed"
    g_prev.frame = grown;
    g_prev.cap = len;
  }
  memcpy(g_prev.frame, buf, (size_t) len);
  g_prev.len = len;
}

void editorFreeScreenCache(void) {
  screenDiffFree();
  gfree(g_prev.frame);
  g_prev.frame = NULL;
  g_prev.len = 0;
  g_prev.cap = 0;
}

void editorSwapScreenCache(ScreenCache *other) {
  char *frame = g_prev.frame;
  const int len = g_prev.len;
  const int cap = g_prev.cap;
  g_prev.frame = other->frame;
  g_prev.len = other->len;
  g_prev.cap = other->cap;
  other->frame = frame;
  other->len = len;
  other->cap = cap;
  screenDiffSwap(&other->diff);
}

/**
 * @brief Columns the inlay hints drawn before the cursor push it right by,
 * on the cursor's row of the active pane — the same hints editorDrawPane()
 * draws there.
 */
static int cursorInlayHintShift(const EditorPane *pane, const struct buffer_t *buf) {
  int at[64];
  const char *labels[64];
  Utf8Span span;
  int sel_from;
  int sel_to;
  int sel_extends;
  int n;
  int i;
  int w;
  int shift = 0;
  const int cursor_at = buf->x;
  int content_width;
  if (!buf->lsp_hints || buf->type != FILE_BUFF || buf->y >= buf->numrows)
    return 0;
  if (rowSelectionRange(buf, buf->y, &sel_from, &sel_to, &sel_extends))
    return 0;
  content_width = pane->width - editorGutterWidth(buf);
  utf8VisibleSpan(buf->row[buf->y].characters, buf->row[buf->y].size, pane->x_offset,
                  content_width, &span);
  n = rowInlayHints(buf, buf->y, &span, at, labels, 64);
  for (i = 0; i < n; i++) {
    // A hint goes before its character: the cursor on that character
    // sits after it.
    if (at[i] > cursor_at - span.start + span.lead_pad)
      break;
    lspUiClipToColumns(labels[i], (int) strlen(labels[i]), content_width, &w);
    shift += w;
  }
  return shift;
}

void editorRefreshScreen() {
  struct abuf ab = ABUF_INIT;
  char buf[32];
  EditorPane *active;
  struct buffer_t *buffer;
  const char *diag;
  char single_line[512];
  int i;
  int cursor_x;

  abAppend(&ab, HIDE_CURSOR, 6);
  abAppend(&ab, CURSOR_TOP_LEFT, 3);
  abAppend(&ab, E.theme.BACKGROUND_COLOR,
           (int)strlen(E.theme.BACKGROUND_COLOR));

  splitScreenSyncCursor();
  editorScroll();

  active = splitScreenGetActivePane();
  buffer = bufferFindById(active->buffer_id);

  editorDrawAllPanes(&ab);
  if (E.constantes.LSP) {

    // ── LSP: draw completion popup every frame while visible
    // ──────────────────

    // reset flags
    if (lspActiveClient()) {
      lspActiveClient()->completion_just_arrived = 0;
      lspActiveClient()->completion_requested = 0;
    }

    // ── LSP: diagnostic for current line in status bar
    // ────────────────────────
    diag = buffer ? lspUiDiagnosticAtCursor(&E.lsp_diagnostics,
                                                       active->buffer_id,
                                                       buffer->y)
                              : NULL;
    if (diag) {
      i = 0;

      // Copy until newline, \0, or screen width
      while (diag[i] && diag[i] != '\n' && i < E.screencols - 4) {
        single_line[i] = diag[i];
        i++;
      }

      // If message was truncated, add ellipsis
      if (diag[i] != '\0' && diag[i] != '\n') {
        single_line[i++] = '.';
        single_line[i++] = '.';
        single_line[i++] = '.';
      }
      single_line[i] = '\0';
      editorShowStatus("● %s", single_line);
    }
  }
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);
  if (E.constantes.LSP && lspActiveClient() &&
      lspActiveClient()->state == LSP_READY) {
    lspUiDrawInfo(&ab, &E.lsp_info);
    lspUiDrawCompletion(&ab, &E.lsp_completion);
  }
  bufferSwitcherDraw(&ab);
  listPopupDraw(&ab);
  fileFinderDraw(&ab);
  whichKeyDraw(&ab);

  // ── Position cursor (account for gutter width and inlay hints) ──────────────
  cursor_x = active->cursor_x;
  if (buffer && E.constantes.INLAY_HINTS) {
    cursor_x += cursorInlayHintShift(active, buffer);
    if (cursor_x > active->width - editorGutterWidth(buffer) - 1)
      cursor_x = active->width - editorGutterWidth(buffer) - 1;
  }
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
           active->cursor_y + active->origin_y + 1,
           cursor_x + active->origin_x + 1 + editorGutterWidth(buffer));
  abAppend(&ab, buf, (int)strlen(buf));
  abAppend(&ab, SHOW_CURSOR, 6);

  if (screenFrameChanged(ab.b, ab.len)) {
    // Outside the frame, which screenDiffWrite() only writes the changed
    // rows of. Repeated with each frame: harmless, and how a new daemon
    // window or a changed MOUSE setting gets it.
    screenWrite(E.constantes.MOUSE ? MOUSE_ON_SEQ : MOUSE_OFF_SEQ, MOUSE_SEQ_LEN);
    screenWrite(E.constantes.EXTENDED_KEYS ? EXTENDED_KEYS_ON_SEQ : EXTENDED_KEYS_OFF_SEQ,
                EXTENDED_KEYS_SEQ_LEN);
    screenDiffWrite(ab.b, ab.len, E.screenrows + 2, E.screencols);
    screenFrameCache(ab.b, ab.len);
  }
  abFree(&ab);
}
