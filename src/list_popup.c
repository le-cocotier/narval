/**
 * @file list_popup.c
 * @brief Generic modal list picker driven entirely from Lisp via
 * (editor-list-popup title items on-select on-cancel filterable?).
 * @details A single global instance (E.list_popup), same shape as the
 * CTRL-x b buffer switcher (buffer_switcher.c) and the LSP completion
 * popup (lsp_ui.c), but with no notion of buffers or completion items —
 * just (label . value) pairs supplied by whichever Lisp package opened it.
 * This is the primitive packages like the outline viewer build on instead
 * of every feature growing its own bespoke C popup — and, with the
 * optional `filterable?` argument, the same primitive for "type to
 * narrow this list" popups too, not just modal pick-with-arrows ones.
 */

#include "../include/list_popup.h"
#include "../include/define.h"
#include "../include/popup_ui.h"
#include "../include/galloc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern EditorConfig E;

static int isWordStart(const char *label, const char *p) {
  return p == label || p[-1] == '-' || p[-1] == ' ' || p[-1] == '/' || p[-1] == '_' ||
         p[-1] == '.';
}

/**
 * @brief How well `label` matches `query` with the letters of the query in
 * order (fuzzy): lower is better, -1 for no match.
 * @details Only the part before a tab counts (the rest is a second column,
 * e.g. the keys in the command palette). Each start of the first letter is
 * tried, matching the rest as early as possible; the tightest span wins, and
 * letters that start a word ("e-s" in editor-save) earn a bonus, so "sv"
 * puts editor-save before dashboard-activate.
 */
static int fuzzyScore(const char *label, const char *query) {
  const char *end = strchr(label, '\t');
  const char *s;
  const char *p;
  const char *q;
  int best = -1;
  int bonus;
  int score;
  if (!end)
    end = label + strlen(label);
  for (s = label; s < end; s++) {
    if (tolower((unsigned char)*s) != tolower((unsigned char)query[0]))
      continue;
    bonus = isWordStart(label, s);
    for (p = s + 1, q = query + 1; p < end && *q; p++)
      if (tolower((unsigned char)*p) == tolower((unsigned char)*q)) {
        bonus += isWordStart(label, p);
        q++;
      }
    if (*q)
      break; // no match from here on either
    score = 2 * (int)(p - s) - 3 * bonus + 4; // >= 0: span >= 1 + the bonus
    if (score < 0)
      score = 0;
    if (best < 0 || score < best)
      best = score;
  }
  return best;
}

/**
 * @brief Case-insensitive substring test: does `label` contain `query`
 * anywhere? An empty query always matches (so an untouched filterable
 * popup opens showing everything, same order as passed in).
 */
static int labelMatchesQuery(const char *label, const char *query) {
  const char *p;
  const char *a;
  const char *b;
  if (!query[0])
    return 1;

  for (p = label; *p; p++) {
    a = p;
    b = query;
    while (*a && *b &&
           tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
      a++;
      b++;
    }
    if (!*b)
      return 1; // walked off the end of query -> matched in full
  }
  return 0;
}

typedef struct {
  Lisp pair;
  int score;
  int index;
} RankedItem;

static int compareRanked(const void *a, const void *b) {
  const RankedItem *x = a;
  const RankedItem *y = b;
  if (x->score != y->score)
    return x->score - y->score;
  return x->index - y->index; // ties keep the order they were passed in
}

/**
 * @brief Rebuilds E.list_popup.items/count from all_items + query.
 * @details Only meaningful when filterable — non-filterable popups never
 * call this, items IS all_items for their whole lifetime. A substring
 * filter keeps the order of all_items; a fuzzy one sorts the matches best
 * first (fuzzyScore), ties in the order passed in.
 */
static void listPopupRebuildFilter(void) {
  Lisp head = lisp_null();
  Lisp tail = lisp_null();
  int count = 0;
  int total = 0;
  int score;
  int i;
  Lisp it;
  Lisp pair;
  Lisp cell;
  RankedItem *ranked = NULL;
  const int fuzzy = E.list_popup.filterable == LIST_FILTER_FUZZY && E.list_popup.query[0];

  if (fuzzy) {
    for (it = E.list_popup.all_items; lisp_is_pair(it); it = lisp_cdr(it))
      total++;
    ranked = gcalloc((size_t)(total > 0 ? total : 1), sizeof(RankedItem));
  }

  for (it = E.list_popup.all_items; lisp_is_pair(it); it = lisp_cdr(it)) {
    pair = lisp_car(it);
    if (!lisp_is_pair(pair))
      continue;
    if (ranked) {
      score = fuzzyScore(lisp_string(lisp_car(pair)), E.list_popup.query);
      if (score >= 0) {
        ranked[count].pair = pair;
        ranked[count].score = score;
        ranked[count].index = count;
        count++;
      }
      continue;
    }
    if (!labelMatchesQuery(lisp_string(lisp_car(pair)), E.list_popup.query))
      continue;

    cell = lisp_cons(pair, lisp_null(), E.ctx);
    if (lisp_is_null(head))
      head = cell;
    else
      lisp_set_cdr(tail, cell);
    tail = cell;
    count++;
  }

  if (ranked) {
    qsort(ranked, (size_t)count, sizeof(RankedItem), compareRanked);
    for (i = count - 1; i >= 0; i--)
      head = lisp_cons(ranked[i].pair, head, E.ctx);
    gfree(ranked);
  }

  E.list_popup.items = head;
  E.list_popup.count = count;
  if (E.list_popup.selected >= count)
    E.list_popup.selected = count > 0 ? count - 1 : 0;
  if (E.list_popup.selected < 0)
    E.list_popup.selected = 0;
}

void listPopupOpen(const char *title, const Lisp items, const Lisp on_select,
                   const Lisp on_cancel, const int filterable) {
  strncpy(E.list_popup.title, title, sizeof(E.list_popup.title) - 1);
  E.list_popup.title[sizeof(E.list_popup.title) - 1] = '\0';
  E.list_popup.all_items = items;
  E.list_popup.selected = 0;
  E.list_popup.on_select = on_select;
  E.list_popup.on_cancel = on_cancel;
  E.list_popup.on_move = lisp_null();
  E.list_popup.filterable = filterable;
  E.list_popup.query[0] = '\0';
  E.list_popup.query_len = 0;

  if (filterable) {
    listPopupRebuildFilter(); // query is empty here: matches everything, in
                              // order
  } else {
    E.list_popup.items = items;
    E.list_popup.count = lisp_list_length(items);
  }

  // A non-filterable popup with nothing to show has nothing to
  // navigate either — leave it closed rather than drawing an empty
  // box. A filterable one opens even with zero initial items: an empty
  // items list plus a query the caller pre-populated some other way
  // isn't something this builtin does today, but there's no reason to
  // special-case "started empty" out from under a query that could
  // still be typed into productively... in practice this is always
  // non-empty in the filterable case too, since callers pass real data.
  E.list_popup.visible = filterable || E.list_popup.count > 0;
}

/** @brief Calls on_move with the highlighted item's value, if both exist. */
static void listPopupNotifyMove(void) {
  LispError err;
  Lisp item;
  if (!E.list_popup.visible || lisp_is_null(E.list_popup.on_move) || E.list_popup.count == 0)
    return;
  item = lisp_list_ref(E.list_popup.items, E.list_popup.selected);
  if (!lisp_is_pair(item))
    return;
  err = LISP_ERROR_NONE;
  lisp_apply(E.list_popup.on_move, lisp_cons(lisp_cdr(item), lisp_null(), E.ctx), &err, E.ctx);
}

void listPopupSetOnMove(const Lisp on_move) {
  E.list_popup.on_move = on_move;
  listPopupNotifyMove();
}

int listPopupHandleKey(const Utf8Char *key) {
  unsigned char c;
  int is_single;
  LispError err;
  Lisp item;
  Lisp value;
  int is_text;
  int n;
  if (!E.list_popup.visible)
    return 0;

  c = key->character[0];
  is_single = (key->length == 1);

  if (is_single && c == '\x1b') { // ESC — cancel
    E.list_popup.visible = 0;
    if (!lisp_is_null(E.list_popup.on_cancel)) {
      err = LISP_ERROR_NONE;
      lisp_apply(E.list_popup.on_cancel, lisp_null(), &err, E.ctx);
    }
    return 1;
  }
  if (is_single && (c == ARROW_DOWN || c == CTRL_KEY('n'))) {
    if (E.list_popup.selected < E.list_popup.count - 1) {
      E.list_popup.selected++;
      listPopupNotifyMove();
    }
    return 1;
  }
  if (is_single && (c == ARROW_UP || c == CTRL_KEY('p'))) {
    if (E.list_popup.selected > 0) {
      E.list_popup.selected--;
      listPopupNotifyMove();
    }
    return 1;
  }
  if (is_single && c == '\r') { // ENTER — select the highlighted item
    if (E.list_popup.count == 0)
      return 1; // filterable + query matches nothing: nothing to select
    item = lisp_list_ref(E.list_popup.items, E.list_popup.selected);
    value = lisp_is_pair(item) ? lisp_cdr(item) : lisp_null();
    E.list_popup.visible = 0;
    if (!lisp_is_null(E.list_popup.on_select)) {
      err = LISP_ERROR_NONE;
      lisp_apply(E.list_popup.on_select, lisp_cons(value, lisp_null(), E.ctx),
                 &err, E.ctx);
    }
    return 1;
  }

  if (E.list_popup.filterable) {
    if (is_single && (c == BACKSPACE || c == DEL_KEY)) {
      if (E.list_popup.query_len > 0) {
        // Step back over a whole UTF-8 char: drop trailing
        // continuation bytes (0b10xxxxxx), then the lead byte.
        E.list_popup.query_len--;
        while (E.list_popup.query_len > 0 &&
               (E.list_popup.query[E.list_popup.query_len] & 0xC0) == 0x80)
          E.list_popup.query_len--;
        E.list_popup.query[E.list_popup.query_len] = '\0';
        E.list_popup.selected = 0;
        listPopupRebuildFilter();
        listPopupNotifyMove();
      }
      return 1;
    }

    // Typed text — printable ASCII, or any multi-byte UTF-8 char.
    is_text = (is_single && c >= 0x20 && c <= 0x7E) || key->length > 1;
    if (is_text) {
      n = (unsigned char)key->length;
      if (E.list_popup.query_len + n < (int)sizeof(E.list_popup.query) - 1) {
        memcpy(E.list_popup.query + E.list_popup.query_len, key->character,
               (size_t)n);
        E.list_popup.query_len += n;
        E.list_popup.query[E.list_popup.query_len] = '\0';
        E.list_popup.selected = 0;
        listPopupRebuildFilter();
        listPopupNotifyMove();
      }
      return 1;
    }
  }

  // Modal: swallow anything else so it doesn't fall through into the
  // buffer behind it.
  return 1;
}

void listPopupDraw(struct abuf *ab) {
  int count;
  int visible_count;
  int scroll;
  char title[192];
  int content_w;
  Lisp it;
  int i;
  Lisp pair;
  int label_len;
  int box_w;
  int rows;
  int box_h;
  int origin_x;
  int origin_y;
  PopupStyle style;
  char empty[LIST_POPUP_MAX_WIDTH + 1];
  int idx;
  int is_sel;
  int row;
  const char *label;
  const char *tab;
  char left[LIST_POPUP_MAX_WIDTH + 1];
  int left_w;
  int right_w;
  int len;
  const PopupMatch match = E.list_popup.filterable == LIST_FILTER_FUZZY ? POPUP_MATCH_FUZZY
                                                                        : POPUP_MATCH_SUBSTRING;
  if (!E.list_popup.visible)
    return;
  // Non-filterable popups never open with count == 0 (see
  // listPopupOpen()); a filterable one legitimately can once a query
  // matches nothing, and still needs to draw — an empty box with
  // "no matches" instead of vanishing until the query changes again.

  count = E.list_popup.count;
  visible_count = count < LIST_POPUP_MAX_H ? count : LIST_POPUP_MAX_H;

  // Scroll window so the selected item is always visible.
  scroll = 0;
  if (E.list_popup.selected >= LIST_POPUP_MAX_H)
    scroll = E.list_popup.selected - LIST_POPUP_MAX_H + 1;

  // Title row: plain title, or "title: query_  (N)" while filterable —
  // same shape find-file's own popup uses, for a consistent feel.
  if (E.list_popup.filterable)
    if (E.list_popup.selected > 0) // "(3/14)": where you are in a long list
      snprintf(title, sizeof(title), "%s: %s_  (%d/%d)", E.list_popup.title,
               E.list_popup.query, E.list_popup.selected + 1, count);
    else
      snprintf(title, sizeof(title), "%s: %s_  (%d)", E.list_popup.title,
               E.list_popup.query, count);
  else
    snprintf(title, sizeof(title), "%s", E.list_popup.title);

  // Size the box to the longest visible label (or the title, if wider).
  // A label "left\tright" has two columns: each measured on its own.
  content_w = (int)strlen(title);
  left_w = 0;
  right_w = 0;
  it = lisp_list_advance(E.list_popup.items, scroll);
  for (i = 0; i < visible_count && lisp_is_pair(it); i++, it = lisp_cdr(it)) {
    pair = lisp_car(it);
    if (!lisp_is_pair(pair))
      continue;
    label = lisp_string(lisp_car(pair));
    tab = strchr(label, '\t');
    label_len = tab ? (int)(tab - label) : (int)strlen(label);
    if (label_len > left_w)
      left_w = label_len;
    if (tab && (int)strlen(tab + 1) > right_w)
      right_w = (int)strlen(tab + 1);
  }
  if (left_w + (right_w ? right_w + 3 : 0) > content_w)
    content_w = left_w + (right_w ? right_w + 3 : 0);
  if (content_w > E.screencols - 6)
    content_w = E.screencols - 6;
  if (content_w > LIST_POPUP_MAX_WIDTH)
    content_w = LIST_POPUP_MAX_WIDTH;
  if (content_w < 10)
    content_w = 10;

  box_w = content_w + 4; // border(2) + marker/margin(1) + margin(1)
  // At least one content row, so a filterable popup with no current
  // matches still draws a box (for its "no matches" row) instead of
  // collapsing to nothing.
  rows = visible_count > 0 ? visible_count : 1;
  box_h = rows + 4; // top border, title, separator, rows, bottom border

  popupCenter(box_w, box_h, &origin_x, &origin_y);

  popupStyleInit(&style);

  popupDrawTop(ab, &style, origin_x, origin_y, box_w);
  popupDrawTitleRow(ab, &style, origin_x, origin_y + 1, box_w, title,
                    content_w);
  popupDrawSeparator(ab, &style, origin_x, origin_y + 2, box_w);

  if (visible_count == 0) {
    popupRowBegin(ab, &style, origin_x, origin_y + 3, 0);
    snprintf(empty, sizeof(empty), "%-*s", content_w, "  (no matches)");
    empty[content_w] = '\0';
    abAppend(ab, empty, content_w);
    popupRowEnd(ab, &style);
  }

  it = lisp_list_advance(E.list_popup.items, scroll);
  for (i = 0; i < visible_count; i++) {
    idx = i + scroll;
    is_sel = (idx == E.list_popup.selected);
    row = origin_y + 3 + i;

    popupRowBegin(ab, &style, origin_x, row, is_sel);

    label = (const char *)"";
    if (lisp_is_pair(it)) {
      pair = lisp_car(it);
      if (lisp_is_pair(pair))
        label = lisp_string(lisp_car(pair));
    }
    tab = strchr(label, '\t');
    if (tab && right_w > 0 && content_w > right_w + 3) {
      len = (int)(tab - label) < (int)sizeof(left) - 1 ? (int)(tab - label) : (int)sizeof(left) - 1;
      memcpy(left, label, (size_t)len);
      left[len] = '\0';
      popupAppendLabel(ab, &style, is_sel, left, content_w - right_w - 3,
                       E.list_popup.filterable ? E.list_popup.query : NULL, match, 0);
      abAppend(ab, "   ", 3);
      popupAppendLabel(ab, &style, is_sel, tab + 1, right_w, NULL, POPUP_MATCH_NONE,
                       POPUP_LABEL_DIM);
    } else {
      popupAppendLabel(ab, &style, is_sel, label, content_w,
                       E.list_popup.filterable ? E.list_popup.query : NULL, match, 0);
    }

    popupRowEndScroll(ab, &style, i, visible_count, count, scroll);

    if (lisp_is_pair(it))
      it = lisp_cdr(it);
  }

  popupDrawBottom(ab, &style, origin_x, origin_y + 3 + rows, box_w);
  popupDrawShadow(ab, &style, origin_x, origin_y, box_w, box_h);
}
