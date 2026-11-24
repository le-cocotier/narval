#ifndef NARVAL_LIST_POPUP_H
#define NARVAL_LIST_POPUP_H

#include "data.h"
#include "append_buffer.h"

// ListPopup.filterable: off, the label contains the query, or the label
// holds the query's letters in order.
#define LIST_FILTER_SUBSTRING 1
#define LIST_FILTER_FUZZY 2

// Opens the popup with `title`, a proper Lisp list of (label . value)
// pairs, and callbacks invoked on selection/cancellation. `on_select` is
// called with the chosen pair's cdr as its single argument; `on_cancel`
// (may be lisp_null()) is called with no arguments if the user presses
// ESC instead. Does nothing if `items` is empty.
//
// `filterable`: when true, printable keys and BACKSPACE edit a live query
// shown in the title bar, and the visible/navigable list narrows to
// pairs whose label contains it (case-insensitive substring) — the arrow
// keys, ENTER and ESC all still act on the filtered view. When false,
// behaves exactly as before (every key not already bound above is
// swallowed, not typed into a query).
void listPopupOpen(const char *title, Lisp items, Lisp on_select, Lisp on_cancel, int filterable);
// Calls `on_move` with the highlighted item's value now and each time the
// highlight moves to another item (arrows, typing), until the popup
// closes. For a live preview: the theme picker applies each theme as you
// go. Call right after listPopupOpen; opening a popup clears it.
void listPopupSetOnMove(Lisp on_move);

// Input handling — returns 1 if the key was consumed by the popup.
// Call this before normal keybind dispatch while the popup is visible.
int listPopupHandleKey(const Utf8Char *key);

// Call from editorRefreshScreen() — appends the popup to abuf when visible.
void listPopupDraw(struct abuf *ab);

#endif //NARVAL_LIST_POPUP_H
