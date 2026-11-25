#ifndef MEMORY_INSPECTOR_H_
#define MEMORY_INSPECTOR_H_

// ─── Memory inspector ───────────────────────────────────────────────────────
// Debug-only (APP_DEBUG) view of every block currently tracked by gutils
// (galloc/grealloc). Results live in an ordinary read-only scratch buffer
// ("*memory*"), same as project search and the explorer sidebar, so it
// scrolls and splits like any other buffer.

// Opens (or reuses) the "*memory*" buffer, fills it with a fresh snapshot
// of every tracked block, and shows it in the active pane.
void memoryInspectorOpen(void);

// Re-renders the snapshot into the already-open "*memory*" buffer without
// changing pane focus. No-op if the buffer isn't open.
void memoryInspectorRefresh(void);

// Cycles the display sort order (size, largest first -> count -> recency ->
// file:line/func location -> back to size) and re-renders. The underlying
// gutils list itself is never reordered.
void memoryInspectorCycleSort(void);

// Switches between one row per allocation site (the default) and one row
// per block.
void memoryInspectorToggleView(void);

// Marks a baseline: from then on the sites view shows only the sites whose
// blocks or bytes changed since, with the change, and the blocks view only
// the blocks allocated since and still alive.
void memoryInspectorMarkBaseline(void);

// Drops the baseline.
void memoryInspectorClearBaseline(void);

// Opens the source line that allocated the block or site under the cursor,
// in the other pane when the screen is split.
void memoryInspectorGoto(void);

// Prompts for a file path (ESC cancels) and writes the current view to it
// as plain text.
void memoryInspectorExport(void);

#endif
